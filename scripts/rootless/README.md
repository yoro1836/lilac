# Rootless bring-up scripts

Device-side scripts for iterating on the rootless engine over adb, as the
shell user, in `/data/local/tmp/vine-dev/`:

- `bin/`: `tawcroot` and `binderd` (`tawcroot/build.sh --abi=aarch64`, from
  `app/src/main/jniLibs/arm64-v8a/`), `ext4extract` (NDK static build of
  `app/src/main/cpp/rootless/ext4_extract.c` with `-DVINE_EXT4_EXTRACT_MAIN`).
- `w/rootfs`: a Waydroid system image extracted with
  `bin/ext4extract system.img w/rootfs --owners w/tawcroot/owners`, its
  vendor image into `w/rootfs/vendor` the same way.

The shell user may not create socket files under `/data/local/tmp`, so
binderd runs with `--abstract` there (`BINDERD_ARGS="--abstract"`).

`run-sm.sh`: binderd + servicemanager, then `service list`.
