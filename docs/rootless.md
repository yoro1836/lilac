# Rootless engine: Waydroid images on tawcroot

Wayondroid today needs root: `libvine_helper.so` runs under `su` because
`unshare`, `mount` and loop devices need `CAP_SYS_ADMIN`. This document is
the structure for the no-root path (roadmap Phase 4), which replaces the
planned `proot` fallback.

The idea: boot an Android image with **tawcroot** (`tawcroot/`, the
seccomp/SIGSYS rootless runtime imported from TAWC and developed here), emulate
binder in userspace, and draw the guest through a small Wayland compositor
inside the Wayondroid app. The guest ROM is a **Waydroid** LineageOS image, which
is already built for containers.

Rule of thumb: kernel interfaces are emulated in tawcroot (works for any
container image, redroid included). Android policy problems are patched in
our own ROM build, only where emulation would be absurd.

## Why Waydroid images

Checked against `lineage-20.0-20260927-VANILLA-waydroid_arm64-system.zip` and
`lineage-20.0-20261022-MAINLINE-waydroid_arm64-vendor.zip` from
`ota.waydro.id` (the OTA JSON lists a sha256 per file).

- Android 13, userdebug, security patch 2026-02, redistributable (LineageOS).
  Raw ext4: system 1.9 GB, vendor 0.4 GB.
- System-as-root: no ramdisk, the system image root is `/`.
- **Flattened APEX** (`/system/apex/<name>/` directories): no apexd
  loop/dm-verity.
- `hwcomposer.waydroid` is a Wayland client with `wl_shm`, dmabuf and
  `android_wlegl` buffer paths, and an optional one-window-per-task mode.
- Input needs no uinput: the HWC writes Wayland input into
  `/dev/input/wl_*_events` for InputFlinger.
- Host config is one file, `/vendor/waydroid.prop` (`sys.use_memfd=true`
  removes the need for ashmem).
- Caveat: `ro.zygote=zygote64_32`. tawcroot has no AArch32, so the
  rootless engine sets `ro.zygote=zygote64` (64-bit apps only) until our
  own 64-bit-only build exists.

## Process layout (rootless)

```
Wayondroid app process (app uid)
├─ Kotlin UI, VineVMManager, HelperClient
├─ libvine_runtime.so: DisplayBridge draws <instance>/shared/vinefb
└─ libvine_helper.so --rootless                      (same line protocol, no su)
     ├─ per instance: rootfs preparation (a child, once per boot)
     ├─ binderd: binder / hwbinder / vndbinder
     ├─ vine_wl: Wayland server on run/wayland-0 ──► shared/vinefb,
     │    input lines on <instance>/rootless/input
     ├─ vine_pulse: PulseAudio server on run/pulse/native
     └─ keeper (subreaper of every guest process)
          └─ tawcroot --init --all-caps --cgroup --owners -r <rootfs> … -- /system/bin/init second_stage
               └─ servicemanager, hwservicemanager, surfaceflinger,
                  composer HAL (hwcomposer.waydroid) ──► wayland-0
                  zygote64 → system_server → apps
```

The rootless engine (`helper/rootless_server.cpp`,
`rootless/rootless_instance.cpp`) is a second implementation of the
helper protocol. `VineVMManager` keeps one `HelperClient` per engine:
instances whose ROM has no `ramdisk.img` (system-as-root Waydroid
images, `package_vrom.py --kind waydroid`) go to
`libvine_helper.so --rootless`, started directly as the app; the others
to the root helper under `su`.

How the protocol maps:

| Command | Rootless engine |
|---|---|
| `start` | prepares the rootfs in a child (extraction on first boot, [rootless-boot.md](rootless-boot.md)), then starts binderd, vine_wl, vine_pulse and init; `running` once init's rc writes `/run/user/1000/boot_completed` |
| `stop`, `kill` | SIGTERM to the keeper, which kills its whole tree (zygotes and apps included), then the servers |
| `pause`, `resume` | SIGSTOP/SIGCONT to every process below the keeper |
| `agent_send` | the line goes to vine_wl's input socket; keys without an evdev code (APP_SWITCH) go through the guest's `input keyevent` |
| `agent ping` | answers ok (input works); other agent requests fail: there is no guest agent |
| `exec` | `tawcroot -r <rootfs> --owners <binds> -- /system/bin/sh -c 'exec "$@"'`, environment from `init.environ.rc` |
| `apps` | `cmd package query-activities` for launcher activities; labels are package names |
| `launch` | `cmd package resolve-activity`, then `cmd activity start-activity` |
| `icon`, snapshots | not supported yet |

