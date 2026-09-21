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
    /system/bin/a6l-guard.sh 2>/dev/null || { [ -e /tmp/a6l-framework-phone-approved ] || grep -q virt /proc/device-tree/model 2>/dev/null || { echo A6L_RI_FAIL guard; exit 2; }; }
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
$T losetup -r /dev/loop5 "$P/system.erofs" && $T mount -t erofs -o ro /dev/loop5 $R || { echo A6L_RI_FAIL system mount; exit 21; }
$T losetup -r /dev/loop6 "$P/vendor.erofs" && $T mount -t erofs -o ro /dev/loop6 $R/vendor || { echo A6L_RI_FAIL vendor mount; exit 22; }
$T mount -t tmpfs -o mode=0755,nosuid tmpfs $R/dev || exit 23
$T mkdir -p $R/dev/pts $R/dev/socket $R/dev/dm-user
$T mount -t devpts -o mode=0600,ptmxmode=0000 devpts $R/dev/pts
for n in "kmsg 1 11 0600" "kmsg_debug 1 11 0622" "null 1 3 0666" "random 1 8 0666" "urandom 1 9 0666" "ptmx 5 2 0666" "tty 5 0 0666" "console 5 1 0600"; do
    set -- $n; $T mknod -m $4 $R/dev/$1 c $2 $3
done
$T mount -t proc -o hidepid=2,gid=3009 proc $R/proc || exit 24          # fresh proc: shows the new PID namespace
$T mount -t sysfs sysfs $R/sys || exit 25
$T mount -t selinuxfs selinuxfs $R/sys/fs/selinux || exit 26
$T mount -t tmpfs -o mode=0755,uid=0,gid=1000,nosuid,nodev,noexec tmpfs $R/mnt
$T mkdir -p $R/mnt/vendor $R/mnt/product
for d in debug_ramdisk second_stage_resources metadata; do [ -d $R/$d ] && $T mount -t tmpfs -o mode=0755,nosuid tmpfs $R/$d; done
$T mount -t tmpfs -o mode=0771,uid=1000,gid=1000,nosuid,nodev,size=1536m tmpfs $R/data || exit 27
echo A6L_RI_EXEC_INIT > $R/dev/kmsg
cd $R && exec $T chroot $R /system/bin/init selinux_setup
