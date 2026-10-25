# Making Android's init container aware

Stock `init` assumes it owns the machine. In a Wayondroid instance it does not, and one thing in particular is dangerous: it loads its SELinux policy into the kernel it runs on, and that kernel is the phone's. The engine therefore refuses to start an `init` that does not contain the marker below.

```
VINE_CONTAINER_INIT_V1
```

The marker is only a promise. The patch below is what keeps it. The function names come from AOSP `system/core/init` and move between releases, so treat this as a map to the right places, not a diff to apply.

**This is a sketch. It has not been applied to a real AOSP tree or booted.**

## 1. Embed the marker

Keep the string in the binary, for example a `const char kVineMarker[] = "VINE_CONTAINER_INIT_V1";` that `main()` reads through a `volatile` pointer so the linker keeps it. The engine scans the first megabytes of `/init` for it.

## 2. Detect the container

```
static bool InVineContainer() {
    return access("/vine/boot.prop", R_OK) == 0;
}
```

The engine writes that file before init starts, so its presence is a reliable test.

## 3. Skip what the engine already did

When `InVineContainer()` is true, init should:

| Stock behaviour (file) | In a container |
|---|---|
| First stage mounts tmpfs `/dev`, `/proc`, `/sys` and the fstab partitions (`first_stage_init.cpp`, `first_stage_mount.cpp`) | Skip it. They are mounted. Go straight to the second stage. |
| Re-exec into the second stage (`first_stage_init.cpp`) | Call `SecondStageMain` directly. |
| Load and enforce SELinux policy (`selinux.cpp`, `SetupSelinux`, `SelinuxInitialize`) | Skip completely. Do not call `security_load_policy` or `setenforce`. |
| Restore file contexts (`SelinuxRestoreContext`) | Skip. |
| Cold boot and ueventd (`ueventd.cpp`, `uevent_listener.cpp`) | Skip. There is no uevent source and the engine created the device nodes. |
| Mount `/dev/pts`, `/proc`, `/sys`, `/dev/cpuctl` and cgroups in the early mount code (`mount_handler.cpp`, `init.cpp`) | Skip existing mounts, keep cgroup setup but tolerate failure. |
| Set `kernel.*` sysctls and write to `/proc/sys` (`init.rc` and the `write` actions) | These fail on the read only `/proc/sys`. Remove or ignore the failures. |
| `sys.powerctl` shutdown and reboot (`reboot.cpp`) | Do not call `reboot(2)`. Exit init instead. PID 1 leaving ends the guest and the host shows the instance as stopped. |

## 4. Boot properties

The engine puts `ro.boot.vine.*` into the ramdisk's `default.prop` and `/proc/cmdline`. Nothing extra is needed. Android's `ro.boot.*` import from the kernel command line already picks up the `androidboot.vine.*` words.

## 5. Services Android expects but a container lacks

| Service | Typical fix |
|---|---|
| `ueventd` | Remove from init.rc. |
| `healthd`, `vold` | Stub or remove. `vold` needs block devices the guest does not have. |
| `bootanim` | Keep, it writes through the display. |
| Anything touching `/dev/graphics/fb0` | Replace with `vinefb`. See `hwc/vinefb_writer.h`. |

## 6. Verify

1. `strings out/target/product/*/ramdisk/init | grep VINE_CONTAINER_INIT_V1`
2. In the app, create an instance from the ROM and start it. Show diagnostics on the instance screen includes the tail of the boot log with init's console output.
3. The agent answering `ping` means userspace reached `sys.boot_completed`.
