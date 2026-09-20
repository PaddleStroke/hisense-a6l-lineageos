#!/system/bin/sh
# Diskless VM only: genuine netd with init-style control sockets. The VM has no
# network interface; this checks Binder registration and firewall/BPF setup on
# the V59 runtime kernel, not A6L Wi-Fi, modem or tethering behaviour.
set -eu
/system/bin/a6l-guard.sh || exit 80
echo A6L_NETD_BEGIN
grep -q ' /sys/fs/cgroup cgroup2 ' /proc/mounts || { mkdir -p /sys/fs/cgroup; mount -t cgroup2 none /sys/fs/cgroup || echo A6L_NETD_CGROUP2_MOUNT_FAILED; }
mkdir -p /data/misc/net /data/misc/netd /data/misc/ethernet
iptables -w -t filter -L -n > /logs/iptables-filter.txt 2>&1 && echo A6L_IPTABLES_FILTER_PASS || echo A6L_IPTABLES_FILTER_FAILED
ip6tables -w -t mangle -L -n > /logs/ip6tables-mangle.txt 2>&1 && echo A6L_IP6TABLES_MANGLE_PASS || echo A6L_IP6TABLES_MANGLE_FAILED
ulimit -l 1048576
timeout --foreground -k 3 1500 /system/bin/a6l_socket_exec dnsproxyd:0666 mdns:0666 fwmarkd:0666 -- /system/bin/netd > /logs/netd.log 2>&1 &
netd_pid=$!
i=0
while [ "$i" -lt 40 ]; do
    if service check netd | grep -q ': found'; then
        echo A6L_NETD_SERVICE_PASS
        service check dnsresolver | grep -q ': found' && echo A6L_DNSRESOLVER_SERVICE_PASS || true
        exit 0
    fi
    kill -0 "$netd_pid" || { echo A6L_NETD_EXITED; tail -n 40 /logs/netd.log; exit 81; }
    sleep 1
    i=$((i+1))
done
tail -n 40 /logs/netd.log
exit 82
