# On the phone: a request to the debug app's rootless helper (pushed by hs.sh)
LIB=$(for p in /proc/[0-9]*; do [ -r $p/cmdline ] || continue; c=$(tr 2>/dev/null '\0' ' ' < $p/cmdline 2>/dev/null); case $c in *libvine_helper.so*--rootless*) echo ${c%%/libvine_helper.so*}; break;; esac; done)
cd /data/user/0/com.hexadecinull.vineos.debug/files && exec $LIB/libvine_helper.so --send rootless.sock "$@"
