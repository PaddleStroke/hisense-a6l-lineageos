#!/system/bin/sh
# Diskless VM only: real vold and idmap2d so StorageManagerService and overlay
# idmap requests have their genuine Binder peers. No block device, fstab entry,
# encryption or A6L storage behaviour is exercised or implied here.
set -eu
grep -q virt /proc/device-tree/model || exit 70
echo A6L_STORAGE_DAEMONS_BEGIN
mkdir -p /dev/block/vold /data/resource-cache /data/misc/vold /mnt/user /mnt/runtime /storage
if [ -s /logs/vold-early.pid ]; then
    vold_pid=$(cat /logs/vold-early.pid)   # V60+: started before apexd, which otherwise waits ~60 s for it
else
    timeout --foreground -k 3 400 /system/bin/vold \
        --blkid_context=u:r:blkid:s0 --blkid_untrusted_context=u:r:blkid_untrusted:s0 \
        --fsck_context=u:r:fsck:s0 --fsck_untrusted_context=u:r:fsck_untrusted:s0 > /logs/vold.log 2>&1 &
    vold_pid=$!
fi
# Normally lazily started through ctl.start; the VM has no init control path.
timeout --foreground -k 3 300 /system/bin/idmap2d > /logs/idmap2d.log 2>&1 &
idmap_pid=$!
i=0; vold_ok=0; idmap_ok=0
while [ "$i" -lt 20 ]; do
    [ "$vold_ok" -eq 1 ] || { service check vold | grep -q ': found' && { vold_ok=1; echo A6L_VOLD_SERVICE_PASS; }; } || true
    [ "$idmap_ok" -eq 1 ] || { service check idmap | grep -q ': found' && { idmap_ok=1; echo A6L_IDMAP_SERVICE_PASS; }; } || true
    [ "$vold_ok" -eq 1 ] && [ "$idmap_ok" -eq 1 ] && exit 0
    kill -0 "$vold_pid" || { echo A6L_VOLD_EXITED; cat /logs/vold.log; exit 71; }
    kill -0 "$idmap_pid" || { echo A6L_IDMAP_EXITED; cat /logs/idmap2d.log; exit 72; }
    sleep 1
    i=$((i+1))
done
cat /logs/vold.log /logs/idmap2d.log
exit 73
