#!/system/bin/sh
# Shared guard for the framework-*.sh helpers: QEMU "virt", or the approved attended phone run on V68.
grep -q virt /proc/device-tree/model 2>/dev/null && exit 0
[ "$(tr -d '\\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v68 ] && [ -e /tmp/a6l-framework-phone-approved ] && exit 0
exit 1
