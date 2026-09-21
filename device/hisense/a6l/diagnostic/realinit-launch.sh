#!/system/bin/sh
# realinit-launch.sh — run the REAL Android init (selinux_setup -> second_stage) from RAM, as PID 1 of a private
# PID + mount namespace, instead of the hand-written supervisor (framework-services). DRAFT, VM first.
#
# Why a container: on the phone everything is RAM-only for now, so PID 1 stays the diagnostic recovery's init and the
# system image arrives over ADB after boot. Android's init only needs to BE pid 1 in its own namespace. The same
# system.erofs/vendor.erofs later become the real partitions, where the normal first stage replaces this script.
#
# This script does what first-stage init would have done (system/core/init/first_stage_init.cpp): mount the labeled
# system-as-root image, vendor, /dev tmpfs + basic nodes, devpts, proc, sysfs, selinuxfs, /mnt, /metadata, /data (tmpfs),
# /debug_ramdisk and /second_stage_resources, then exec init.
# NOTE selinux_setup loads the image's policy KERNEL-WIDE (not namespaced). Boot cmdline must carry
# androidboot.selinux=permissive (the recovery cmdline does), otherwise the host's adbd would be confined.
# usage: P=/tmp/a6l-ri sh realinit-launch.sh      (needs $P/system.erofs, $P/vendor.erofs, optional $P/overlay.ko)
set -u
T=/system/bin/toybox
P=${P:-/tmp/a6l-ri}
if [ "${1:-}" != --inner ]; then
    /system/bin/a6l-guard.sh 2>/dev/null || { [ -e /tmp/a6l-framework-phone-approved ] || $T grep -q dummy-virt /proc/device-tree/compatible 2>/dev/null || { echo A6L_RI_FAIL guard; exit 2; }; }
    $T grep -q 'androidboot.selinux=permissive' /proc/cmdline || { echo A6L_RI_FAIL cmdline is not permissive; exit 3; }
    $T mkdir -p "$P/logs"
    exec $T unshare -m -p -f /system/bin/sh "$0" --inner
fi
echo A6L_RI_INNER pid=$$
$T mount -o rprivate none / || exit 20
R=/ri; $T mkdir -p $R
ln() { n=$1; [ -e /dev/loop$n ] || $T mknod /dev/loop$n b 7 $n; }
[ -e /dev/loop-control ] || $T mknod /dev/loop-control c 10 237
ln 5; ln 6
# The image root is read-only and lacks the mount points first-stage init creates in its ramdisk (/bootstrap-apex, ...).
# r3 finding: an overlayfs root is rejected by Android 16's overlay_remounter during selinux_setup. So build a tmpfs
# skeleton root instead: every top-level directory of the image is bind-mounted, symlinks/files are copied.
$T mkdir -p /ri-lower
$T losetup -r /dev/loop5 "$P/system.erofs" && $T mount -t erofs -o ro /dev/loop5 /ri-lower || { echo A6L_RI_FAIL system mount; exit 21; }
$T mount -t tmpfs -o size=16m,mode=0755 tmpfs $R || exit 21
for e in /ri-lower/* /ri-lower/.[!.]*; do
    [ -e "$e" ] || [ -L "$e" ] || continue; n=${e##*/}
    if [ -L "$e" ]; then $T cp -a "$e" "$R/$n"
    elif [ -d "$e" ]; then $T mkdir -p "$R/$n"; $T mount --bind "$e" "$R/$n" || { echo "A6L_RI_FAIL bind $n"; exit 21; }
    else $T cp -a "$e" "$R/$n"; fi
