#!/system/bin/sh
# Waydroid init under tawcroot in the app context (run-as me.vine.dev),
# with binderd and the guest's logcat. See README.md.
cd /data/user/0/me.vine.dev || exit 1
B=$PWD; W=$B/w; R=$W/rootfs
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
  # 64-bit-only zygote: the 64_32 one without its 32-bit secondary.
  Z=$R/system/etc/init/hw
  [ -f "$Z/init.zygote64.rc" ] || sed '/^service zygote_secondary/,$d' "$Z/init.zygote64_32.rc" > "$Z/init.zygote64.rc"
  chmod 644 "$Z/init.zygote64.rc"   # init skips group/world-writable rc files
fi
# lmkd needs PSI or memcg, both closed to apps (the host's lmkd covers
# us): keep it from being critical so its exits don't abort boot.
P=$R/system/product/etc/init   # $R/product is an absolute symlink
mkdir -p "$P"
printf '%s\n' \
  'service lmkd /system/bin/lmkd' \
  '    class core' \
  '    user lmkd' \
  '    group lmkd system readproc' \
  '    capabilities DAC_OVERRIDE KILL IPC_LOCK SYS_NICE SYS_RESOURCE' \
  '    socket lmkd seqpacket+passcred 0660 system system' \
  '    task_profiles ServiceCapacityLow' \
  '    oneshot' \
  '    override' \
  > "$P/vine-rootless.rc"
chmod 644 "$P/vine-rootless.rc"
# Waydroid's /dev nodes: libbinder access()es the driver before opening it.
for b in binder hwbinder vndbinder; do ln -sfn binderfs/$b "$R/dev/$b"; done
# What first-stage init would have created.
mkdir -p "$R/dev/socket" "$R/dev/pts" "$R/mnt/vendor" "$R/mnt/product"
( ${WRAP:+$B/$WRAP} ${BINDERD:-bin/binderd} ${BINDERD_FLAGS:-} "$W/binderfs"; echo "--- binderd exited: $?" >&2 ) 2> "$W/binderd.log" &
BD=$!
i=0; while [ ! -e "$W/binderfs/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
# init (and every service it forks) starts at the guest's /.
(cd "$R" && exec ${WRAP:+$B/$WRAP} $B/bin/tawcroot -r $R --init --all-caps ${CGROUP_FLAG---cgroup} --owners -b $W/binderfs:/dev/binderfs -b $W/data:/data -b /proc:/proc -b /sys:/sys -b /dev/null:/dev/null -b /dev/zero:/dev/zero -b /dev/random:/dev/random -b /dev/urandom:/dev/urandom -b /dev/ptmx:/dev/ptmx -b /dev/pts:/dev/pts -- /system/bin/init ${INIT_ARGS:-second_stage}) > "$W/init.log" 2>&1 &
IP=$!
# The guest's own log, read through its logd.
( sleep ${LOGCAT_DELAY:-4}; exec bin/tawcroot -r $R -b /proc:/proc -b /dev/null:/dev/null -- /system/bin/logcat -b all -v brief ) > "$W/logcat.txt" 2>&1 &
LP=$!
sleep ${T:-20}
if kill -0 $IP 2>/dev/null; then echo "--- init alive: yes"; else wait $IP; echo "--- init exited: $?"; fi
echo "--- init.log"; tail -40 "$W/init.log"
echo "--- kmsg"; tr "\0" "\n" < "$R/dev/kmsg" | grep -v "Reboot ending\|reboot call returned\|InitFatalReboot" | head -${KMSG_LINES:-40}
echo "--- binderd.log"; tail -5 "$W/binderd.log"
kill -9 $IP $LP 2>/dev/null; pkill -f "bin/binderd" 2>/dev/null
