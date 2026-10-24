# Rootless engine: Waydroid images on tawcroot

VineOS today needs root: `libvine_helper.so` runs under `su` because
`unshare`, `mount` and loop devices need `CAP_SYS_ADMIN`. This document is
the structure for the no-root path (roadmap Phase 4), which replaces the
planned `proot` fallback.

The idea: boot an Android image with **tawcroot**, the seccomp/SIGSYS
rootless runtime from [TAWC](https://github.com/yoro1836/tawc), emulate
binder in userspace, and draw the guest through a small Wayland compositor
inside the VineOS app. The guest ROM is a **Waydroid** LineageOS image, which
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
VineOS app process (app uid)
├─ Kotlin UI, VineVMManager, HelperClient           (unchanged)
├─ libvine_runtime.so
│    └─ vine compositor: Wayland server on <instance>/run/wayland-0,
│       draws into the VMDisplayScreen Surface, feeds touch/keys
└─ libvine_helper.so --rootless                      (same line protocol, no su)
     ├─ binderd (one per instance): binder / hwbinder / vndbinder
     └─ tawcroot --init --cgroup -r <rootfs> … -- /init
          └─ servicemanager, hwservicemanager, surfaceflinger,
             composer HAL (hwcomposer.waydroid) ──► wayland-0
             zygote64 → system_server → apps, vine agent
```

`HelperClient` already takes a `spawn: () -> Process`. The rootless engine
is a second implementation behind the same tab-separated protocol
(`start`, `stop`, `status`, `apps`, `launch`, `icon`, …), so
`VineVMManager` and the UI do not change. Only `HelperLauncher` picks
`su -c libvine_helper.so` or a direct exec of
`libvine_helper.so --rootless`.

## Repository layout

| Path | Contents |
|---|---|
| `deps/tawc` | git submodule, pinned commit of yoro1836/tawc. Provides tawcroot and binderd. |
| `app/src/main/cpp/rootless/` | rootless backend: instance preparation, process supervision, `waydroid.prop` and `/proc/cmdline` generation, read-only ext4 extractor (`.vrom` images → rootfs directory) |
| `app/src/main/cpp/helper/` | `helper_main` gains `--rootless`; `HelperServer` dispatches to the root `Container` or the rootless engine |
| `app/src/main/rust/vine_compositor/` | minimal Smithay compositor (xdg-shell, `wl_shm`, `wl_seat` touch/keyboard, later `android_wlegl`), derived from TAWC's `compositor/` (MIT) without its launcher, Xwayland, ando and remote parts |
| `app/src/main/java/.../helper/HelperLauncher.kt` | root or rootless spawn |
| `app/src/main/java/.../domain/` | engine choice: rootless by default, root when available and asked for |
| `rom/waydroid/` | guest overlay: prop template (`ro.zygote=zygote64`, `sys.use_memfd=true`, GPU props), init rc that starts the vine agent |
| `rom/build/` (later) | `local_manifests` patches over Waydroid's tree, Hetzner build scripts |
| `scripts/waydroid_to_vrom.py` | Waydroid OTA zips → `.vrom` (system.img + vendor.img, no ramdisk) |

What lives in TAWC (`deps/tawc/tawcroot/`) and is developed there:

- `src/binder.c`, `include/binder_wire.h`, `binderd/`: binder emulation.
- Android device and `/proc`/`/sys` emulation, nested-seccomp no-op for
  zygote, an ownership store (see risks).

Gradle builds the submodule with `deps/tawc/tawcroot/build.sh` (NDK) and
packages `libtawcroot.so` and `libbinderd.so` as jniLibs, the same
extractor trick `libvine_helper.so` uses. tawcroot only supports `arm64-v8a`
and `x86_64`, so the rootless engine is unavailable on the 32-bit ABIs.

## ROMs

One `.vrom` serves both engines: the root engine loop-mounts the images,
the rootless engine extracts them once per ROM into a shared read-only
rootfs directory, with a per-instance writable `data` dir.

- `ROMImage` gains a `kind` (`vine`, `waydroid`).
- The root engine needs system-as-root support (it currently requires
  `ramdisk.img`, `container/namespace_manager.cpp:593`).
- The extractor is read-only ext4 (extents, linear and htree directories,
  fast/slow symlinks, mode/uid/gid). Ownership goes into the ownership store.

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
   `tawcroot --init --cgroup -r <rootfs> -b <instance>/data:/data -b <instance>/run:/run/user/1000 -- /init`.
4. The vine agent reports boot completion, apps, icons, as on the root path.

## Milestones

1. binderd + tawcroot shim, host tests (in TAWC): transaction/reply,
   refcounts, fd passing, death notification, scatter-gather.
2. On device: servicemanager + hwservicemanager + `service list` under
   tawcroot.
3. Compositor in the app, surfaceflinger + composer HAL → first frame.
4. system_server, launcher, input; freeze the ROM patch list.
5. Rootless engine behind the helper protocol, `.vrom` extraction, UI switch.
6. Own 64-bit-only ROM build if milestone 4 needs patches.

## Risks

- **Phantom process killer**: Android 12+ kills app child processes past
  32, system-wide. A booted guest runs 60+. Likely needs the user to turn
  it off (12L+: `adb shell settings put global settings_enable_monitor_phantom_procs false`).
  Blocking for real use.
- **File ownership**: tawcroot reports files as root-owned and has no
  per-file uid store. Android relies on per-uid ownership (`/data/data/<pkg>`
  owned by the app uid, `system` files), so tawcroot needs an ownership store.
- Binder cost: every call is SIGSYS + socket round trips + two copies.
  Fallback: patch libbinder in our ROM to talk to binderd directly.
- Networking: no netns rootless, and ConnectivityService wants a
  netd-managed default network. Probably a ROM patch.
- FUSE storage: `/dev/fuse` cannot be mounted rootless. Probably a ROM patch.
- 16 KB page kernels: the guest's 4 KB-aligned ELFs will not load.
- Memory: a booted Android 13 needs roughly 1.5–2 GB.