Per instance, under `<instance>/rootless/`: `rootfs/` (the extracted
images, `rootfs.stamp` records which), `tawcroot/` (the store and
ownership table), `data/` (the guest's `/data`), `binderfs/`, `run/`
(`/run/user/1000`), `input`, `logs/` (`prepare`, `init`, `binderd`,
`vine_wl`, `vine_pulse`; `diag` shows their tails, kmsg and logcat).
A changed system or vendor image is extracted again, and `/data` and the
ownership table start over with it. The images stay in the instance dir
next to the rootfs (about twice the ROM's size on disk).

## Repository layout

| Path | Contents |
|---|---|
| `tawcroot/` | the runtime (MIT, imported from TAWC). Host tests: `tawcroot/test.sh --host`. Design docs: `docs/tawcroot/` |
| `tawcroot/src/binder.c`, `tawcroot/include/binder_wire.h`, `tawcroot/binderd/` | binder emulation (below) |
| `app/src/main/cpp/rootless/` | rootless backend: instance preparation, process supervision, `waydroid.prop` and `/proc/cmdline` generation, read-only ext4 extractor (`.vrom` images → rootfs directory) |
| `app/src/main/cpp/helper/` | `helper_main` gains `--rootless`; `HelperServer` dispatches to the root `Container` or the rootless engine |
| `app/src/main/rust/vine_compositor/` | minimal Smithay compositor (xdg-shell, `wl_shm`, `wl_seat` touch/keyboard, later `android_wlegl`), derived from TAWC's `compositor/` (MIT) without its launcher, Xwayland, ando and remote parts |
| `app/src/main/java/.../helper/HelperLauncher.kt` | root or rootless spawn |
| `app/src/main/java/.../domain/` | engine choice: rootless by default, root when available and asked for |
| `rom/waydroid/` | guest overlay: prop template (`ro.zygote=zygote64`, `sys.use_memfd=true`, GPU props), init rc that starts the vine agent |
| `rom/build/` (later) | `local_manifests` patches over Waydroid's tree, Hetzner build scripts |
| `rom/package_vrom.py --kind waydroid` | Waydroid's system.img + vendor.img (unzipped from the OTA zips) → `.vrom`, no ramdisk |

tawcroot also gets the Android device and `/proc`/`/sys` emulation, a
nested-seccomp no-op for zygote. The ownership store is done (`--owners`,
below).

Gradle runs `tawcroot/build.sh` (NDK) from `preBuild` and packages
`libtawcroot.so` (later `libbinderd.so`) as jniLibs, the same extractor
trick `libvine_helper.so` uses. tawcroot only supports `arm64-v8a`
and `x86_64`, so the rootless engine is unavailable on the 32-bit ABIs.

## Binder emulation

Implemented (milestone 1): `tawcroot/binderd/binderd.c` (the driver),
`tawcroot/src/binder.c` (the in-handler shim), wire protocol in
`tawcroot/include/binder_wire.h`. Enabled by binding binderd's directory
to `/dev/binderfs` (`-b <dir>:/dev/binderfs`); it holds the `binder`,
`hwbinder` and `vndbinder` sockets and the `features/` files libbinder reads.

- `open("/dev/{binder,hwbinder,vndbinder}")` (or `/dev/binderfs/<name>`)
  connects to binderd. The guest's binder fd **is** that SOCK_SEQPACKET
  process connection, at fd ≥ 1000, so poll/epoll on it works (binderd
  keeps a `BW_POLL` byte queued while the process has work) and its
  `close` traps.
- `mmap` of fds ≥ 1000 traps (a new seccomp leaf; anonymous maps and
  ordinary files stay inline). For a binder fd it maps the receive buffer
  memfd read-only and shared; binderd writes transactions straight into it.
- Each guest thread gets its own connection and a staging memfd shared with
  binderd, which carries requests (BC bytes, then per transaction its data,
  offsets and PTR buffers) and responses (BR bytes, fd fixups). Fds travel
  via `SCM_RIGHTS` and land CLOEXEC, as the kernel installs them.
- A blocked read waits on {thread socket, signalfd(guest's unblocked
  signals)}. A pending signal sends `BW_CANCEL`; the ioctl returns `-EINTR`
  with `write_consumed` set, and libbinder's `talkWithDriver` retries.
- Sender pid/euid come in-band from tawcroot's identity and `--init` pid
  emulation.
- binderd follows `drivers/android/binder.c`: node/ref refcounting with
  BR_INCREFS/ACQUIRE/RELEASE/DECREFS, deferred TRANSACTION_COMPLETE for sync
  calls, nested-call routing through transaction stacks, per-node oneway
  queues, death notifications, BINDER/HANDLE/FD/FDA/PTR objects,
  `BR_TRANSACTION_SEC_CTX` (`--secctx`, default `u:r:untrusted_app:s0`),
  BR_SPAWN_LOOPER, `SET_CONTEXT_MGR(_EXT)`, `GET_NODE_INFO_FOR_REF`,
  `GET_NODE_DEBUG_INFO`, `GET_EXTENDED_ERROR`.
- Not implemented: priority inheritance, freezing (`BINDER_FREEZE` is
  `-EINVAL`), oneway spam detection, async space accounting.
- Tests: `tawcroot/test.sh --host 'prod_binder.*'` runs two guest processes
  (`tests/integration/programs/static_binder_check.c`) against an
  ASan/UBSan binderd: secctx, epoll, oneway scatter-gather with an FDA, fd
  passing, binder→handle translation, nested calls, death notification, and
  the manager dying mid nested call.
- To do: the helper's host API (register the `waydroid*` services, call
  `waydroidplatform`), and running real libbinder (servicemanager) on a
  device.

## Ownership store

`tawcroot --owners` (`tawcroot/src/owners.c`, layout in
`tawcroot/include/owners_table.h`). The host app uid owns every file, so
without it a guest that switches uids sees no ownership at all.

- `<store>/owners` (the store is `<rootfs>/../tawcroot`) is a 24 MiB sparse
  open-addressing table, (st_dev, st_ino) → (uid, gid), mapped shared by
  every guest process. stat reads it without a syscall; writers flock it.
- No entry means root:root. Files a non-root guest creates (open O_CREAT,
  mkdir, mknod, symlink) belong to its fsuid/fsgid. chown follows the
  kernel's rules for the virtual identity: any change at euid 0, otherwise
  only the owner changing the group to one of its own.
- Removing an inode's last name (unlink, rmdir, rename over it) drops its
  entry, so a reused inode number starts as root again.
- A session flag (`TAWC_NS_OWNERS`): inherited across fork and exec, off by
  default (TAWC-style distro sessions keep today's behaviour).
- Not done: setgid-directory group inheritance, permission checks (see
  risks), table growth past 786k live entries (writes then fail with
  ENOSPC and the files read as root).
- Test: `tawcroot/test.sh --host 'prod_owners.*'`.

## ROMs

One `.vrom` serves both engines: the root engine loop-mounts the images,
the rootless engine extracts them once per ROM into a shared read-only
rootfs directory, with a per-instance writable `data` dir.

- `ROMImage` gains a `kind` (`vine`, `waydroid`).
- The root engine needs system-as-root support (it currently requires
  `ramdisk.img`, `container/namespace_manager.cpp:593`).
- The extractor is read-only ext4 (extents, linear and htree directories,
  fast/slow symlinks, mode/uid/gid). Non-root owners go straight into the
  ownership table (`tawcroot/include/owners_table.h`).

## ROM builds

Only when a ROM patch is needed. Hetzner Cloud, billed hourly: create a
CPX51-class server (16 vCPU, 32 GB RAM, 360 GB disk; add swap), sync
Waydroid's LineageOS tree with our `local_manifests`, build, pull the
images, delete the server. Deleting is what stops billing, so the script
does it. Keep a snapshot only while iterating often.

## Boot flow (rootless)

1. Prepare: rootfs extracted, `/vendor/waydroid.prop` written, synthesized
   `/proc/cmdline`, `<instance>/data`, `<instance>/run/`.
2. The app starts the compositor on `<instance>/run/wayland-0`.
3. The helper starts binderd, then
   `tawcroot --init --all-caps --cgroup --owners -r <rootfs> -b <instance>/data:/data -b <instance>/run:/run/user/1000 -- /init`
   (rootfs preparation and binds: [rootless-boot.md](rootless-boot.md)).
4. The vine agent reports boot completion, apps, icons, as on the root path.

## Milestones

1. ~~binderd + tawcroot shim, host tests: transaction/reply,
   refcounts, fd passing, death notification, scatter-gather.~~ Done.
2. ~~On device: servicemanager + hwservicemanager + `service list` under
   tawcroot.~~ Done.
3. ~~Compositor in the app, surfaceflinger + composer HAL → first
   frame.~~ Done: `vine_wl` serves Waydroid's hwcomposer, the guest boots
   to its lock screen ([rootless-boot.md](rootless-boot.md)).
4. system_server, launcher, input; freeze the ROM patch list. Mostly
   there: system_server, SystemUI and the launcher run and injected touch
   and keys work; binderd stability remains. No ROM patches so far, only
   rootfs preparation ([rootless-boot.md](rootless-boot.md)).
5. Rootless engine behind the helper protocol, `.vrom` extraction, UI
   switch. In progress: the engine and the app's engine choice are done
   and host tested (`tests/native/run_rootless_tests.sh`); icons, app
   labels and snapshots are not.
6. Own 64-bit-only ROM build if milestone 4 needs patches.

## Risks

- **Phantom process killer**: Android 12+ kills app child processes past
  32, system-wide. A booted guest runs 60+. Likely needs the user to turn
  it off (12L+: `adb shell settings put global settings_enable_monitor_phantom_procs false`).
  Blocking for real use.
- **File ownership**: done for stat/chown (`--owners`), but permission
  checks are not emulated: the kernel sees the app uid on every file, so an
  app uid can read another app's 0700 files. Nothing in Android's boot
  should depend on being denied; app isolation inside the guest is not a
  security boundary.
- Binder cost: every call is SIGSYS + socket round trips + two copies.
  Fallback: patch libbinder in our ROM to talk to binderd directly.
- Networking: no netns rootless, and ConnectivityService wants a
  netd-managed default network. Probably a ROM patch.
- FUSE storage: `/dev/fuse` cannot be mounted rootless. Probably a ROM patch.
- 16 KB page kernels: the guest's 4 KB-aligned ELFs will not load.
- Memory: a booted Android 13 needs roughly 1.5–2 GB.
