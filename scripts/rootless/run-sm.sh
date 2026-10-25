#!/system/bin/sh
# Milestone 2: binderd + servicemanager under tawcroot, then `service list`.
cd /data/local/tmp/vine-dev || exit 1
W=$PWD/w; R=$W/rootfs
for a in "$R"/system/apex/*; do n=${a##*/}; [ -L "$R/apex/$n" ] || ln -s "/system/apex/$n" "$R/apex/$n"; done
for d in null zero random urandom; do [ -e "$R/dev/$d" ] || : > "$R/dev/$d"; done
rm -rf "$W/binderfs"; mkdir -p "$W/binderfs"
bin/binderd ${BINDERD_ARGS:-} "$W/binderfs" 2> "$W/binderd.log" &
echo $! > "$W/binderd.pid"
i=0; while [ ! -e "$W/binderfs/ready" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i+1)); done
TR="bin/tawcroot -r $R --owners -b $W/binderfs:/dev/binderfs -b /proc:/proc -b /sys:/sys -b /dev/null:/dev/null -b /dev/zero:/dev/zero -b /dev/random:/dev/random -b /dev/urandom:/dev/urandom"
$TR -- /system/bin/servicemanager > "$W/sm.log" 2>&1 &
echo $! > "$W/sm.pid"
sleep 2
echo "--- servicemanager alive: $(kill -0 $(cat $W/sm.pid) 2>/dev/null && echo yes || echo no)"
$TR -- /system/bin/service list 2>&1 | head -20
echo "--- sm.log"; tail -20 "$W/sm.log"
echo "--- binderd.log"; tail -20 "$W/binderd.log"
kill $(cat "$W/sm.pid") $(cat "$W/binderd.pid") 2>/dev/null
