# Waydroid init segfaults in early second stage (rootless, on device)

First real milestone-4 blocker. On a Galaxy S24 (Android 17, kernel
6.1.162, 4K pages, SELinux enforcing), running as a normal app uid
(`run-as me.vine.dev`), Waydroid's `/system/bin/init second_stage` under
`tawcroot --init --owners` with binderd bound at `/dev/binderfs` gets
through a lot, then **SIGSEGVs inside init itself** (signal 11; init's
own handler then loops InitFatalReboot -> `reboot(2)` EPERM).

Reproduce: `scripts/rootless/` (app-context variant), rootfs extracted
with `ext4extract` into the app data dir, images copied to app-readable
storage first.

## How far it gets (with `--cgroup`)

- second stage starts, props load, SELinux file_contexts loaded,
  restorecon runs (the `Could not set context` lines are harmless -- the
  app uid can't setfscreatecon).
- `property_service` socket created; property service up.
- `Cannot open fd for current mount namespace: Permission denied` (twice)
  -- init's SetupMountNamespaces; it logs and continues.
- vendor_init subcontext forks.
- all `/system/etc/init/*.rc` and vendor rc files parse.
- `SetupCgroups` action runs; libprocessgroup fails to mount every
  controller (blkio/cpu/cpuset/memory/schedtune: EPERM).
- **SIGSEGV right after**, before any service starts.

Without `--cgroup`, SetupCgroups hits "Attempt to call SetupCgroups more
than once" and SIGSEGVs at the same point, so the crash is not the mount
failures themselves.

## Suspects

- libprocessgroup / init walking cgroup state after every controller
  mount failed -- a NULL/empty path it never sees on a real device,
  where the controllers always mount.
- tawcroot `--cgroup` emulation vs. what libprocessgroup expects at
  `/sys/fs/cgroup` when the guest is pid-1-ish but unprivileged.
- `Cannot open fd for current mount namespace` -- init wants its own
  mount ns; rootless has none. It continues, but later mount assumptions
  may be violated.

## Next

Early boot is now an iterate-on-a-real-crash problem, which needs a
debuggable loop:

- Capture a tombstone: run init under a wrapper with a SIGSEGV handler
  dumping `/proc/self/maps` + the faulting address, or bind a writable
  `/data/tombstones` and let debuggerd write there.
- Or stand up the GitHub Actions Android emulator loop (KVM there) so
  crashes come with symbols and tombstones pull locally.
- Decide what cgroup shape to present: a fake read-only `/sys/fs/cgroup`
  that makes libprocessgroup's setup a no-op success rather than a
  partial failure.
