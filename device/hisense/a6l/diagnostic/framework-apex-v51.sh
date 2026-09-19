#!/system/bin/sh
# Disposable, diskless QEMU only. Called within the guarded VM supervisor.
set -eu
grep -q virt /proc/device-tree/model || exit 40
echo A6L_APEX_BEGIN
mkdir -p /data/apex/active /data/apex/decompressed /data/apex/backup /data/apex/hashtree /data/apex/sessions /metadata/apex/sessions /metadata/apex/images /data/misc_de/0 /data/misc_ce/0
# Native guarded VM supervisor creates loop/mapper nodes without 256 execs.
[ -c /dev/loop-control ] && [ -c /dev/device-mapper ] || exit 41
# Remove the expanded-package fixture: only apexd may emit the new inventory.
rm /apex/apex-info-list.xml
cat /dev/kmsg > /logs/apex-kernel.log &
timeout --foreground -k 3 300 /system/bin/apexd > /logs/apexd.log 2>&1 &
apex_pid=$!
i=0
while [ "$i" -lt 80 ]; do
    state=$(getprop apexd.status)
    [ "$state" != activated ] || break
    kill -0 "$apex_pid" || exit 42
    sleep 1
    i=$((i+1))
done
[ "$(getprop apexd.status)" = activated ] || exit 43
service check apexservice | tee /logs/apex-binder.txt
grep -q ': found' /logs/apex-binder.txt || exit 44
[ -s /apex/apex-info-list.xml ] || exit 45
cp /apex/apex-info-list.xml /logs/apex-info-list.xml
cat /proc/mounts > /logs/apex-mounts.txt
while read name; do
    grep -F " /apex/$name " /proc/mounts | grep -q ' ro,' || exit 46
    grep -F "moduleName=\"$name\"" /apex/apex-info-list.xml | grep -q 'isActive="true"' || exit 47
done < /system/etc/a6l-expected-apexes.txt
echo A6L_APEX_MOUNTS_PASS
timeout --foreground -k 3 30 /system/bin/apexd --snapshotde > /logs/apex-snapshotde.log 2>&1
[ "$(getprop apexd.status)" = ready ] || exit 48
echo A6L_APEX_SERVICE_PASS
