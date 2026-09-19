#!/system/bin/sh
# Offline VM dependency check, not a phone suspend/charging test.
set -eu
grep -q virt /proc/device-tree/model || exit 50
echo A6L_NATIVE_BOOTSTRAP_BEGIN
mkdir -p /metadata/aconfig/flags /metadata/aconfig/maps /metadata/aconfig/boot
chmod 0775 /metadata/aconfig /metadata/aconfig/maps /metadata/aconfig/boot
chmod 0770 /metadata/aconfig/flags
chown -R 1000:1000 /metadata/aconfig
# Genuine post-fs initialization handles the absence of an early-init marker.
if timeout --foreground -k 3 30 /system/bin/aconfigd-system platform-init > /logs/aconfig-init.log 2>&1; then
    ls -l /metadata/aconfig/boot > /logs/aconfig-files.txt
    if [ -s /metadata/aconfig/boot/system.val ]; then
        echo A6L_ACONFIG_STORAGE_PASS
    else
        echo A6L_ACONFIG_STORAGE_FAILED reason=missing_system_values
    fi
else
    echo A6L_ACONFIG_STORAGE_FAILED reason=initializer_exit
fi
# Capture independent suspend/runtime results even if flag initialization fails.
# /sys is read-only in this VM. AOSP SystemSuspend explicitly supports this
# configuration for virtual devices: real Binder/wakelocks, no host suspend.
timeout --foreground -k 3 1500 /system/bin/hw/android.system.suspend-service > /logs/system-suspend.log 2>&1 &
suspend_pid=$!
i=0
while [ "$i" -lt 15 ]; do
    service check android.system.suspend.ISystemSuspend/default > /logs/suspend-binder.txt
    if grep -q ': found' /logs/suspend-binder.txt; then
        echo A6L_SYSTEM_SUSPEND_SERVICE_PASS
        exit 0
    fi
    kill -0 "$suspend_pid" || exit 52
    sleep 1
    i=$((i+1))
done
exit 53
