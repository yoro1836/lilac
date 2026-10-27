# Wayondroid

Runs Waydroid's LineageOS 20 images inside an ordinary Android app, without
root: tawcroot (seccomp-trapping userspace runtime), binderd (userspace
binder), vine_wl (Wayland server for Waydroid's hwcomposer), vine_pulse
(audio), vine_kumquat (gfxstream GPU), and guest stand-ins (vine_lmkd,
the vold FUSE shim, vine_mediascan). Start with [README.md](README.md),
[docs/rootless.md](docs/rootless.md) (design) and
[docs/rootless-boot.md](docs/rootless-boot.md) (what the boot needs, and why).

## Talking to the user
- Reply in the language the user writes in. Keep answers short and to the point.
- Never print tokens, keys, server addresses or gist URLs, in replies or logs;
  redact them in command output.

## Quick reference
- Build: `./gradlew :app:assembleDebug`. In a cloud session add
  `--init-script scripts/remote/maven-mirror.init.gradle.kts` (Maven Central
  fails at random through the egress proxy).
- GPU libraries (`app/src/main/jniLibs/*/libgfxstream_backend.so`,
  `libvine_kumquat.so`, `libvine_vk_gfxstream.so`, `libc++_shared.so`) are
  gitignored: build them with `scripts/gpu/build-gpu.sh` before an APK that
  needs GPU acceleration. tawcroot and binderd build with Gradle.
- Tests: `app/src/main/cpp/tests/native/run_{rootless,wayland,display,pulse}_tests.sh`
  (host), `tawcroot/test.sh --host`, `./gradlew :app:testDebugUnitTest`.
  A test you add should fail without the fix.
- App ids: debug `com.hexadecinull.vineos.debug`, release `com.hexadecinull.vineos`.

## Phones
- Remote access from a cloud session: [docs/remote-device.md](docs/remote-device.md).
  `scripts/remote/setup.sh`, then `connect.sh` (token from
  `$WAYONDROID_PHONE_TOKEN` or the user), `deploy.sh` (APK through the
  tunnel: minutes), `install-ci.sh` (push first; the phone fetches the
  `dev-apk` workflow's APK itself: seconds once CI is done), `hs.sh` (the rootless
  helper's debug socket: `status|start|stop|exec <instance> ...`), `ui.py tap`.
- The cloud session's network is HTTPS-only (port 443, no UDP, no inbound).
  Don't try to get around it; ask the user when something needs more.
- The phones are the user's own. Don't hold the screen longer than a test
  needs; the user may be using it.
  - Galaxy S24 (main phone, no root): never reboot it.
  - Galaxy S22 (test phone, no lock screen): reboots are fine when a test
    needs one. It is rooted, but `adb shell` gets no root; install it with
    `install-ci.sh` (CI's signing key, so not `deploy.sh`).
- Filter logcat to our processes. Scratch files go in `/data/local/tmp/tawc-dev/`;
  delete them, and screenshots, when done.
- Guest app processes keep zygote's cmdline under tawcroot; find them by
  `/proc/<pid>/comm`. The helper's `exec` runs outside the guest's init
  session: no `/linkerconfig`, so `app_process` tools (`sm`, `content`) fail
  there; `cmd`-based ones (`am`, `input`), `dumpsys`, `getprop`, `logcat` work.

## Code and git
- Match the surrounding code: naming, comment density, idiom. Keep comments,
  errors and commit messages short.
- Don't run formatters (`clang-format`, `ktlint`, `cargo fmt`, ...).
- Work on the session's assigned branch; commit and push when asked or when
  the stop hook asks. Update the docs a change makes stale in the same commit.
