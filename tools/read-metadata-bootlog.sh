#!/bin/bash
# r6c (30 Sep 2026, docs/rom-r6c-20260930.md) LAPTOP, ATTENDED ONLY: read the debug boot log of the last LineageOS boot(s)
# from the phone's /metadata partition while the phone is in the V75 diagnostic recovery (adb serial HLTE730T-PROBE,
# state "recovery"). READ-ONLY on the phone: loads the eMMC driver if needed, creates one block node in /dev (RAM) and
# streams the whole 10 MiB metadata partition over `adb exec-out` (a toybox printf marker separates the recovery shell's
# linker warnings from the data; cat's stderr is discarded). Nothing is mounted or written on the phone.
# On the laptop: the copy is checked (10485760 bytes), kept as meta-orig.img, e2fsck -fy runs on a SECOND copy (journal
# replay of a boot that died), then debugfs dumps /a6l and the streams are concatenated.
# usage: read-metadata-bootlog.sh [outdir]   (default logs/meta-<UTC>) -> <outdir>/{kmsg,logcat}-{cur,prev}.txt, props/ps, bootlog.txt
set -u
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
DECODER=${A6L_RING_DECODER:-$SCRIPT_DIR/decode-a6l-log-ring.py}
S=${ADB_SERIAL:-HLTE730T-PROBE}; T=/system/bin/toybox; SIZE=10485760; MARK=A6L_META_BEGIN_7d41
O=${1:-logs/meta-$(date -u +%Y%m%dT%H%M%SZ)}; mkdir -p "$O" || exit 1
die() { echo "META_FAIL $*"; exit 1; }
adb devices | grep -qE "^$S[[:space:]]+recovery" || die "phone $S not in recovery state (adb devices)"
d=$(adb -s "$S" shell "$T grep -q '^sdhci_msm ' /proc/modules || { $T insmod /sdhci-msm.ko; $T sleep 6; }; for b in /sys/class/block/mmcblk*p*; do $T grep -qx PARTNAME=metadata \$b/uevent && echo R \$($T cat \$b/dev) \$($T cat \$b/size); done" 2>/dev/null | tr -d '\r' | grep '^R ')
set -- $d; [ $# = 3 ] || die "metadata partition not found ($d)"
[ "$3" = $((SIZE / 512)) ] || die "metadata size $3 sectors (expected $((SIZE / 512)))"
echo "metadata dev $2"
adb -s "$S" shell "$T mkdir -p /dev/block/a6lmeta; $T rm -f /dev/block/a6lmeta/md; $T mknod /dev/block/a6lmeta/md b ${2%%:*} ${2##*:}" 2>/dev/null
adb -s "$S" exec-out "$T printf $MARK; $T cat /dev/block/a6lmeta/md 2>/dev/null" > "$O/raw.bin" || die "exec-out failed"
python3 - "$O/raw.bin" "$O/meta-orig.img" $MARK $SIZE <<'PY' || exit 1
import sys
raw, out, mark, size = open(sys.argv[1], 'rb').read(), sys.argv[2], sys.argv[3].encode(), int(sys.argv[4])
i = raw.find(mark)
if i < 0 or i > 65536: sys.exit('META_FAIL marker not found')
data = raw[i + len(mark):]
if len(data) != size: sys.exit('META_FAIL got %d bytes after the marker, expected %d' % (len(data), size))
open(out, 'wb').write(data); print('metadata copy ok: %d bytes (%d bytes before the marker dropped)' % (len(data), i))
PY
sha256sum "$O/meta-orig.img"; cp "$O/meta-orig.img" "$O/meta.img"
E2FSCK=$(command -v e2fsck || echo /sbin/e2fsck); DEBUGFS=$(command -v debugfs || echo /sbin/debugfs)
$E2FSCK -fy "$O/meta.img" > "$O/e2fsck.log" 2>&1; echo "e2fsck rc=$? (0/1 = clean/fixed)"
rm -rf "$O/dump"; mkdir -p "$O/dump"; $DEBUGFS -R "rdump /a6l $O/dump" "$O/meta.img" > "$O/debugfs.log" 2>&1
$DEBUGFS -R stats "$O/meta.img" 2>/dev/null | grep -E '^(Block count|Free blocks):' > "$O/fs-stats.txt"
A=$O/dump/a6l; [ -d "$A" ] || die "no /a6l on metadata (the boot never reached mount_all, or the log never started)"
for b in cur prev; do
  [ -d "$A/$b" ] || continue
  ls $A/$b/kmsg.* > /dev/null 2>&1 && cat $(ls $A/$b/kmsg.* | sort) > "$O/kmsg-$b.txt"
  ls $A/$b/logcat.* > /dev/null 2>&1 && cat $(ls $A/$b/logcat.* | sort) > "$O/logcat-$b.txt"
  for stream in kmsg logcat; do
    if [ -f "$A/$b/$stream.ring" ]; then
      [ -f "$DECODER" ] || die "ring decoder unavailable: $DECODER"
      python3 "$DECODER" "$A/$b/$stream.ring" "$O/$stream-$b.txt" 2> "$O/$stream-$b-ring-health.txt" || die "invalid $stream $b ring"
    fi
  done
  for f in props.txt ps.txt bootlog.txt tombstones.txt camera-topology.txt pmtrace.txt pmtrace-info.txt pmtrace-status.txt; do
    [ -f "$A/$b/$f" ] && cp "$A/$b/$f" "$O/${f%.txt}-$b.txt"
  done
done
[ -f "$A/prev/kmsg-r6b.txt" ] && cp "$A/prev/kmsg-r6b.txt" "$O/kmsg-prev-r6b.txt"
( cd "$A" && find . -type f -printf '%p %s\n' | sort ) > "$O/files.txt"
echo "== $O"; cat "$O/fs-stats.txt"; ls -la "$O" | grep -E 'txt$'
echo "gaps: segments dropped by the size budget are listed in bootlog-*.txt ('dropped N segment(s) ...')"
echo META_READ_OK
