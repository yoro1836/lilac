#!/system/bin/sh
# Waydroid init under tawcroot in the app context (run-as me.vine.dev),
# with binderd and the guest's logcat. See README.md.
cd /data/user/0/me.vine.dev || exit 1
B=$PWD; W=$B/w; R=$W/rootfs
# Every guest process, reparented ones included (zygotes outlive init).
guests_kill() {
  for p in /proc/[0-9]*; do
    [ "$(readlink "$p/exe" 2>/dev/null)" = "$B/bin/tawcroot" ] && kill -9 "${p#/proc/}" 2>/dev/null
  done
}
guests_kill
for a in "$R"/system/apex/*; do n=${a##*/}; [ -L "$R/apex/$n" ] || ln -s "/system/apex/$n" "$R/apex/$n"; done
rm -rf "$W/binderfs"; mkdir -p "$W/binderfs" "$W/data"
for d in null zero random urandom kmsg; do [ -f "$R/dev/$d" ] && rm -f "$R/dev/$d"; done
: > "$R/dev/kmsg"
# Runtime state of the previous boot: the guest's /dev and tawcroot's
# per-session store files (the ownership table stays).
rm -rf "$R/dev/__properties__" "$R/dev/cgroup_info" "$R/dev/.booting" "$R/dev/socket" "$R/dev/pts"
(cd "$W/tawcroot" && rm -rf mounts mounts.event mnt cgroup cgroup.registry uts ids)
# A host without AArch32 can't run the ROM's 32-bit ABIs: present it
# as 64-bit only, like the host.
if [ -z "$(getprop ro.product.cpu.abilist32)" ]; then
  for f in "$R/system/build.prop" "$R/vendor/build.prop" "$R/vendor/odm/etc/build.prop"; do
    [ -f "$f" ] && sed -i -e 's/^\(ro\.[a-z]*\.product\.cpu\.abilist\)=.*/\1=arm64-v8a/' \
      -e 's/^\(ro\.[a-z]*\.product\.cpu\.abilist32\)=.*/\1=/' \
      -e 's/^ro\.zygote=.*/ro.zygote=zygote64/' "$f"
  done
  # The audio HAL service is 32-bit only; its 64-bit implementations
  # are served by our own passthrough service (vine-rootless.rc).
  cp bin/vine_hal_service "$R/vendor/bin/hw/vine_hal_service"
  chmod 755 "$R/vendor/bin/hw/vine_hal_service"
  # 64-bit-only zygote: the 64_32 one without its 32-bit secondary.
  Z=$R/system/etc/init/hw
  [ -f "$Z/init.zygote64.rc" ] || sed '/^service zygote_secondary/,$d' "$Z/init.zygote64_32.rc" > "$Z/init.zygote64.rc"
  chmod 644 "$Z/init.zygote64.rc"   # init skips group/world-writable rc files
fi
# lmkd needs PSI or memcg, both closed to apps (the host's lmkd covers
# us): a stand-in answers system_server on its socket instead.
cp bin/vine_lmkd "$R/system/bin/vine_lmkd"
chmod 755 "$R/system/bin/vine_lmkd"
P=$R/system/product/etc/init   # $R/product is an absolute symlink
mkdir -p "$P"
printf '%s\n' \
  'service lmkd /system/bin/vine_lmkd' \
  '    class core' \
  '    user lmkd' \
  '    group lmkd system readproc' \
  '    capabilities DAC_OVERRIDE KILL IPC_LOCK SYS_NICE SYS_RESOURCE' \
  '    socket lmkd seqpacket+passcred 0660 system system' \
  '    task_profiles ServiceCapacityLow' \
  '    override' \
  '' \
  'service vendor.audio-hal /vendor/bin/hw/vine_hal_service android.hardware.audio@4.0::IDevicesFactory android.hardware.audio.effect@4.0::IEffectsFactory' \
  '    class hal' \
  '    user audioserver' \
  '    group audio camera drmrpc inet media mediadrm net_bt net_bt_admin net_bw_acct wakelock context_hub' \
  '    capabilities BLOCK_SUSPEND SYS_NICE' \
  '    onrestart restart audioserver' \
  '    override' \
  '' \
  'service bugreport /system/bin/dumpstate -d -p -z' \
  '    oneshot' \
  '    disabled' \
  '    override' \
  > "$P/vine-rootless.rc"
chmod 644 "$P/vine-rootless.rc"
# What Waydroid's host writes: no GPU device, so software GLES (ANGLE on
# lavapipe) over memfd-backed gralloc buffers.
printf '%s\n' \
  'ro.hardware.gralloc=default' \
  'ro.hardware.egl=angle' \
  'ro.hardware.vulkan=lvp' \
  'sys.use_memfd=true' \
  'ro.opengles.version=196610' \
  "ro.sf.lcd_density=${DENSITY:-320}" \
  'waydroid.background_start=false' \
  'waydroid.stub_sensors_hal=1' \
  > "$R/vendor/waydroid.prop"
