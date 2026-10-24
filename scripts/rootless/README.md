# Rootless bring-up scripts

Device-side scripts for iterating on the rootless engine over adb.

Staging lives in `/data/local/tmp/vine-dev/` (shell user):

- `bin/`: `tawcroot` and `binderd` (`tawcroot/build.sh --abi=aarch64`, from
  `app/src/main/jniLibs/arm64-v8a/`), `ext4extract` (NDK static build of
  `app/src/main/cpp/rootless/ext4_extract.c` with `-DVINE_EXT4_EXTRACT_MAIN`),
  `faultcatch` (NDK static build of `faultcatch.c`).
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

Knobs: `T` (seconds to run, default 20), `WRAP=bin/faultcatch` (init and
binderd under the crash dumper), `BINDERD` (another binderd build),
`CGROUP_FLAG=` (drop `--cgroup`), `LOGCAT_DELAY`, `KMSG_LINES`.
