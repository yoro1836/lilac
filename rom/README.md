# Wayondroid ROM kit

Everything a ROM needs to run inside a Wayondroid instance, kept next to the engine that expects it.

**Status: written against the engine contract and unit tested on a host, never run inside a booted Android ROM.** The engine runs a fake init on a real kernel in the host tests. Nothing here has booted a real Android system image yet, so treat the first boot as the open milestone, not a given.

## What the engine gives a guest

The runtime starts the ROM's `/init` (from `ramdisk.img`) as PID 1 with these things already in place:

| Path | What it is |
|---|---|
| `/system`, `/vendor` | The ROM's images, loop mounted read only. |
| `/data` | A sparse ext4 image created on first start. Its size is the instance's storage setting and is only an upper bound on real disk use. |
| `/cache` | 64 MB tmpfs. |
| `/proc`, `/sys`, `/dev` | Fresh mounts. `/proc/sys` and sysrq are read only, `/sys` is read only, `/dev` is a small tmpfs with null, zero, full, random, urandom, tty, fuse, pts, ptmx, console and the binder devices. There is no uevent source, so ueventd has nothing to do. |
| `/dev/binder`, `/dev/hwbinder`, `/dev/vndbinder` | A private binderfs instance when the phone kernel has binderfs, otherwise the Anbox binder devices if present. |
| `/vine` | The instance's shared folder. See below. |
| Network | Own network namespace with `eth0` already addressed (`10.77.<slot>.2/30`) and a default route, NATed by the host. |

The container has its own PID, UTS, IPC, network and mount namespaces and a cgroup. It has **no user namespace**: guest root is the phone's root. See the threat model in the documentation repository.

## The shared folder at `/vine`

| File | Direction | Meaning |
|---|---|---|
| `boot.prop` | host to guest | `ro.boot.vine.*` properties (instance, width, height, dpi, ram_mb, root, qemu32, net_ip, net_gw, net_dns). The engine also merges them into `default.prop` of the ramdisk and into `/proc/cmdline` as `androidboot.vine.*`. |
| `vinefb` | guest to host | The display. 4096 byte header then pixels. Write it with `hwc/vinefb_writer.h`. |
| `agent.sock` | host to guest | Unix socket the guest agent listens on. Input, app list, launch, icons, clipboard. |
| `boot_completed` | guest to host | Created by the guest when Android finished booting. The host marks the instance running when it appears. |

## Contents of this kit

| Path | Purpose |
|---|---|
| `INIT_PATCH.md` | What a stock Android `init` must change to live in a container, including the marker string the engine requires. |
| `init/init.vine.rc` | Ramdisk `/init.vine.rc`. Starts the agent, the network repair script and writes `boot_completed` once `sys.boot_completed=1`. |
| `system/etc/vine/net.sh` | Repairs `eth0` from the boot props if the engine's setup is missing. |
| `agent/` | The guest agent. `AgentProtocol.java` is plain Java and unit tested, `VineAgent.java` is the Android side (input injection, PackageManager, clipboard). `build_agent.sh` makes `vine-agent.jar`. |
| `hwc/vinefb_writer.h` | Header only writer for the display contract, tested against the host reader. |
| `package_vrom.py` | Packs `ramdisk.img`, `system.img` and an optional `vendor.img` into a `.vrom`. `--kind waydroid` packs a system-as-root image without a ramdisk (Waydroid's system and vendor images), which the app boots on the rootless engine (`docs/rootless.md`). |

## Assembling a ROM

1. Patch `init` as described in `INIT_PATCH.md` and rebuild the ramdisk.
2. Build the agent: `ANDROID_HOME=$HOME/Android/Sdk sh agent/build_agent.sh`, then copy `agent/build/vine-agent.jar` to `/system/framework/` of the system image.
3. Put `init/init.vine.rc` at the root of the ramdisk as `/init.vine.rc` (the engine sets `ro.hardware` to `vine` when it finds that file, so `init` loads it) and copy `system/etc/vine/net.sh` to `/system/etc/vine/` in the system image.
4. Make the ROM's graphics stack write finished frames through `vinefb::Writer` (for example from a small hwcomposer or gralloc shim, or the software renderer's present call).
5. Pack it: `python3 package_vrom.py images/ MyROM.vrom --name "My ROM" --android-version 13 --api-level 33`.
6. Import it in the app, create an instance, start it, and open Show diagnostics on the instance screen to read the tail of the boot log when it fails.

## Known open problems

- **The first real boot.** Nothing here proves that a stock Android 10+ userspace gets through `init` without a kernel that has what it expects (binder, ashmem or memfd, the right cgroups). The host probe in the app lists what the phone kernel offers.
- **SELinux.** The kernel's policy is the phone's. A guest `init` that loads its own policy is refused by the engine unless the marker is present, and even a patched `init` has to skip policy loading.
- **Android's own networking.** `eth0` works at the kernel level, but Android's ConnectivityService only treats it as the default network if the ROM ships Ethernet support. `net.sh` sets the addresses and DNS properties, it cannot make the framework adopt the interface.
- **Display.** No ROM writes `vinefb` yet. The host reader and guest writer match each other, which is all that is proven.
- **Guest root is host root.** There is no user namespace, so only run ROMs you trust.
