#!/bin/bash
# diag-r5 LAPTOP, ATTENDED ONLY: write a 64 MiB image into the phone's BOOT partition from the V75 diagnostic recovery
# (or from a diag boot image, same RAM environment). Same method as the r6b update (adb push, sha on the phone, toybox dd).
#   1. phone checks: serial HLTE730T-PROBE, state recovery, eMMC driver (insmod /sdhci-msm.ko if needed),
#      boot = the partition with PARTNAME=boot, start sector 671744, 131072 sectors (64 MiB) - else STOP
#   2. backup: the current boot partition is read on the phone, sha256 printed, and copied to logs/boot-before-<UTC>.img
#      (--expect-current <sha>: STOP before writing if the current content differs)
#   3. push the image to /tmp (RAM), sha256 on the phone must equal the laptop sha - else STOP
#   4. toybox dd into the partition (conv=fsync), sync, drop caches, read the partition back: sha256 must equal
# Nothing else is written. No reboot is done here (reboot = long-press Power, see the doc).
# usage: host/diag-bootwrite.sh <image> [--expect-current <sha256>]
set -u
S=${ADB_SERIAL:-HLTE730T-PROBE}; cd "$(dirname "$0")/.." || exit 1
IMG=${1:?image}; EXP=; [ "${2:-}" = --expect-current ] && EXP=${3:?sha}
T=/system/bin/toybox
say() { echo "BOOTWRITE $*"; }
die() { echo "BOOTWRITE_FAIL $*"; exit ${2:-1}; }
sh1() { adb -s "$S" shell "$1" 2>/dev/null | tr -d '\r' | grep -E '^R ' | sed 's/^R //'; }   # phone lines prefixed "R "
[ -f "$IMG" ] || die "no $IMG"
[ "$(stat -c %s "$IMG")" = 67108864 ] || die "image is not 64 MiB"
NEW=$(sha256sum "$IMG" | cut -c1-64); say "image $IMG sha256 $NEW"
adb devices | grep -qE "^$S[[:space:]]+recovery" || die "phone $S not in recovery state (adb devices)"
# 1. partition
P=$(sh1 "$T grep -q '^sdhci_msm ' /proc/modules || { $T insmod /sdhci-msm.ko; $T sleep 6; }; for b in /sys/class/block/mmcblk*p*; do $T grep -qx PARTNAME=boot \$b/uevent && echo R \$($T cat \$b/dev) \$($T cat \$b/start) \$($T cat \$b/size) \${b##*/}; done")
set -- $P; [ $# = 4 ] || die "boot partition not found (got: $P)"
DEV=$1; START=$2; SIZE=$3; NAME=$4; say "boot = $NAME dev $DEV start $START size $SIZE"
[ "$START" = 671744 ] && [ "$SIZE" = 131072 ] || die "unexpected boot geometry (expected start 671744, 131072 sectors)"
N=/dev/block/a6ldiag/boot
sh1 "$T mkdir -p /dev/block/a6ldiag; $T rm -f $N; $T mknod $N b ${DEV%%:*} ${DEV##*:} && echo R mknod-ok" | grep -q mknod-ok || die "mknod"
# 2. backup of the current boot
CUR=$(sh1 "$T dd if=$N of=/tmp/boot-before.img bs=1048576 count=64 2>/dev/null; echo R \$($T sha256sum /tmp/boot-before.img)" | cut -c1-64)
[ ${#CUR} = 64 ] || die "could not read the current boot"
say "current boot sha256 $CUR"
mkdir -p logs; B=logs/boot-before-$(date -u +%Y%m%dT%H%M%SZ).img
adb -s "$S" pull /tmp/boot-before.img "$B" >/dev/null 2>&1; [ "$(sha256sum "$B" | cut -c1-64)" = "$CUR" ] || die "backup pull mismatch"
say "backup saved $B"
sh1 "$T rm -f /tmp/boot-before.img" >/dev/null
if [ "$CUR" = "$NEW" ]; then say "boot already holds this image - nothing written"; echo BOOTWRITE_OK_ALREADY $NEW; exit 0; fi
[ -z "$EXP" ] || [ "$CUR" = "$EXP" ] || die "current boot $CUR is not the expected $EXP - nothing written" 4
# 3. push + verify on the phone
adb -s "$S" push "$IMG" /tmp/boot-new.img | tail -n 1
PH=$(sh1 "echo R \$($T sha256sum /tmp/boot-new.img)" | cut -c1-64)
[ "$PH" = "$NEW" ] || { sh1 "$T rm -f /tmp/boot-new.img"; die "sha on the phone $PH != $NEW - nothing written" 5; }
say "pushed, phone sha ok"
# 4. write + read back
sh1 "$T dd if=/tmp/boot-new.img of=$N bs=1048576 count=64 conv=fsync 2>&1 | $T tail -n 1 | $T sed 's/^/R dd: /'; $T sync; echo 3 > /proc/sys/vm/drop_caches; echo R synced" | sed 's/^/BOOTWRITE /'
RB=$(adb -s "$S" shell "$T dd if=$N bs=1048576 count=64 2>/dev/null | $T sha256sum" 2>/dev/null | tr -d '\r' | grep -oE '^[0-9a-f]{64}' | head -n 1)
say "readback sha256 $RB"
sh1 "$T rm -f /tmp/boot-new.img" >/dev/null
[ "$RB" = "$CUR" ] && die "dd wrote nothing: boot unchanged ($CUR). Check the dd line above." 7
[ "$RB" = "$NEW" ] || die "READBACK MISMATCH: boot partition sha $RB != $NEW. DO NOT REBOOT; write the backup back: $0 $B" 6
echo "BOOTWRITE_OK $NEW (previous $CUR, backup $B)"