done
for d in bootstrap-apex debug_ramdisk second_stage_resources metadata mnt data apex linkerconfig tmp dev proc sys vendor; do [ -e $R/$d ] || $T mkdir -p $R/$d; done
$T losetup -r /dev/loop6 "$P/vendor.erofs" && $T mount -t erofs -o ro /dev/loop6 $R/vendor || { echo A6L_RI_FAIL vendor mount; exit 22; }
$T mount -t tmpfs -o mode=0755,nosuid tmpfs $R/dev || exit 23
$T mkdir -p $R/dev/pts $R/dev/socket $R/dev/dm-user
$T mount -t devpts -o mode=0600,ptmxmode=0000 devpts $R/dev/pts
for n in "kmsg 1 11 0600" "kmsg_debug 1 11 0622" "null 1 3 0666" "random 1 8 0666" "urandom 1 9 0666" "ptmx 5 2 0666" "tty 5 0 0666" "console 5 1 0600"; do
    set -- $n; $T mknod -m $4 $R/dev/$1 c $2 $3
done
$T mount -t proc -o hidepid=2,gid=3009 proc $R/proc || exit 24          # fresh proc: shows the new PID namespace
# r5 finding: the recovery cmdline carries androidboot.init_rc=..., which makes init parse ONLY that file (no
# /system/etc/init, no services). Give the container a filtered view of /proc/cmdline.
$T sed 's# androidboot.init_rc=[^ ]*##' /proc/cmdline > /ri-cmdline && $T mount --bind /ri-cmdline $R/proc/cmdline || exit 24
# r6 finding: the GSI system image reboots to the bootloader when ro.vndk.version is undefined (init.vndk-nodef.rc).
# Bring-up only: mask that rc with an empty file. (A real lineage_a6l product is not a GSI and will not carry it.)
# Bring-up visibility: an extra rc (bound over an unused debug rc) streams logcat errors to the kernel log and marks boot completion.
# (no heredoc: mksh needs a writable TMPDIR for those, which the recovery does not have)
$T printf '%s\n' 'service a6l_logcat /system/bin/logcat -b main,system,crash -v brief *:E' '    stdio_to_kmsg' '    user root' '    group root log' '    seclabel u:r:su:s0' '    disabled' 'on init' '    start a6l_logcat' 'on property:sys.boot_completed=1' '    write /dev/kmsg "A6L_RI_BOOT_COMPLETED"' > /ri-debug.rc
f=$R/system/etc/init/bootstat-debug.rc; [ -f $f ] && $T mount --bind /ri-debug.rc $f
echo "A6L_RI_DEBUGRC size=$($T wc -c < /ri-debug.rc) bound=$($T grep -c a6l_logcat $f)"
: > /ri-empty.rc; f=$R/system/system_ext/etc/gsi/init.vndk-nodef.rc; [ -f $f ] && { $T mount --bind /ri-empty.rc $f || exit 24; }
$T mount -t sysfs sysfs $R/sys || exit 25
$T mount -t selinuxfs selinuxfs $R/sys/fs/selinux || exit 26
$T mount -t tmpfs -o mode=0755,uid=0,gid=1000,nosuid,nodev,noexec tmpfs $R/mnt
$T mkdir -p $R/mnt/vendor $R/mnt/product
for d in debug_ramdisk second_stage_resources metadata; do [ -d $R/$d ] && $T mount -t tmpfs -o mode=0755,nosuid tmpfs $R/$d; done
$T mount -t tmpfs -o mode=0771,uid=1000,gid=1000,nosuid,nodev,size=1536m tmpfs $R/data || exit 27
echo A6L_RI_EXEC_INIT > $R/dev/kmsg
# r4 finding: plain chroot is not enough. init creates a second mount namespace and setns() back, which resets its root to
# the namespace root (the recovery initramfs). pivot_root cannot leave an initramfs, so do what switch_root does: move the
# new root on top of "/" in this private namespace (setns follows the stacked mount), then chroot into it.
cd $R && $T mount --move $R / || { echo A6L_RI_FAIL move root; exit 28; }
# r7-r10 finding: started from an adb/su shell, init stays in u:r:su:s0 and no service gets its domain ("no domain transition
# from u:r:su:s0"), which breaks logd/property/servicemanager peers even in permissive mode. Real first-stage init runs as
# u:r:kernel:s0 and transitions to u:r:init:s0 on the re-exec after the policy load, so start it there.
exec $T runcon u:r:kernel:s0 $T chroot . /system/bin/init selinux_setup
