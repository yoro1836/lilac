<div align="center">

# 🌿 VineOS

**A free, open-source, ad-free Android-on-Android virtual machine.**
Run a full isolated Android guest inside your existing device, with
**32-bit app support on arm64-only hardware** via QEMU user-mode emulation.

[![Pipeline status](https://gitlab.com/Hexadecinull/VineOS/VineOS/badges/main/pipeline.svg)](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/pipelines)
[![Latest release](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/badges/release.svg)](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/releases)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)
[![Renovate enabled](https://img.shields.io/badge/renovate-enabled-brightgreen?logo=renovatebot)](renovate.json)
[![GitLab stars](https://img.shields.io/gitlab/stars/Hexadecinull%2FVineOS%2FVineOS)](https://gitlab.com/Hexadecinull/VineOS/VineOS)
[![GitLab open issues](https://img.shields.io/gitlab/issues/open/Hexadecinull%2FVineOS%2FVineOS)](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/issues)
[![GitLab open merge requests](https://img.shields.io/gitlab/merge-requests/open/Hexadecinull%2FVineOS%2FVineOS)](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/merge_requests)
[![Discord](https://img.shields.io/discord/1558189985230889033?logo=discord&logoColor=white&label=Discord&color=5865F2)](https://discord.gg/sVgCAdgGgR)

[**Download**](#installation) · [**Website**](https://vineos.hexadecinull.dpdns.org) · [**Documentation**](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation) · [**Roadmap**](#roadmap) · [**FAQ**](#faq) · [**Discord**](https://discord.gg/sVgCAdgGgR)

</div>

---

## Table of Contents

- [What is VineOS?](#what-is-vineos)
- [Features](#features)
- [Supported devices](#supported-devices)
- [ROM support](#rom-support)
- [Installation](#installation)
- [Documentation](#documentation)
- [Repositories](#repositories)
- [Roadmap](#roadmap)
- [FAQ](#faq)
- [Community](#community)
- [Contributing](#contributing)
- [License](#license)
- [Credits & acknowledgements](#credits--acknowledgements)

---

## What is VineOS?

VineOS runs a complete, isolated Android operating system inside your
existing Android device, without modifying the host OS or requiring root
on most paths.

Unlike simple "app cloners" (Parallel Space, Dual Space) that sandbox
individual apps, VineOS virtualizes the full Android stack: its own
`init`, `Zygote`, `SurfaceFlinger`, `ServiceManager`, and everything else.
You get a real second phone inside your phone.

> **Status.** The container engine boots a test ROM through real Linux
> namespaces and mounts, and the app, helper and guest protocols are unit
> tested. No real Android system image has booted inside VineOS yet, so the
> first bootable ROM is the open milestone. The
> [`rom/`](rom/README.md) folder holds the guest side kit a ROM needs.

**Why VineOS over alternatives like VPhoneOS or VMOS?**

| Feature | VineOS | VPhoneOS | VMOS | Virtual Master |
|---|:---:|:---:|:---:|:---:|
| Open source | ✅ | ❌ | ❌ | ❌ |
| Ad-free | ✅ | ❌ | ❌ | ❌ |
| Free (fully) | ✅ | Partial | Partial | ❌ |
| 32-bit on arm64-only devices | ✅ | ✅ | ❌ | ❌ |
| Runs on arm, x86, and x86_64 hosts | ✅ | ❌ | ❌ | ❌ |
| GPL-3.0 licensed | ✅ | ❌ | ❌ | ❌ |
| Material You | ✅ | ❌ | ❌ | ❌ |
| Multiple ROM versions | ✅ | Paid | Partial | Paid |
| No telemetry | ✅ | ❌ | ❌ | ❌ |

---

## Features

- 🆓 **Completely free**: no paywalls, no premium tiers, no ads, ever
- 🔓 **Open source**: GPL-3.0, fully auditable
- 📦 **Multiple ROMs**: Android 7.1, 9.0, 11, 12 (more planned)
- 🏗️ **32-bit support everywhere**: runs armeabi-v7a apps on arm64-only devices via QEMU user-mode binary translation
- 🖥️ **Multi-host support**: builds and runs on arm64-v8a, armeabi-v7a, x86_64, and x86 devices, not just ARM
- 🎨 **Material You**: dynamic color from your wallpaper on Android 12+, static vine-green fallback on 8-11
- ⚡ **Lightweight**: zero background overhead when no VM is running, no persistent daemons
- 🔒 **Isolated**: PID, mount, UTS, IPC, and network namespace separation
- 🌱 **Multi-instance**: run multiple independent VMs simultaneously
- 🔗 **App shortcuts**: pin any app of an instance to your home screen, a tap starts the instance and opens the app
- 💾 **Big storage**: instance storage up to your free space, the disk is a sparse file so it only uses what the guest writes
- 🖥️ **Fullscreen guest**: the guest screen fills the phone, the three on-screen navigation buttons are hidden by default
- 🔧 **Root support**: optional Magisk/root inside instances (requires rooted host)
- ⚡ **Shizuku**: optional, no-root privilege probe via ADB/Wireless Debugging (see [NOROOT_RESEARCH.md](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/reference/NOROOT_RESEARCH.md))
- 📱 **minSdk 26, targetSdk 37**: supports Android 8.0 through Android 17

---

## Supported devices

- Android 8.0 (API 26) or higher as the host OS
- `arm64-v8a`, `armeabi-v7a`, `x86_64`, or `x86` processor
- A kernel with Linux namespaces and loop device support (standard since Android 4.4)

32-bit ARM guest apps run natively where the host CPU supports it, and
through QEMU user-mode emulation everywhere else, including on the growing
range of arm64-only SoCs across many vendors, and on non-ARM hosts
entirely. See
[ARCHITECTURE.md](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/reference/ARCHITECTURE.md#abi-compatibility) for the full
host/guest compatibility matrix.

---

## ROM support

| ROM | Android Version | API | 32-bit | Status |
|---|---|---|---|---|
| `vine-rom-7` | Android 7.1.2 Nougat | 25 | ✅ | 🟡 In progress |
| `vine-rom-9` | Android 9.0 Pie | 28 | ✅ | 🔴 Planned |
| `vine-rom-11` | Android 11 | 30 | ✅ | 🔴 Planned |
| `vine-rom-12` | Android 12 | 31 | ✅ | 🔴 Planned |

ROMs are built from AOSP targeting the `ranchu` virtual device board and
distributed as `.vrom` archives. See
[BUILDING.md](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/guides/BUILDING.md#building-aosp-rom-images) for the ROM
build guide.

---

## Installation

Download the latest release APK from the
[Releases page](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/releases).

VineOS is not yet on the Play Store. First release planned at Beta
stability.

---

## Documentation

All project documentation, including the architecture guide, build
instructions, usage guide, contributing guide, and policies, lives in its own
repository, [VineOS-Documentation](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation). Start there.

---

## Repositories

VineOS is split across three GitLab repositories:

| Repository | What it contains |
|---|---|
| [VineOS](https://gitlab.com/Hexadecinull/VineOS/VineOS) | The Android app, the native runtime, the guest side ROM kit (`rom/`), and CI (this repository) |
| [VineOS-Website](https://gitlab.com/Hexadecinull/VineOS/VineOS-Website) | The project website |
| [VineOS-Documentation](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation) | Guides, reference, and project policies |

---

## Roadmap

### Phase 1: Foundation *(current)*
- [x] Linux namespace container runtime (`unshare`, `pivot_root`, `execve`)
- [x] QEMU binfmt_misc integration for 32-bit support (4-layer AArch32 detection)
- [x] JNI bridge with `FramebufferBridge` + `UInputBridge` wiring
- [x] Material You UI: Home, ROMs, Settings, and detail/create-instance screens
- [x] Room DB + ViewModels + DataStore preferences
- [x] ROM downloader (manifest fetch, streaming download, SHA-256 verification)
- [x] Multi-host-ABI support (arm64-v8a, armeabi-v7a, x86_64, x86)
- [x] Root helper process that owns every guest, spoken to over stdin and stdout
- [x] Engine tested on a real kernel with a fake ROM (namespaces, loop mounts, ext4 `/data`, cgroups, NAT, snapshots)
- [x] ROM kit for the guest side: init patch notes, guest agent, framebuffer writer, `.vrom` packer (`rom/`)
- [ ] First bootable Android 7.1.2 ROM image

### Phase 2: Display & Input
- [x] Framebuffer render loop (mmap to ANativeWindow blit)
- [x] RGB565 to RGBA8888 conversion for Android 7.x guests
- [x] Single-touch forwarding, synced to the guest's real display resolution
- [x] Hardware key forwarding (back, home, volume, recents)
- [x] Multi-touch forwarding, up to 10 fingers, through the guest agent (needs a ROM that runs it)
- [x] Hidden on-screen navigation buttons and a guest screen that fills the phone
- [ ] Clipboard sharing host to guest (guest agent side written, app side pending)

### Phase 3: Storage & ROMs
- [x] Writable `/data` partition (sparse ext4 image per instance, sizes beyond 16 GB)
- [x] Instance snapshot / restore
- [x] App shortcuts on the home screen
- [ ] Android 9, 11, 12 ROM images
- [ ] ROM CDN + in-app download UI completion

### Phase 4: No-root Path
- [x] Shizuku integration to probe whether ADB-shell privilege clears `unshare`/`mount` on a given device (Settings → Shizuku)
- [ ] `proot` fallback for non-rooted devices
- [ ] Android Virtualization Framework (AVF) backend (Android 13+)

### Phase 5: Polish
- [ ] Play Store + F-Droid release
- [ ] Magisk inside instances
- [ ] Per-instance VPN / network isolation
- [ ] GPU passthrough research

---

## FAQ

**Does VineOS require root?**
Today yes. The engine runs in a small helper started with `su`, because
`unshare()` and `mount()` need `CAP_SYS_ADMIN`. The no-root path is Phase 4
and is not built yet. Root inside an instance (Magisk) is a separate, optional thing.

**Does VineOS work on arm64-only phones without 32-bit support?**
Yes, this is one of the core things VineOS is built for. We're aiming to
support a wide range of SoCs across vendors, not just a specific model or
two, and VineOS auto-detects arm64-only chips and switches to QEMU mode
automatically.

**Why GPL-3.0 and not MIT/Apache?**
VineOS uses QEMU (GPL-2.0-or-later) and is architecturally inspired by
Anbox/Waydroid (GPL-3.0). GPL-3.0 ensures commercial forks must also open
their changes.

**Can I use VineOS on x86_64 Android devices or emulators?**
Yes. VineOS builds and runs on `arm64-v8a`, `armeabi-v7a`, `x86_64`, and
`x86` hosts; ARM guest ROMs run through QEMU on non-ARM hosts. See
[ARCHITECTURE.md](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/reference/ARCHITECTURE.md#abi-compatibility) for exactly
what runs natively versus emulated on each host.

---

## Community

Questions, bug reports, or just curious how it works? Join the official
[VineOS Discord](https://discord.gg/sVgCAdgGgR). It's the quickest way to
reach the developers and other users.

---

## Contributing

Contributions of every size are welcome, from typo fixes to ROM images. Read
the [contributing guide](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/project/CONTRIBUTING.md) first, then open an
[issue](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/issues) or a [merge request](https://gitlab.com/Hexadecinull/VineOS/VineOS/-/merge_requests). To
report a security vulnerability, follow the
[security policy](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/project/SECURITY.md) instead of opening a public issue.

---

## License

VineOS is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE) for the full text, and
[TERMS.md](https://gitlab.com/Hexadecinull/VineOS/VineOS-Documentation/-/blob/main/project/TERMS.md) for the project's terms of use.

### Third-party components

| Component | License | Use |
|---|---|---|
| [QEMU](https://www.qemu.org/) | GPL-2.0-or-later | ARMv7 user-mode emulation |
| [AOSP](https://source.android.com/) | Apache-2.0 + various | ROM images |
| [Jetpack Compose](https://developer.android.com/jetpack/compose) | Apache-2.0 | UI framework |
| [Hilt](https://dagger.dev/hilt/) | Apache-2.0 | Dependency injection |
| [Room](https://developer.android.com/training/data-storage/room) | Apache-2.0 | Local database |

---

## Credits & acknowledgements

- **[Anbox](https://github.com/anbox/anbox)**: the original Android-in-a-Linux-container project; VineOS's container architecture is directly inspired by its design
- **[Waydroid](https://github.com/waydroid/waydroid)**: maintained successor, reference for namespace setup and binfmt_misc integration
- **[redroid](https://github.com/remote-android/redroid-doc)**: Android in Docker, reference for mount table and device node setup
- **[QEMU](https://www.qemu.org/)**: the backbone of VineOS's 32-bit emulation
- **[VPhoneOS / VPhoneGaGa](https://vphoneos.com/)**: the closed-source inspiration that VineOS aims to replace with an open alternative
- The entire Android homebrew and modding community

---

<div align="center">
  <sub>Made with 🌿 by <a href="https://gitlab.com/Hexadecinull">Hexadecinull</a> and contributors</sub>
</div>
