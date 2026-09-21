#!/system/bin/sh
# Shared guard for the framework-*.sh helpers: QEMU "virt", or the approved attended phone run on V68.
grep -q virt /proc/device-tree/model 2>/dev/null && exit 0
case "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" in v68|v69|v70|v71) ;; *) exit 1;; esac; [ 1 = 1 ] && [ -e /tmp/a6l-framework-phone-approved ] && exit 0
exit 1
