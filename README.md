<div align="center">

# 🌿 Wayondroid

**Waydroid on Android: a second Android inside an ordinary app, without root.**

[![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue.svg)](LICENSE)

</div>

Wayondroid boots Waydroid's LineageOS 20 (Android 13) images inside a normal
Android app on a stock, unrooted phone. There is no VM and no kernel module:
the guest's own `init`, `zygote`, `system_server`, SurfaceFlinger and apps
run as the app's processes, on a userspace container runtime (**tawcroot**)
and a userspace binder (**binderd**).

> **Status: alpha.** Developed and tested on a Galaxy S24 (Exynos 2400,
> Android 17, arm64-only, SELinux enforcing). Other phones are untested.
> Expect rough edges; see [Known limitations](#known-limitations).

## What works

| Area | State |
|---|---|
| Boot | Waydroid LineageOS 20 `waydroid_arm64_only` images, VANILLA and GAPPS, boot to a usable home screen without root |
| Display | Guest frames go straight to the screen. With GPU acceleration on, frames stay on the phone's GPU (zero-copy, Android 10+); otherwise the guest renders in software |
| GPU | Experimental: the guest's Vulkan/GLES runs on the phone's GPU through gfxstream |
| Input | Multi-touch, keys, text input. The phone's volume keys control the guest while its screen is open |
| Audio | Guest playback through the phone's speakers (a PulseAudio server for Waydroid's audio HAL, played with AAudio) |
| Network | The guest is online through the phone's connection |
| Storage | Shared storage (`/sdcard`) is mounted: the Files app, file and photo pickers work |
| Google apps | GAPPS images run Google Play services |
| Memory | The guest kills its cached apps when it goes over the instance's RAM setting or the phone runs low |
| Per instance | RAM, storage size, frame rate (up to the phone's refresh rate) and resolution |
| App | Material You UI, multiple instances, home-screen shortcuts to guest apps |

## Requirements

- A 64-bit phone (`arm64-v8a`). tawcroot also builds for `x86_64`; the
  32-bit ABIs only get the root engine.
- The app installs on Android 8.0 and newer, but the engine has only been
  tested on Android 17. Zero-copy display needs Android 10, matching the
  phone's refresh rate to the guest needs Android 11.
- Plenty of free RAM: the guest is a whole Android, GAPPS more so.
- Storage: about twice the ROM's size per instance (images plus the extracted
  rootfs), plus what the guest writes.

## Getting a ROM

Wayondroid boots `.vrom` files: a zip holding a `manifest.json`, `system.img`
and `vendor.img`. Build one from Waydroid's official images:

1. Download a system image and the vendor image from Waydroid's OTA server.
   Use the **`waydroid_arm64_only`** builds (the guest runs 64-bit only):
   - system: `https://ota.waydro.id/system/lineage/waydroid_arm64_only/VANILLA.json`
     or `.../GAPPS.json` for Google apps
   - vendor: `https://ota.waydro.id/vendor/waydroid_arm64_only/MAINLINE.json`
     (one vendor image serves both)

   Each JSON lists the file's URL and sha256.
2. Unzip both into one folder, so it holds `system.img` and `vendor.img`.
3. Pack them:

   ```sh
   python3 rom/package_vrom.py --kind waydroid IMAGES_DIR waydroid-lineage20.vrom \
       --name "Waydroid LineageOS 20" --android-version 13 --api-level 33
   ```
4. In the app, import the `.vrom` on the **ROMs** tab, then create an instance from it.

The first boot extracts the images and compiles every app, so it takes a few
minutes; later boots are much faster.

## Building

```sh
./gradlew :app:assembleDebug
```

Needs the Android SDK and NDK. Gradle builds tawcroot and binderd
(`tawcroot/build.sh`) and the native code. The GPU pieces (gfxstream host
renderer, the guest's gfxstream Vulkan driver, `vine_kumquat`) are prebuilt by
`scripts/gpu/build-gpu.sh` into `app/src/main/jniLibs/`.

Host tests: `tawcroot/test.sh --host` for the runtime and the scripts in
`app/src/main/cpp/tests/native/` (`run_rootless_tests.sh`,
`run_wayland_tests.sh`, `run_display_tests.sh`, `run_pulse_tests.sh`) for the
engine. App unit tests: `./gradlew :app:testDebugUnitTest`.

## How it works

```
Wayondroid app (one app uid, no root)
├─ UI, instance manager, display (SurfaceView / SurfaceControl)
└─ libvine_helper.so --rootless
     ├─ binderd       binder, hwbinder and vndbinder in userspace
     ├─ vine_wl       Wayland server for Waydroid's hwcomposer → shared vinefb
     ├─ vine_pulse    PulseAudio server → AAudio
     ├─ vine_kumquat  gfxstream host renderer (GPU acceleration on)
     └─ tawcroot --init -r <rootfs> -- /system/bin/init
          └─ the guest: servicemanager, SurfaceFlinger, zygote64,
             system_server, apps
```

- **tawcroot** traps the guest's syscalls with seccomp and emulates what an app
  may not do: root identities and file ownership, mounts and mount namespaces,
  `/proc` and `/sys`, devices, network interfaces.
- **binderd** implements the binder driver over Unix sockets.
- Inside the guest, small stand-ins replace what needs kernel features apps
  don't get: `vine_lmkd` (memory killer), a vold shim (shared storage
  without FUSE) and a gralloc shim.

Details: [docs/rootless.md](docs/rootless.md) (design),
[docs/rootless-boot.md](docs/rootless-boot.md) (what the boot needs, and why),
[docs/tawcroot/](docs/tawcroot/) (the runtime).

## Known limitations

- 32-bit guest apps don't run (the guest is 64-bit only).
- GPU acceleration is experimental; turn it off if apps misdraw or crash.
- Files apps write directly into shared storage show up in the gallery and
  media lists only after the next media scan (the Files app sees them at once).
- Snapshots and app icons are not available for these instances yet.
- Now and then a boot stalls; stopping and starting the instance again helps.
- Every guest app is a process of Wayondroid, so the phone's memory is shared:
  close other heavy apps for a smoother guest.

The older root engine (real namespaces and loop mounts, for rooted phones) and
its guest kit in [`rom/`](rom/README.md) are still in the tree but have not
booted a ROM yet.

## License

Wayondroid is licensed under the GNU General Public License v3.0, see
[LICENSE](LICENSE). tawcroot comes from TAWC (MIT).

| Component | License | Use |
|---|---|---|
| [Waydroid](https://github.com/waydroid/waydroid) / [LineageOS](https://lineageos.org/) images | Apache-2.0 and others | Guest ROM |
| [gfxstream](https://android.googlesource.com/platform/hardware/google/gfxstream/), [Mesa](https://www.mesa3d.org/) | Apache-2.0, MIT | GPU acceleration |
| [QEMU](https://www.qemu.org/) | GPL-2.0-or-later | 32-bit emulation (root engine) |
| [Jetpack Compose](https://developer.android.com/jetpack/compose), [Hilt](https://dagger.dev/hilt/), [Room](https://developer.android.com/training/data-storage/room) | Apache-2.0 | App |

## Credits

- **[Waydroid](https://github.com/waydroid/waydroid)**: the container-ready
  Android images and hwcomposer this builds on
- **TAWC** (Tess's Android Wayland Compositor): where tawcroot started
- **[Anbox](https://github.com/anbox/anbox)** and
  **[redroid](https://github.com/remote-android/redroid-doc)**: earlier
  Android-in-a-container work
- **gfxstream / rutabaga** (AOSP, crosvm): the GPU stack