chmod 644 "$R/vendor/waydroid.prop"
# A first boot has no app ops recorded, and LineageOS 20's SystemUI then
# crashes on getPackagesForOps() returning null (fetchCurrentActiveOps).
# Seed one that persists: SYSTEM_ALERT_WINDOW allowed for the system package.
if [ ! -e "$W/data/system/appops.xml" ]; then
  mkdir -p "$W/data/system"
  printf '%s\n' "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>" \
    '<app-ops v="1">' '<pkg n="android">' '<uid n="1000">' '<op n="24" m="0" />' '</uid>' '</pkg>' '</app-ops>' \
    > "$W/data/system/appops.xml"
fi
# Waydroid's /dev nodes: libbinder access()es the driver before opening it.
for b in binder hwbinder vndbinder; do ln -sfn binderfs/$b "$R/dev/$b"; done
# What first-stage init would have created.
mkdir -p "$R/dev/socket" "$R/dev/pts" "$R/mnt/vendor" "$R/mnt/product"
# The display: hwcomposer.waydroid connects to /run/user/1000/wayland-0,
# frames land in w/vinefb, input lines go to w/input (vine_wl --send).
# The hwcomposer hands touches and keys to InputFlinger through FIFOs
# it makes in /dev/input.
# No keychords (bugreport's are overridden above): init would open those
# FIFOs and block until the hwcomposer writes one.
rm -rf "$R/dev/input"
mkdir -p "$W/run" "$R/run/user/1000" "$R/dev/input"
bin/vine_wl --socket "$W/run/wayland-0" --vinefb "$W/vinefb" --create "${SIZE:-720x1440}" --input "$W/input" 2> "$W/vine_wl.log" &
WL=$!
# Audio: the audio HAL's libpulse connects to /run/user/1000/pulse/native.
mkdir -p "$W/run/pulse"
bin/vine_pulse --socket "$W/run/pulse/native" ${PULSE_FLAGS:-} 2> "$W/vine_pulse.log" &
PA=$!
( ${WRAP:+$B/$WRAP} ${BINDERD_WRAP:+$B/$BINDERD_WRAP} ${BINDERD:-bin/binderd} ${BINDERD_FLAGS:-} "$W/binderfs"; echo "--- binderd exited: $?" >&2 ) 2> "$W/binderd.log" &
BD=$!
i=0; while [ ! -e "$W/binderfs/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
# init (and every service it forks) starts at the guest's /.
(cd "$R" && exec ${WRAP:+$B/$WRAP} $B/bin/tawcroot -r $R --init --all-caps ${CGROUP_FLAG---cgroup} --owners -b $W/binderfs:/dev/binderfs -b $W/data:/data -b /proc:/proc -b /sys:/sys -b /dev/null:/dev/null -b /dev/zero:/dev/zero -b /dev/random:/dev/random -b /dev/urandom:/dev/urandom -b /dev/ptmx:/dev/ptmx -b /dev/pts:/dev/pts -b $W/run:/run/user/1000 -- /system/bin/init ${INIT_ARGS:-second_stage}) > "$W/init.log" 2>&1 &
IP=$!
# The guest's own log, read through its logd.
( sleep ${LOGCAT_DELAY:-4}; exec bin/tawcroot -r $R -b /proc:/proc -b /dev/null:/dev/null -- /system/bin/logcat -b all -v brief ) > "$W/logcat.txt" 2>&1 &
LP=$!
sleep ${T:-20}
if kill -0 $IP 2>/dev/null; then echo "--- init alive: yes"; else wait $IP; echo "--- init exited: $?"; fi
# DIAG: shell commands run in the guest (binder too) before teardown
[ -n "${DIAG:-}" ] && (cd "$R" && exec $B/bin/tawcroot -r $R -b $W/binderfs:/dev/binderfs -b /proc:/proc -b /dev/null:/dev/null -- /system/bin/sh -c "$DIAG") > "$W/diag.txt" 2>&1
echo "--- init.log"; tail -40 "$W/init.log"
echo "--- kmsg"; tr "\0" "\n" < "$R/dev/kmsg" | grep -v "Reboot ending\|reboot call returned\|InitFatalReboot" | head -${KMSG_LINES:-40}
echo "--- binderd.log"; tail -5 "$W/binderd.log"
echo "--- vine_wl.log"; tail -5 "$W/vine_wl.log"
echo "--- vine_pulse.log"; tail -5 "$W/vine_pulse.log"
kill -9 $IP $LP 2>/dev/null; pkill -f -- "$W/binderfs" 2>/dev/null; kill $WL $PA 2>/dev/null
guests_kill
