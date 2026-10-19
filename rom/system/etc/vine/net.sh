#!/system/bin/sh
# Repairs eth0 from the boot props, the engine already configured it before init
ip_addr="$(getprop ro.boot.vine.net_ip)"
gw="$(getprop ro.boot.vine.net_gw)"
dns="$(getprop ro.boot.vine.net_dns)"
[ -n "$ip_addr" ] || exit 0

i=0
while [ ! -d /sys/class/net/eth0 ] && [ $i -lt 20 ]; do
    sleep 1
    i=$((i + 1))
done
[ -d /sys/class/net/eth0 ] || { log -t vine-net "no eth0, running without network"; exit 0; }

ip link set eth0 up
ip addr show dev eth0 | grep -q "inet $ip_addr/" || ip addr add "$ip_addr/30" dev eth0
ip route show default | grep -q . || ip route add default via "$gw" dev eth0
[ -n "$dns" ] && setprop net.dns1 "$dns" && setprop net.eth0.dns1 "$dns"
log -t vine-net "eth0 $ip_addr via $gw dns $dns"
