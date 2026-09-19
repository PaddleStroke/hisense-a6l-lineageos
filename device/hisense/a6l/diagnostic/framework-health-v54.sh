#!/system/bin/sh
# Diskless VM only: real AOSP Health HAL with no physical power supplies.
set -eu
grep -q virt /proc/device-tree/model || exit 60
echo A6L_HEALTH_BEGIN
timeout --foreground -k 3 300 /vendor/bin/hw/android.hardware.health-service.example > /logs/health-hal.log 2>&1 &
health_pid=$!
i=0
while [ "$i" -lt 15 ]; do
    service check android.hardware.health.IHealth/default > /logs/health-binder.txt
    if grep -q ': found' /logs/health-binder.txt; then
        timeout --foreground -k 3 5 dumpsys android.hardware.health.IHealth/default > /logs/health-dump.txt 2>&1
        echo A6L_HEALTH_SERVICE_PASS
        exit 0
    fi
    kill -0 "$health_pid" || exit 61
    sleep 1
    i=$((i+1))
done
exit 62
