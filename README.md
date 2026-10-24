<div align="center">

# 🪻 LILAC

**LILAC Is a Lightweight Android Chroot.**
Run Android inside Android, no root, no VM.

[![CI](https://github.com/yoro1836/lilac/actions/workflows/ci.yml/badge.svg)](https://github.com/yoro1836/lilac/actions/workflows/ci.yml)
[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

</div>

---

## What is LILAC?

LILAC aims to run a guest Android userspace inside an ordinary Android app,
as a container rather than a virtual machine. The guest is a plain
directory tree in the app's private storage, and LILAC's own user-space
engine gives it the view of the filesystem and the privileges it expects,
so the host needs neither root nor hardware virtualization.

LILAC started as a fork of [VineOS](https://gitlab.com/Hexadecinull/VineOS/VineOS)
but goes in a different direction. VineOS boots its guest through Linux
namespaces and loop mounts from a root helper, with QEMU for 32-bit code.
LILAC drops all of that.

> **Status.** Early. This repository currently holds the app shell only.
> The container engine is in development and not part of any build yet, so
> nothing boots today.

## Design

- **No root.** Everything runs inside the app's own sandbox, as the app's
  own user.
- **No VM, no emulation.** Guest code runs natively on the host CPU, so
  the guest's ABI has to match the host's.
- **Directory rootfs.** The guest lives in a directory, not a disk image.
- **Boot first, then speed.** The first engine aims for correctness; cutting
  per-syscall overhead comes after the guest boots.

## Requirements

- Android 8.0 (API 26) or higher as the host
- A 64-bit `arm64-v8a` or `x86_64` device

The app targets API 28 on purpose. From API 29 on, SELinux stops apps from
executing files in their own data directory, which is exactly where a guest
rootfs lives. That also means LILAC can't ship on Google Play, which requires
a recent target API.

## Roadmap

### Phase 1: App shell *(done)*
- [x] Fork of VineOS with every VineOS runtime feature removed
- [x] New package (`io.github.yoro1836.lilac`), branding, and GitHub CI

### Phase 2: First boot *(current)*
- [ ] Container engine that starts a guest's `init` from a directory rootfs
- [ ] Rootfs import into app storage
- [ ] Foreground service and log view for a running guest

### Phase 3: A usable guest
- [ ] Binder for guest system services
- [ ] Display and input
- [ ] Networking

### Phase 4: Speed
- [ ] Fewer engine round trips per guest syscall

## Building

You need JDK 17 and the Android SDK (Android Studio is the easiest way to
get both).

```sh
./gradlew :app:assembleDebug        # APK in app/build/outputs/apk/debug/
./gradlew :app:testDebugUnitTest    # unit tests
./gradlew :app:ktlintCheck :app:lintDebug
```

Releases are built by the [Release workflow](.github/workflows/release.yml),
started by hand with a version tag. It signs with the keystore in the
`RELEASE_KEYSTORE_BASE64`, `RELEASE_STORE_PASSWORD`, `RELEASE_KEY_ALIAS`, and
`RELEASE_KEY_PASSWORD` repository secrets.

## Contributing

Issues and pull requests are welcome. Please use
[Conventional Commits](https://www.conventionalcommits.org/) for commit
messages, since release notes are generated from them. To report a security
vulnerability, use "Report a vulnerability" under the repository's Security
tab instead of opening a public issue.

## License

LILAC is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE) for the full text.

## Credits

- [VineOS](https://gitlab.com/Hexadecinull/VineOS/VineOS) by Hexadecinull,
  which LILAC is forked from. Its app structure, theme, and build setup are
  the starting point here.
- [Ananbox](https://github.com/Ananbox/ananbox), a rootless Android container
  that is a reference for how a guest Android can run inside an app.
- [PRoot](https://proot-me.github.io/), which shows that a user-space
  engine can give a process its own root filesystem without privileges.
