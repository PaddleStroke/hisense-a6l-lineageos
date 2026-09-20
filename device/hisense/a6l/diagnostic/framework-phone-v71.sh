#!/system/bin/sh
# framework-phone-v71.sh — ATTENDED ONLY. First run of the VM-validated framework payload on the spare phone,
# entirely from RAM under the V68 diagnostic recovery. NOT YET RUN ON THE PHONE.
#
# Safety properties:
#  * refuses unless DT marker = v68 AND the host runner created /tmp/a6l-framework-phone-approved;
#  * everything happens in a private mount namespace (adbd's view of / is untouched);
#  * the supervisor replaces /dev with a private tmpfs: no eMMC/partition node exists for any Android daemon;
#  * the supervisor binds /sys read-only: no suspend, regulator, backlight or charger writes;
#  * payload is one read-only EROFS image verified against SHA256SUMS; all writes land in tmpfs;
#  * hard time limit (default 20 min); nothing persists across the reboot back to stock.
# usage (host): adb push bundle/* /tmp/a6l-fw/ ; adb shell touch /tmp/a6l-framework-phone-approved ;
#               adb shell /system/bin/sh /tmp/a6l-fw/framework-phone-v71.sh
set -u
P=${P:-/tmp/a6l-fw}
LIMIT_S=${LIMIT_S:-1200}
if [ "${1:-}" != --inner ]; then
    case "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" in v68|v69) ;; *) echo A6L_PHONE_FW_FAIL wrong image; exit 2;; esac
    [ -e /tmp/a6l-framework-phone-approved ] || { echo A6L_PHONE_FW_FAIL not approved by the attended host runner; exit 3; }
    [ "$(id -u)" = 0 ] || { echo A6L_PHONE_FW_FAIL root required; exit 4; }
    ( cd "$P" && /system/bin/toybox sha256sum -c SHA256SUMS ) || { echo A6L_PHONE_FW_FAIL payload hash; exit 5; }
    free_kb=$(/system/bin/toybox grep MemAvailable /proc/meminfo | /system/bin/toybox tr -dc 0-9)
    echo "A6L_PHONE_FW_MEMAVAILABLE_KB=$free_kb"
    [ "$free_kb" -ge 2500000 ] || { echo A6L_PHONE_FW_FAIL less than 2.5 GB available; exit 6; }
    mkdir -p "$P/logs"
    exec /system/bin/toybox unshare -m /system/bin/sh "$0" --inner > "$P/logs/console.log" 2>&1
fi
echo A6L_PHONE_FW_INNER_START
/system/bin/toybox mount -o rprivate none / || exit 20   # toybox syntax; the recovery root is a shared mount
mkdir -p /v48/lower /v48/rw /v48/payload
[ -e /dev/loop-control ] || /system/bin/toybox mknod /dev/loop-control c 10 237
[ -e /dev/loop7 ] || /system/bin/toybox mknod /dev/loop7 b 7 7
/system/bin/toybox losetup -r /dev/loop7 "$P/payload.erofs" || exit 30
/system/bin/toybox mount -t erofs -o ro /dev/loop7 /v48/lower || exit 31
/system/bin/toybox grep -q '^overlay ' /proc/modules || /system/bin/toybox insmod "$P/overlay.ko" || exit 32
/system/bin/toybox mount -t tmpfs -o size=1024m tmpfs /v48/rw || exit 33
mkdir -p /v48/rw/upper /v48/rw/work
/system/bin/toybox mount -t overlay -o lowerdir=/v48/lower,upperdir=/v48/rw/upper,workdir=/v48/rw/work overlay /v48/payload || exit 34
echo A6L_EROFS_DELIVERY_PASS
# r3 (20 Sep phone run 1): the generated data-directory helper inside the image still carried an inline
# "QEMU only" guard (exit 97), so user storage could not be prepared. Patch it through the writable overlay.
h=/v48/payload/root/system/bin/framework-datadirs-v64.sh
[ -f "$h" ] && /system/bin/toybox sed -i 's#^grep -q virt /proc/device-tree/model || exit 97$#/system/bin/a6l-guard.sh || exit 97#' "$h" && echo A6L_PHONE_FW_DATADIRS_GUARD_PATCHED
# Optional GPU userspace (A6L_EGL=mesa): only when the msm render node exists; ANGLE/SwiftShader stays the default.
if [ "${A6L_EGL:-angle}" = mesa ] && [ -e /sys/class/drm/renderD128 ] && [ -e /v48/payload/root/vendor/lib64/egl/libEGL_mesa.so ]; then
    printf 'ro.hardware.egl=mesa\ndebug.renderengine.backend=skiaglthreaded\ndebug.hwui.renderer=skiagl\n' >> /v48/payload/root/system/etc/a6l-runtime.prop
    echo A6L_PHONE_FW_EGL=mesa
else
    echo A6L_PHONE_FW_EGL=angle
fi
# Keep the kernel log off the framebuffer console while Android owns the display (restored on exit).
old_printk=$(/system/bin/toybox cat /proc/sys/kernel/printk); echo 1 > /proc/sys/kernel/printk
/system/bin/toybox grep -q '^a6l_simplefb ' /proc/modules || /system/bin/toybox insmod /v48/payload/a6l_simplefb.ko || echo A6L_PHONE_FW_NOTE simplefb module not loaded
for part in apex vendor system_ext product system; do
    mkdir -p /$part; /system/bin/toybox mount --bind /v48/payload/root/$part /$part || exit 40
done
/system/bin/toybox mount --bind /system/etc /etc || exit 41
mkdir -p /logs
( sleep "$LIMIT_S"; echo A6L_PHONE_FW_TIME_LIMIT; /system/bin/toybox killall -9 framework-services 2>/dev/null ) &
watchdog=$!
/v48/payload/bin/framework-services
result=$?
kill "$watchdog" 2>/dev/null
echo A6L_PHONE_FW_SUPERVISOR_EXIT=$result
echo "$old_printk" > /proc/sys/kernel/printk
for f in /logs/*; do echo "LOGFILE:$f"; /system/bin/toybox cat "$f"; done
echo A6L_PHONE_FW_DONE
