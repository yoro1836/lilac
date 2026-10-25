# Rootless boot status (Waydroid LineageOS 20, on device)

Galaxy S24 (Android 17, kernel 6.1, 4K pages, arm64 only, SELinux
enforcing), app uid via `run-as`, `scripts/rootless/boot-init.sh`.

## Where it is

The guest boots to its lock screen and is usable through injected
input: a swipe unlocks to the launcher, taps open apps (Settings), HOME
and BACK work, and text typed into a search field arrives. init runs
the whole boot sequence, zygote forks system_server, SurfaceFlinger
composes in software (ANGLE on lavapipe) and Waydroid's hwcomposer
posts each frame as a `wl_shm` buffer to `vine_wl`, which writes it into
`w/vinefb`. Not stable yet, see the gaps below.

## Display path

- `vine_wl --socket w/run/wayland-0 --vinefb w/vinefb --create WxH`
  (`app/src/main/cpp/display/wayland/`): a small Wayland server for one
  client, Waydroid's hwcomposer in single-window mode. Globals
  wl_compositor, wl_shm, xdg_wm_base, wl_output, wl_seat (touch and
  keyboard); no subcompositor or viewporter, so the client stays on the
  single-surface path. `w/run` is bound to `/run/user/1000`.
- Input: `vine_wl --input w/input` takes the guest agent's lines
  (`touch`, `key`, `text`, as `GuestProtocol.kt` writes them) and sends
  them as wl_touch/wl_keyboard events; the hwcomposer writes evdev
  events into `/dev/input/wl_{touch,keyboard}_events` FIFOs, which the
  image's EventHub opens as `wayland_touch` and `wayland_keyboard`
  (Generic.kl). Android keycodes map to evdev codes below 239 (the
  hwcomposer's limit): APP_SWITCH has none. `vine_wl --send w/input
  LINE...` injects from a shell.
- `/vendor/waydroid.prop`, what Waydroid's host writes: gralloc
  `default`, egl `angle`, vulkan `lvp`, `sys.use_memfd=true`, GLES 3.2,
  `ro.sf.lcd_density`, plus `waydroid.background_start=false` (else the
  first window is destroyed and no frame is posted) and
  `waydroid.stub_sensors_hal=1` (else the sensors HAL never registers and
  system_server waits for it).

## Audio path

- `vine_pulse --socket w/run/pulse/native` (`app/src/main/cpp/audio/pulse/`):
  a small PulseAudio server (native protocol 32, no shared memory) for
  Waydroid's audio HAL, whose ALSA `pulse` plugin (libpulse 12) connects
  to `/run/user/1000/pulse/native`. Each playback stream plays through
  its own AAudio stream, each record stream captures through AAudio
  (silence when AAudio can't open it). Tested on the host with libpulse
  and the ALSA plugin; on device the server starts, but no stream has
  reached it yet (see the gaps).
- The image's `mediaserver` (`media.player`, `libmediaplayerservice`) is
  32-bit only, so on 64-bit-only hosts every `MediaPlayer` user waits
  for a service that never comes (ringtone previews hang Settings), and
  codec lists fall back to slow local lookups. Fixing that takes a
  64-bit-only image or 32-bit emulation.

## Rootfs preparation (host contract)

What the engine must do to an extracted image: the app's rootless engine
does it in `app/src/main/cpp/rootless/rootless_prep.cpp` before every
boot, the dev loop in `boot-init.sh`:

- Hosts without AArch32 (Exynos 2400 and other recent SoCs): rewrite the
  `ro.*.product.cpu.abilist*` props to 64-bit only and `ro.zygote` to
  `zygote64`; derive `init.zygote64.rc` from `init.zygote64_32.rc` minus
  `zygote_secondary`. Otherwise init runs 32-bit services
  (`boringssl_self_test32`, `reboot_on_failure`) that cannot exec.
- `/dev/{binder,hwbinder,vndbinder}` symlinks into `/dev/binderfs`:
  libbinder `access()`es the driver path before opening it.
- Bind host `/dev/ptmx` and `/dev/pts`: init's logwrapper needs a pty
  (linkerconfig, vdc), without which no linker namespaces get generated.
- lmkd: `/product/etc/init/vine-rootless.rc` replaces it with
  `vine_lmkd` (`app/src/main/cpp/rootless/lmkd_stub.c`), which answers
  system_server on the lmkd socket. The real one needs PSI or memcg, both
  closed to apps (the host's lmkd manages the app as a whole); without
  any lmkd every oom-adj update stalls system_server for about 3 s.
- Audio HAL: in 64_32 images Waydroid's service is 32-bit only. `vine_hal_service`
  (`rootless/hal_service.cpp`) registers the 64-bit passthrough
  implementations instead (audioserver and AudioService wait for them).
  `waydroid_arm64_only` images ship a 64-bit service (and a 64-bit
  mediaserver), which is kept.
- Hosts without AArch32 also drop the image's other 32-bit-only vendor
  services (64_32 images: omx, camera provider, cas): their rc files and
  VINTF entries go, so system_server does not wait forever for a
  declared IOmxStore and init does not keep restarting them. statsd is
  disabled (it cannot find libstatssocket and restarts every few
  seconds).
- No keychords: `vine-rootless.rc` overrides bugreport without its
  `keycodes`, else init opens the input FIFOs and blocks until the
  hwcomposer writes one. `/dev/input` starts empty each boot.
- Boot signal: `vine-rootless.rc` writes `/run/user/1000/boot_completed`
  on `sys.boot_completed=1`; the engine reports the instance running when
  it appears.
- Socket paths: an app data dir leaves no room in `sun_path` (108 bytes)
  for `<instance>/rootless/run/pulse/native` and the like. binderd,
  vine_wl and vine_pulse run with their cwd in `<instance>/rootless` and
  listen on relative paths; the binder shim connects through the bind's
  directory fd when the full path does not fit.
- First boot: seed `/data/system/appops.xml` with one op SystemUI
  watches (the system package's SYSTEM_ALERT_WINDOW). LineageOS 20's
  `AppOpsControllerImpl.fetchCurrentActiveOps` iterates
  `getPackagesForOps()` without a null check and SystemUI crash-loops
  while no app has used one of those ops.
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
- selinuxfs: `/sys/fs/selinux/{status,enforce}` read as permissive
  (installd and libselinux's status page).
- Netlink: denied binds succeed, and NFLOG/NETFILTER sockets get a
  loopback stub in init sessions (netd exits on either failure).
- `/proc/<pid>/status` shows virtual Uid/Gid: ActivityManager checks a
  provider host's Uid line, and detached SettingsProvider (hosted in
  system_server) as "crashing" without it.
- SO_PEERCRED reports the fake init as pid 1: init's property service
  took its own "restorecon done" set for a new request and looped
  forever, leaving vold (and system_server's unlock) waiting.
- Abstract unix sockets are one namespace for every app: each guest
  app's ADB-JDWP thread reached the host adbd's `@jdwp-control`, was
  refused and reconnected at once, burning the phone's CPU (adbd plus
  every guest app). An init session now prefixes its abstract names with
  `tawc<init pid>.`, invisibly to the guest.
- init spun a whole core: its MountHandler rewinds its /proc/mounts fd
  instead of reopening it, so the mount-change wake tawcroot emulates
  (an inotify watch, drained only on reopen) never cleared after the
  first change. A rewind (lseek, trapped on synthesized mount fds)
  now renders the table afresh and acknowledges the change, as the
  kernel does.
- set*id privilege followed euid 0 only: webview_zygote is not root but
  holds CAP_SETUID/CAP_SETGID (emulated) to move its children to
  isolated uids, and every WebView aborted on `setresgid(99001)`.
- llvmpipe's rasterizer threads now and then ran a shader whose JIT code
  was already released (SEGV_ACCERR in `llvmpipe-N`, taking
  SurfaceFlinger or SystemUI down); `vine-rootless.rc` exports
  `LP_NUM_THREADS=0` so drawing threads rasterize themselves.
- binderd freed a transaction's target node while the buffer still
  pointed at it (`free_buffer` then touched freed memory); a buffer now
  pins its target node.
- The binder shim's thread table lives in memory a vfork child shares: a
  child closing the binder fd (ART's Runtime.exec child closes every fd)
  tore down the parent's binder threads, which then failed with EIO
  (SurfaceFlinger aborted; init restarted zygote with it).

## Known gaps, next

- Now and then system_server's watchdog fired with ActivityManager's lock
  held by a binder thread in a oneway `scheduleStopService` that never
  completes (after an app death). Not seen since the target node fix
  above; binderd's SIGUSR1 state dump is the starting point if it returns.
- Some Waydroid vendor HALs are 32-bit only (camera, cas, media.omx): no
  camera on 64-bit-only hosts without a rebuild.
- mdnsd aborts on `FD_SET` with fd 1056+: guest-visible fds at or above
  tawcroot's reserved floor (1000) break `select()` users.
- statsd and others fail on missing `libstatssocket` (APEX libs not
  reachable in the default namespace) and on iptables.
