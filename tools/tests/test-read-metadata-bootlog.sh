#!/usr/bin/env bash
# r6c (30 Sep 2026): host test of tools/read-metadata-bootlog.sh with a fake adb (V75 recovery: linker warnings on the
# stream before the marker, /sys/class/block with a metadata partition) and a 10 MiB ext4 image in the r6c layout, whose
# journal holds uncommitted-looking state (the image is left "not clean"). usage: bash tools/tests/test-read-metadata-bootlog.sh
set -u
R=$(cd "$(dirname "$0")/../.." && pwd); W=$(mktemp -d); fails=0
exp() { [ "$2" = "$3" ] && echo "ok   $1 ($3)" || { echo "FAIL $1: got '$2' want '$3'"; fails=$((fails+1)); }; }
PATH=/sbin:/usr/sbin:$PATH
mkdir -p $W/src/a6l/cur $W/src/a6l/prev $W/bin $W/sys/class/block/mmcblk1p40
printf 'kmsg head\n' > $W/src/a6l/cur/kmsg.aaa; printf 'kmsg tail\n' > $W/src/a6l/cur/kmsg.aad
printf 'logcat head\n' > $W/src/a6l/cur/logcat.aaa; printf 'logcat tail\n' > $W/src/a6l/cur/logcat.aac
printf 'A6L_BOOTLOG start\nA6L_BOOTLOG dropped 2 segment(s) kmsg.aab..kmsg.aac of /metadata/a6l/cur\n' > $W/src/a6l/cur/bootlog.txt
printf '[ro.vendor.a6l.rom.build]: [r6c]\n' > $W/src/a6l/cur/props.txt; printf 'PID\n' > $W/src/a6l/cur/ps.txt
printf 'prev kmsg\n' > $W/src/a6l/prev/kmsg.aaa; printf 'r6b\n' > $W/src/a6l/prev/kmsg-r6b.txt
printf 'USB reconnect ret=-110\n' > $W/src/a6l/cur/pmtrace.txt
printf 'clock=boot\n' > $W/src/a6l/cur/pmtrace-info.txt
printf 'started events=4\n' > $W/src/a6l/cur/pmtrace-status.txt
printf 'old PM callback\n' > $W/src/a6l/prev/pmtrace.txt
printf 'Cmdline: surfaceflinger\n' > $W/src/a6l/cur/tombstones.txt
cc -std=c11 -O2 -Wall -Wextra -Werror "$R/device/hisense/a6l/rom/debug/a6l-log-ring.c" -o "$W/ring" || exit 1
printf 'fixed ring previous android log\n' | "$W/ring" "$W/src/a6l/prev/logcat.ring" || exit 1
mke2fs -q -t ext4 -b 4096 -d $W/src $W/part.img 10M || { echo "mke2fs missing"; exit 1; }
printf 'DEVTYPE=partition\nPARTNAME=metadata\n' > $W/sys/class/block/mmcblk1p40/uevent
echo 179:40 > $W/sys/class/block/mmcblk1p40/dev; echo 20480 > $W/sys/class/block/mmcblk1p40/size
cat > $W/bin/adb <<'A'
#!/usr/bin/env bash
W=__W__
if [ "$1" = devices ]; then printf 'List of devices attached\nHLTE730T-PROBE\trecovery\n'; exit 0; fi
shift 2; kind=$1; cmd=$2; echo "$kind $cmd" >> $W/adb.log
cmd=${cmd//\/system\/bin\/toybox /}; cmd=${cmd//\/sys\/class\/block/$W/sys/class/block}; cmd=${cmd//\/proc\/modules/$W/modules}
cmd=${cmd//\/dev\/block\/a6lmeta/$W/devblk}
mknod() { ln -sf $W/part.img "$1"; }; insmod() { echo "sdhci_msm 1 0" >> $W/modules; }
export -f mknod insmod 2>/dev/null
if [ "$kind" = exec-out ]; then
  printf 'WARNING: linker: Warning: failed to find generated linker configuration from "/linkerconfig/ld.config.txt"\n'
fi
W=$W bash -c "mknod() { ln -sf $W/part.img \"\$1\"; }; insmod() { echo 'sdhci_msm 1 0' >> $W/modules; }; $cmd"
A
sed -i "s#__W__#$W#" $W/bin/adb; chmod +x $W/bin/adb; : > $W/modules
( cd $W && PATH=$W/bin:$PATH bash $R/tools/read-metadata-bootlog.sh $W/out ) > $W/run.log 2>&1
exp "read-out completes" "$(tail -1 $W/run.log)" META_READ_OK
exp "exact 10 MiB copy, marker + linker warning stripped" "$(cmp -s $W/out/meta-orig.img $W/part.img && echo same)" same
exp "eMMC driver loaded when missing" "$(grep -c sdhci_msm $W/modules)" 1
exp "cur kmsg concatenated in order" "$(cat $W/out/kmsg-cur.txt | tr '\n' '|')" "kmsg head|kmsg tail|"
exp "cur logcat concatenated" "$(cat $W/out/logcat-cur.txt | tr '\n' '|')" "logcat head|logcat tail|"
exp "prev + r6b legacy + props/bootlog" "$(cat $W/out/kmsg-prev.txt)/$(cat $W/out/kmsg-prev-r6b.txt)/$(grep -c r6c $W/out/props-cur.txt)/$(grep -c dropped $W/out/bootlog-cur.txt)" "prev kmsg/r6b/1/1"
exp "bounded PM diagnostics survive recovery extraction" "$(cat $W/out/pmtrace-cur.txt)/$(cat $W/out/pmtrace-info-cur.txt)/$(cat $W/out/pmtrace-status-cur.txt)/$(cat $W/out/pmtrace-prev.txt)" "USB reconnect ret=-110/clock=boot/started events=4/old PM callback"
exp "tombstone summary survives recovery extraction" "$(cat $W/out/tombstones-cur.txt)" "Cmdline: surfaceflinger"
exp "native fixed-ring log recovered through ext4 extraction" "$(cat $W/out/logcat-prev.txt)" "fixed ring previous android log"
exp "ring decode reports valid retained slot" "$(grep -c "'slots': 1" $W/out/logcat-prev-ring-health.txt)" 1
exp "phone side read-only (no mount, no dd/write to the node)" "$(grep -cE '(^| )mount |dd |of=|[^2]> */dev/block' $W/adb.log)" 0
# wrong size -> refused
echo 20000 > $W/sys/class/block/mmcblk1p40/size
( cd $W && PATH=$W/bin:$PATH bash $R/tools/read-metadata-bootlog.sh $W/out2 ) > $W/run2.log 2>&1
exp "unexpected partition size refused" "$(grep -c 'META_FAIL metadata size' $W/run2.log)" 1
rm -rf $W
echo "A6L_READ_METADATA_TEST $([ $fails = 0 ] && echo PASS || echo FAIL $fails)"; exit $fails
