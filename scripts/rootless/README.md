# Rootless bring-up scripts

Device-side scripts for iterating on the rootless engine over adb.

Staging lives in `/data/local/tmp/vine-dev/` (shell user):

- `bin/`: `tawcroot` and `binderd` (`tawcroot/build.sh --abi=aarch64`, from
  `app/src/main/jniLibs/arm64-v8a/`), `ext4extract` (NDK static build of
  `app/src/main/cpp/rootless/ext4_extract.c` with `-DVINE_EXT4_EXTRACT_MAIN`),
  `faultcatch` (NDK static build of `faultcatch.c`), `vine_wl` and
  `vine_lmkd` (the app's CMake targets `libvine_wl.so`, `libvine_lmkd.so`),
  `vine_hal_service` (see below).

`vine_hal_service` links the image's own `libhidlbase.so` and `libc++.so`,
whose libc++ ABI namespace is `std::__1` (the NDK's is `__ndk1`). Copy
both from the image's `/system/lib64` into a dir `plat-lib`, put a
`__config_site` defining `_LIBCPP_ABI_NAMESPACE __1` into `plat-inc`
next to the image's HIDL headers, then:

    aarch64-linux-android33-clang++ -std=c++17 -O2 -fPIE -pie \
      -isystem plat-inc -nostdlib++ -Lplat-lib -Wl,--allow-shlib-undefined \
      app/src/main/cpp/rootless/hal_service.cpp -lhidlbase -lc++
- `w/rootfs`: a Waydroid system image extracted with
  `bin/ext4extract system.img w/rootfs --owners w/tawcroot/owners`, its
  vendor image into `w/rootfs/vendor` the same way.

The shell user may not create socket files under `/data/local/tmp`, so
binderd runs with `--abstract` there (`BINDERD_ARGS="--abstract"`).

`run-sm.sh`: binderd + servicemanager, then `service list`.

## App context

A full boot runs as an app uid, like the real engine: a code-less
debuggable package (`me.vine.dev`) entered with `run-as`, the same tree
under its data dir (`bin/`, `w/`). Copy files in from the staging dir
(`run-as me.vine.dev cp /data/local/tmp/vine-dev/bin/X bin/`).

`boot-init.sh`: prepares the rootfs (see docs/rootless-boot.md), starts
binderd, `tawcroot --init` on Waydroid's init (second stage) and the
guest's `logcat`. Outputs: `w/init.log`, `w/logcat.txt`,
`w/rootfs/dev/kmsg` (NUL-separated), `w/binderd.log`.

Also starts `vine_wl` (frames land in `w/vinefb`, a 4096-byte header then
RGBA rows) and kills every guest process at the end, reparented zygotes
included.

Knobs: `T` (seconds to run, default 20), `WRAP=bin/faultcatch` (init and
binderd under the crash dumper), `BINDERD` (another binderd build),
`BINDERD_FLAGS` (`-v`), `CGROUP_FLAG=` (drop `--cgroup`), `LOGCAT_DELAY`,
`KMSG_LINES`, `SIZE` and `DENSITY` (the display), `DIAG` (a guest shell
command run before teardown, output in `w/diag.txt`).

`kill -USR1` on binderd logs every process, its threads and their
transaction stacks to `w/binderd.log`.
