# Rootless boot status (Waydroid LineageOS 20, on device)

Galaxy S24 (Android 17, kernel 6.1, 4K pages, arm64 only, SELinux
enforcing), app uid via `run-as`, `scripts/rootless/boot-init.sh`.

## Where it is

init runs the whole boot sequence and stays up: early-init, init,
late-init, fs, post-fs, post-fs-data, zygote-start, boot. Services start
(logd, servicemanager, hwservicemanager, vndservicemanager, vold, apexd,
keystore2, netd, tombstoned, adbd, ...), the guest's logcat and
debuggerd tombstones work. Zygote preloads and forks system_server, which
gets through the bootstrap services and blocks in `StartDisplayManager`:
there is no display yet (SurfaceFlinger aborts: "no suitable EGLConfig
found"). Milestone 3 is next.

## Rootfs preparation (host contract)

What the engine must do to an extracted image, done today by
`boot-init.sh`:

- Hosts without AArch32 (Exynos 2400 and other recent SoCs): rewrite the
  `ro.*.product.cpu.abilist*` props to 64-bit only and `ro.zygote` to
  `zygote64`; derive `init.zygote64.rc` from `init.zygote64_32.rc` minus
  `zygote_secondary`. Otherwise init runs 32-bit services
  (`boringssl_self_test32`, `reboot_on_failure`) that cannot exec.
- `/dev/{binder,hwbinder,vndbinder}` symlinks into `/dev/binderfs`:
  libbinder `access()`es the driver path before opening it.
- Bind host `/dev/ptmx` and `/dev/pts`: init's logwrapper needs a pty
  (linkerconfig, vdc), without which no linker namespaces get generated.
- lmkd: `/product/etc/init/vine-rootless.rc` overrides it as non-critical
  oneshot. It needs PSI or memcg, both closed to apps (the host's lmkd
  manages the app as a whole).
- Start init with its cwd at the guest's `/`: services inherit it, and a
  cwd outside the view reads back as `getcwd` -ENOENT (libopenjdk crashes
  on it in `System_specialProperties`).
- Write files outside tawcroot with care: rc files must not be group or
  world writable (init skips them; app umask is 0), and absolute symlinks
  in the image (`/product`) resolve against the host there.
- Clear the previous boot's runtime state (`/dev/__properties__`,
  `/dev/cgroup_info`, ...) and tawcroot's per-session store files.

## tawcroot / binderd fixes this needed

- epoll_ctl on synthesized mountinfo fds used the x86_64 (packed)
  `epoll_event` layout on arm64, cutting `data` to 32 bits: init's
  MountHandler got a truncated pointer back and segfaulted.
- Denied `security.selinux` writes succeed without effect (restorecon in
  init/apexd treats the denial as fatal).
- `--all-caps`: fake capabilities start full instead of nspawn's set;
  init aborts services whose capset is refused (logd's CAP_SYSLOG).
- Fake securebits incl. `PR_SET_KEEPCAPS` (the host app's are locked).
- setrlimit/prlimit64 raising a hard limit past the host's clamp for a
  fake CAP_SYS_RESOURCE holder (bpfloader's 1 GiB RLIMIT_MEMLOCK).
- `/proc/self/stat` startstack is the guest's stack: bionic sizes the
  main thread's stack from it, and ART's stack guard failed.
- Emulated mount namespaces (`unshare(CLONE_NEWNS)`): zygote and every
  app fork unshare and mount privately.
- binderd: a dying process's last ref could wake the node's owner, whose
  read freed the node before the release finished (double free, killed
  binderd and with it every service).

## Known gaps, next

- Display: compositor + SurfaceFlinger/composer HAL (milestone 3).
- Some Waydroid vendor HALs are 32-bit only (audio, camera, cas): no
  audio/camera on 64-bit-only hosts without a rebuild.
- mdnsd aborts on `FD_SET` with fd 1056+: guest-visible fds at or above
  tawcroot's reserved floor (1000) break `select()` users.
- statsd, netd and others fail on missing `libstatssocket`,
  `libnetd_updatable` (APEX libs not reachable in the default namespace)
  and on netlink/iptables; vold logs EACCES on `/proc/filesystems`.
- lmkd should become a stub that answers system_server's socket.
