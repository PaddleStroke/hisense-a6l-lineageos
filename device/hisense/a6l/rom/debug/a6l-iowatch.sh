#!/vendor/bin/sh
# A6L debug IO-stall detector (r6e, 1 Oct 2026; docs/rom-r6e-20261001.md). userdebug/eng ONLY (rom/debug/bootlog.mk), started
# by init.a6l.bootlog-debug.rc at post-fs-data (/metadata and /data mounted), runs until shutdown. Domain su (permissive build).
#
# Why: r6d stalled 1-2 min after boot (adb enumerated but `adb shell` dead, touch frozen); the persistent boot log on /metadata
# stopped being written back long before the visible freeze. Every PERIOD s this writes a few bytes + fsync + rename on
# /metadata and on /data, each in a background child. A child that has not finished after LIMIT s (counted in awake seconds:
# `sleep 1` ticks, so s2idle suspend time is not counted) = IO stall: it reports to the KERNEL LOG only (never to storage):
#   A6L_IOSTALL target=... age=...       + uptime, /proc/pressure/io, eMMC inflight, /proc/diskstats of the eMMC,
#                                          mmc debugfs ios, D-state tasks (pid comm wchan), the stuck child's kernel stack,
#                                          then sysrq 'w' (blocked tasks) and 'l' (all-CPU backtraces)
#   A6L_IOSTALL recovered target=... after Ns
# repeated at most every REARM s while the stall lasts. The kernel log stays readable live over USB (`adb shell dmesg -w`,
# dmesg_restrict=0 + /dev/kmsg 0644 in debug builds) even when storage is stuck. Every 60 s: one A6L_IOWATCH alive line.
# Only shell builtins + reads of /proc and /sys in the report path (no new exec that could need a page from storage),
# except the two sysrq writes (echo builtin) and the child processes (toybox fsync/mv, already in the page cache).
# Test hook (QEMU / bench): ro.boot.a6l_iowatch_test=fifo (or A6L_IW_TEST=fifo) makes the first /metadata heartbeat block
# on a FIFO for ~45 s, i.e. a simulated stall that must produce A6L_IOSTALL and then "recovered".
KMSG=${A6L_IW_KMSG:-/dev/kmsg}; SYSRQ=${A6L_IW_SYSRQ:-/proc/sysrq-trigger}
T1=${A6L_IW_META:-/metadata/a6l/heartbeat}; T2=${A6L_IW_DATA:-/data/local/tmp/a6l-heartbeat}
PERIOD=${A6L_IW_PERIOD:-5}; LIMIT=${A6L_IW_LIMIT:-15}; REARM=${A6L_IW_REARM:-60}; ALIVE=${A6L_IW_ALIVE:-60}
MAXTICKS=${A6L_IW_MAXTICKS:-0}                      # tests only: stop after N ticks (0 = run forever)
PROC=${A6L_IW_PROC:-/proc}; SYS=${A6L_IW_SYS:-/sys}
TEST=${A6L_IW_TEST:-$(getprop ro.boot.a6l_iowatch_test 2>/dev/null)}
km() { echo "A6L_IOSTALL $*" >> $KMSG; }
kl() { echo "A6L_IOWATCH $*" >> $KMSG; }
kf() {                                              # $1 tag $2 file: each line of a /proc or /sys file to kmsg (builtin read)
    [ -r "$2" ] || return 0
    while IFS= read -r l; do echo "A6L_IOSTALL $1 $l" >> $KMSG; done 2>/dev/null < "$2"
}
# the eMMC (sdhc_1, c0c4000.mmc): mmcblkN + its host mmcN
EMMC=; for b in $SYS/block/mmcblk[0-9]; do
    [ -e "$b" ] || continue
    case "$(readlink -f $b 2>/dev/null)" in *c0c4000*) EMMC=${b##*/};; esac
done
[ -n "$EMMC" ] || EMMC=mmcblk0
HOST=mmc${EMMC#mmcblk}
DBG=$SYS/kernel/debug
[ -d $DBG/$HOST ] || mount -t debugfs debugfs $DBG 2>/dev/null
mkdir -p ${T1%/*} ${T2%/*} 2>/dev/null
if [ "$TEST" = fifo ]; then
    rm -f $T1.tmp; mkfifo $T1.tmp && kl "test: $T1.tmp is a FIFO -> simulated stall, released in ~45 s"
    ( sleep ${A6L_IW_TEST_RELEASE:-45}; cat $T1.tmp > /dev/null; rm -f $T1.tmp ) &
fi
# start-up facts (live-stream and QEMU checks): mount options of /metadata and /data, kernel log readability
while IFS= read -r l; do
    set -- $l
    case "$2" in /metadata|/data) kl "mount $2 $3 $4";; esac
done < $PROC/mounts
kl "start emmc=$EMMC host=$HOST period=${PERIOD}s limit=${LIMIT}s dmesg_restrict=$(cat $PROC/sys/kernel/dmesg_restrict 2>/dev/null) kmsg=$(ls -l /dev/kmsg 2>/dev/null | cut -d' ' -f1) debugfs=$([ -d $DBG/$HOST ] && echo yes || echo no)"
hb() {                                              # $1 target: write + fsync + rename, in the background
    ( echo "$(cat $PROC/uptime)" > $1.tmp && fsync $1.tmp && mv -f $1.tmp $1 ) &
}
alive() {                                           # $1 pid: 0 while running (not a zombie, not gone)
    [ -n "$1" ] || return 1
    read -r _p _c s _r 2>/dev/null < $PROC/$1/stat || return 1
    case "$_c" in *\)) ;; *) set -- $_r; s=$1;; esac   # comm with a space: skip to the field after ')'
    [ "$s" != Z ]
}
report() {                                          # $1 target $2 pid $3 age
    tg=$1; pd=$2
    km "target=$tg pid=$pd age=${3}s (write+fsync not done within ${LIMIT}s) uptime=$(read -r u _ < $PROC/uptime; echo $u)"
    kf psi-io $PROC/pressure/io
    kf inflight $SYS/block/$EMMC/inflight
    while IFS= read -r l; do set -- $l; case "$3" in $EMMC|${EMMC}p*) [ "${12:-0}" != 0 ] || [ "$3" = $EMMC ] && km "diskstats $l";; esac; done < $PROC/diskstats
    kf ios $DBG/$HOST/ios
    kf err_stats $DBG/$HOST/err_stats
    n=0; for d in $PROC/[0-9]*; do
        read -r p c s _r 2>/dev/null < $d/stat || continue
        case "$c" in *\)) ;; *) set -- $_r; s=$1;; esac
        [ "$s" = D ] || continue
        n=$((n + 1)); [ $n -le 40 ] || continue
        w=; read -r w 2>/dev/null < $d/wchan; km "D pid=$p comm=$c wchan=$w"
    done
    km "D tasks: $n"
    ch=; read -r ch 2>/dev/null < $PROC/$pd/task/$pd/children
    for c in $ch $pd; do kf "stack $c" $PROC/$c/stack; done
    echo w >> $SYSRQ 2>/dev/null; echo l >> $SYSRQ 2>/dev/null
    km "end of report target=$tg (sysrq w+l above)"
}
tick=0; p1=; p2=; s1=0; s2=0; r1=-999; r2=-999; n1=0; n2=0; max1=0; max2=0; nexta=$ALIVE
while :; do
    # target 1 (/metadata)
    if [ -n "$p1" ]; then
        a=$((tick - s1))
        if alive $p1; then
            [ $a -ge $LIMIT ] && [ $((tick - r1)) -ge $REARM ] && { r1=$tick; report $T1 $p1 $a; }
        else
            wait $p1 2>/dev/null; [ $r1 -ge 0 ] && km "recovered target=$T1 after ${a}s"
            [ $a -gt $max1 ] && max1=$a; p1=; r1=-999; n1=$((n1 + 1))
        fi
    elif [ $((tick % PERIOD)) = 0 ]; then hb $T1; p1=$!; s1=$tick; fi
    # target 2 (/data)
    if [ -n "$p2" ]; then
        a=$((tick - s2))
        if alive $p2; then
            [ $a -ge $LIMIT ] && [ $((tick - r2)) -ge $REARM ] && { r2=$tick; report $T2 $p2 $a; }
        else
            wait $p2 2>/dev/null; [ $r2 -ge 0 ] && km "recovered target=$T2 after ${a}s"
            [ $a -gt $max2 ] && max2=$a; p2=; r2=-999; n2=$((n2 + 1))
        fi
    elif [ $((tick % PERIOD)) = 0 ]; then hb $T2; p2=$!; s2=$tick; fi
    [ $tick -ge $nexta ] && { nexta=$((tick + ALIVE)); kl "alive t=${tick}s heartbeats meta=$n1 data=$n2 max_s meta=$max1 data=$max2"; }
    sleep 1; tick=$((tick + 1))
    [ $MAXTICKS -gt 0 ] && [ $tick -ge $MAXTICKS ] && break
done
kl "stop t=${tick}s heartbeats meta=$n1 data=$n2"
exit 0
