#!/vendor/bin/sh
# Debug-only persistent log: preallocated 655872-byte rings per stream.
# Each ring retains four immutable early chunks and sixteen cyclic late chunks.
# Native writer checksums committed chunks; recovery decoder orders sequence IDs.
# No split-created files and no growth during audit floods. Legacy segment logs
# remain readable by the recovery tool. Current + previous must fit metadata.
# Debug coverage lasts until service stop/reboot (after_bc_s=0), not a silent
# one-hour expiry. Storage is bounded independently of elapsed time.
umask 077
D=${A6L_BL_DIR:-/metadata/a6l}; C=$D/cur; P=$D/prev
KMSG=${A6L_BL_KMSG:-/dev/kmsg}; PSTORE=${A6L_BL_PSTORE:-/sys/fs/pstore}; SYSRQ=${A6L_BL_SYSRQ:-/proc/sysrq-trigger}
LOGCAT=${A6L_BL_LOGCAT:-/system/bin/logcat}
RING=${A6L_BL_RING:-/vendor/bin/a6l-log-ring}
RING_KB=644; SNAP_RESERVE_KB=1152
PM_TRACE=${A6L_BL_PM_TRACE:-/vendor/bin/a6l-pm-trace.sh}
HEALTH_PROP=${A6L_BL_HEALTH_PROP:-vendor.a6l.bootlog.health}
pm_trace() {
    [ -x "$PM_TRACE" ] && A6L_PM_CAP_KB=$CAP_KB A6L_PM_FREE_KB=$FREE_KB \
        "$PM_TRACE" "$1" "$C" 2>/dev/null
}
PM_STARTED=0
trap '[ "$PM_STARTED" = 0 ] || pm_trace stop; type streams_stop >/dev/null 2>&1 && streams_stop' EXIT
trap 'exit 0' HUP INT TERM
SEG=${A6L_BL_SEG:-262144}; KH=2; KT=2; LH=2; LT=2
TOMB=${A6L_BL_TOMB:-/data/tombstones}
LCB=${A6L_BL_LCB:-main,system,crash,events,radio}
LCF=${A6L_BL_LCF:-SystemServerTiming:S ProcessCpuTracker:S auditd:S PackageCacher:W aconfigd_mainline:W a6l_dualux:I}
CAP_KB=${A6L_BL_CAP_KB:-3072}; FREE_KB=${A6L_BL_FREE_KB:-1024}; SNAP_S=30
AFTER_BC=${A6L_BL_AFTER_BC:-$(getprop ro.vendor.a6l.bootlog.after_bc_s)}
case "$AFTER_BC" in ''|*[!0-9]*) AFTER_BC=120 ;; esac
[ "$AFTER_BC" -le 43200 ] 2>/dev/null || AFTER_BC=43200
# 0 explicitly means keep this debug service until stopped/rebooted.
mkdir -p $D || exit 1
st() { echo "A6L_BOOTLOG $*" >> $C/bootlog.txt; }
segs() { ls $1/$2.* 2>/dev/null | sort; }              # $1 dir $2 prefix: segments in stream order
keep() {                                              # $1 dir $2 prefix $3 head $4 tail: drop the middle segments
    n=$(segs $1 $2 | wc -l); [ $n -gt $(($3 + $4)) ] || return 0
    dl=$(segs $1 $2 | head -n $(($n - $4)) | tail -n +$(($3 + 1)))
    rm -f $dl
    [ -d $C ] && [ -n "$dl" ] && echo "A6L_BOOTLOG dropped $(echo $dl | wc -w) segment(s) $(basename $(echo $dl | cut -d' ' -f1))..$(basename $(echo $dl | awk '{print $NF}')) of $1" >> $C/bootlog.txt
}
used_kb() { du -sk $D 2>/dev/null | cut -f1; }
free_kb() { [ -n "${A6L_BL_DF_AVAIL_KB:-}" ] && { echo $A6L_BL_DF_AVAIL_KB; return; }; df -k $D 2>/dev/null | tail -n 1 | awk '{print $4}'; }
# Main shell owns all four children for each stream. Private FIFOs let us stop
# a blocked reader when split exits, without a process-name kill or orphan loop.
# Each error head caps its file at 4 KiB. Error flooding closes that FIFO and
# therefore fails the writer instead of filling metadata indefinitely.
child_live() {
    case "$1" in ''|0|*[!0-9]*) return 1 ;; esac
    kill -0 "$1" 2>/dev/null || return 1
    # cat/split/head names have no spaces. Check PPID before signalling a PID
    # which the shell might already have reaped and the kernel reused.
    [ "$(awk '{print $4}' /proc/$1/stat 2>/dev/null)" = "$$" ]
}
child_stop() {
    pid=$1
    if child_live "$pid"; then
        kill -TERM "$pid" 2>/dev/null
        n=0
        while child_live "$pid" && [ "$n" -lt 10 ]; do sleep 0.05; n=$((n + 1)); done
        if child_live "$pid"; then
            kill -KILL "$pid" 2>/dev/null
            n=0
            while child_live "$pid" && [ "$n" -lt 4 ]; do sleep 0.05; n=$((n + 1)); done
        fi
    fi
    # Never block the main logger indefinitely waiting for a stuck kernel task.
    if ! child_live "$pid"; then
        wait "$pid" 2>/dev/null
        return 0
    fi
    st "child $pid did not retire after TERM/KILL; restart forbidden"
    return 1
}
stream_get() { eval "sv=\${${1}_${2}:-0}"; }
stream_set() { eval "${1}_${2}=\$3"; }
stream_stop() {
    stream=$1
    # Names are callers' fixed literals, not property or path input.
    case "$stream" in kmsg|logcat) ;; *) return 1 ;; esac
    stop_failed=0
    for role in reader splitter readerr spliterr; do
        stream_get "$stream" "$role"
        if child_stop "$sv"; then stream_set "$stream" "$role" 0; else stop_failed=1; fi
    done
    if [ "$stop_failed" != 0 ]; then
        stream_set "$stream" exhausted 1
        st "$stream cleanup incomplete; retained child PIDs, no replacement stream"
        return 1
    fi
    rm -f "$C/.$stream.data" "$C/.$stream.readerr.pipe" "$C/.$stream.spliterr.pipe"
    stream_set "$stream" active 0
}
streams_stop() { stream_stop kmsg; stream_stop logcat; }
stream_start() {
    stream=$1
    case "$stream" in kmsg|logcat) ;; *) return 1 ;; esac
    stream_get "$stream" exhausted; [ "$sv" = 0 ] || return 0
    if [ "$stream" = logcat ]; then
        [ "$lc" = on ] && [ "$(getprop logd.ready)" = true ] || return 0
    fi
    stream_get "$stream" retries; retry=$sv
    prefix="$C/$stream.ring"
    if ! mkfifo "$C/.$stream.data" "$C/.$stream.readerr.pipe" "$C/.$stream.spliterr.pipe"; then
        st "$stream reader start failed: private FIFO creation"
        stream_set "$stream" exhausted 1
        stream_stop "$stream"
        return 0
    fi
    head -c 4096 < "$C/.$stream.readerr.pipe" > "$C/.$stream.readerr.txt" &
    stream_set "$stream" readerr "$!"
    head -c 4096 < "$C/.$stream.spliterr.pipe" > "$C/.$stream.spliterr.txt" &
    stream_set "$stream" spliterr "$!"
    if [ "$stream" = kmsg ]; then
        cat "$KMSG" > "$C/.$stream.data" 2> "$C/.$stream.readerr.pipe" &
    else
        $LOGCAT -b $LCB -v threadtime $LCF > "$C/.$stream.data" 2> "$C/.$stream.readerr.pipe" &
    fi
    stream_set "$stream" reader "$!"
    "$RING" "$prefix" < "$C/.$stream.data" 2> "$C/.$stream.spliterr.pipe" &
    stream_set "$stream" splitter "$!"
    stream_set "$stream" active 1
    stream_get "$stream" reader; reader=$sv
    stream_get "$stream" splitter
    st "$stream readers started attempt=$retry reader=$reader splitter=$sv prefix=$(basename "$prefix")"
}
stream_health() {
    stream=$1
    if [ "$stream" = logcat ] && [ "$lc" != on ]; then return 0; fi
    stream_get "$stream" exhausted; [ "$sv" = 0 ] || return 0
    stream_get "$stream" active
    if [ "$sv" = 0 ]; then
        stream_get "$stream" next
        [ "$t" -lt "$sv" ] || stream_start "$stream"
        return 0
    fi
    stream_get "$stream" reader; reader=$sv
    stream_get "$stream" splitter; splitter=$sv
    child_live "$reader" && child_live "$splitter" && return 0
    for role in reader splitter; do
        stream_get "$stream" "$role"
        if ! child_live "$sv"; then
            wait "$sv" 2>/dev/null; rc=$?
            st "$stream $role exited rc=$rc pid=$sv t=${t}s"
        fi
    done
    stream_stop "$stream" || return 0
    for role in readerr spliterr; do
        if [ -s "$C/.$stream.$role.txt" ]; then
            st "$stream $role bounded stderr follows"
            head -c 4096 "$C/.$stream.$role.txt" >> "$C/bootlog.txt"
            echo >> "$C/bootlog.txt"
        fi
    done
    stream_get "$stream" retries; retry=$sv
    if [ "$retry" -ge 3 ]; then
        stream_set "$stream" exhausted 1
        st "$stream reader restarts exhausted; persistent stream unavailable"
        setprop "$HEALTH_PROP" degraded
    else
        retry=$((retry + 1)); delay=$((1 << retry))
        stream_set "$stream" retries "$retry"
        stream_set "$stream" next "$((t + delay))"
        st "$stream reader restart scheduled in ${delay}s attempt=$retry"
    fi
}
streams_health() {
    stream_health kmsg; stream_health logcat
    health=healthy
    stream_get kmsg reader; child_live "$sv" || health=degraded
    stream_get kmsg splitter; child_live "$sv" || health=degraded
    if [ "$(getprop logd.ready)" = true ]; then
        stream_get logcat reader; child_live "$sv" || health=degraded
        stream_get logcat splitter; child_live "$sv" || health=degraded
    else health=starting; fi
    [ "$lc" = on ] || health=degraded
    setprop "$HEALTH_PROP" "$health"
}

# ---- rotate: cur -> prev (trimmed), r6b's single-file log (boot-kmsg.txt[.prev], pstore-prev/) -> prev/kmsg-r6b.txt tail
# A same-boot restart must release our old instance before rotating its ownership.
[ -d "$C" ] && pm_trace stop
rm -rf $P
if [ -d $C ]; then
    mv $C $P; rm -rf $P/pstore; keep $P kmsg 1 1; keep $P logcat 1 1
    # Preserve previous kernel ring; reclaim redundant snapshots/logcat first.
    [ "${A6L_BL_KEEP_PREV_LOGCAT:-0}" = 1 ] || rm -f "$P/logcat.ring"
    rm -f "$P/props.txt" "$P/ps.txt" "$P/camera-topology.txt"
elif [ -f $D/boot-kmsg.txt ]; then
    mkdir -p $P; tail -c $SEG $D/boot-kmsg.txt > $P/kmsg-r6b.txt
fi
rm -rf $D/boot-kmsg.txt $D/boot-kmsg.txt.prev $D/pstore-prev
mkdir -p $C
st "start uptime=$(cut -d' ' -f1 /proc/uptime) build=$(getprop ro.vendor.a6l.rom.build) kernel=$(uname -r) after_bc_awake_s=$AFTER_BC used_kb=$(used_kb) free_kb=$(free_kb)"
# pstore/ramoops (DT a6l-ramoops-v75, Hisense recorder_mem 0xb0180000): console/oops of the PREVIOUS boot, when DDR survived
n=0; for f in $(ls $PSTORE 2>/dev/null); do
    [ $n -lt 4 ] || break; mkdir -p $C/pstore; tail -c 131072 $PSTORE/$f > $C/pstore/$f 2>/dev/null; n=$((n + 1))
done
st "pstore-prev: $(ls $C/pstore 2>/dev/null | tr '\n' ' ')"
# Debug-only, owned tracefs instance: callbacks and USB reconnect return code.
# Snapshot stays capped at 128 KiB and is included in the existing metadata budget.
pm_trace start
PM_STARTED=1
# Reserve both rings before readers start. A shell pruning interval cannot bound
# a producer writing arbitrarily fast. Keep at least FREE_KB for Android plus a
# worst-case snapshot allowance. No old evidence is discarded without a receipt.
f=$(free_kb)
if [ -n "$f" ] && [ "$f" -lt $((FREE_KB + 2 * RING_KB + SNAP_RESERVE_KB)) ] && [ -d "$P" ]; then
    rm -rf "$P"; st "reservation: previous boot removed to retain Android free-space reserve"
fi
f=$(free_kb)
if [ ! -x "$RING" ] || [ -z "$f" ] || [ "$f" -lt $((FREE_KB + 2 * RING_KB + SNAP_RESERVE_KB)) ]; then
    st "reservation failed: writer unavailable or insufficient free space ($f KiB)"
    setprop "$HEALTH_PROP" degraded
    setprop vendor.a6l.bootlog running
    exit 1
fi
if ! "$RING" "$C/kmsg.ring" < /dev/null || ! "$RING" "$C/logcat.ring" < /dev/null; then
    st "reservation failed: native fixed-ring allocation"
    setprop "$HEALTH_PROP" degraded
    setprop vendor.a6l.bootlog running
    exit 1
fi
setprop "$HEALTH_PROP" starting
# ---- supervised streams: no restart while reader and ring writer are healthy.
lc=on; t=0
stream_start kmsg
stream_start logcat
snap() {
    pm_trace snapshot
    [ "$lc" = on ] || return 0
    # Read-only graph topology, including sensor-to-lens ancillary links. No
    # stream or focus commands; bounded helper and 64 KiB retained output.
    if [ -c /dev/media0 ] && [ -x /vendor/bin/a6l-media-ancillary ]; then
        /vendor/bin/a6l-media-ancillary 2>&1 | head -c 65536 > $C/camera-topology.tmp
        mv -f $C/camera-topology.tmp $C/camera-topology.txt
    fi
    getprop 2>/dev/null | head -c 65536 > $C/props.tmp; mv -f $C/props.tmp $C/props.txt
    { echo "# uptime $(cut -d' ' -f1 /proc/uptime)"; ps -A -o PID,PPID,USER,S,RSS,WCHAN,TIME,CMDLINE 2>/dev/null; } | head -c 65536 > $C/ps.tmp; mv -f $C/ps.tmp $C/ps.txt
    tombs
}
tombs() {           # r6d: /data/tombstones (after /data is mounted): listing + the 3 identifying lines of each text tombstone
    [ -d $TOMB ] || return 0
    { echo "# uptime $(cut -d' ' -f1 /proc/uptime)"; ls -la $TOMB 2>/dev/null
      for f in $(ls $TOMB 2>/dev/null | grep -v '[.]pb$' | head -n 32); do
          echo "== $f"; grep -m 4 -E '^(Cmdline|Abort message|signal|pid): ' $TOMB/$f 2>/dev/null
      done; } 2>/dev/null | head -c 32768 > $C/tomb.tmp
    head -c 32768 $C/tomb.tmp > $C/tombstones.txt; rm -f $C/tomb.tmp
}
lc=on; snap; sync
# the display group (debug builds) waits for this before its first insmod (a6l-modules.sh display)
setprop vendor.a6l.bootlog running
blocked() {
    { echo "A6L_BOOTLOG $1: display group still running (vendor.a6l.display=$(getprop vendor.a6l.display)); D-state tasks:"
      ps -A -o PID,S,WCHAN,CMD 2>/dev/null | awk '$2=="D"'; } >> $C/bootlog.txt
    for p in $(ps -A -o PID,S 2>/dev/null | awk '$2=="D"{print $1}'); do
        { echo "A6L_BOOTLOG stack $p $(cat /proc/$p/cmdline 2>/dev/null | tr '\0' ' ')"; cat /proc/$p/stack 2>/dev/null; } >> $C/bootlog.txt
    done
    echo w > $SYSRQ
}
budget() {                                            # hard bounds of /metadata/a6l (see BUDGET)
    keep $C kmsg $KH $KT; keep $C logcat $LH $LT
    nb=$((nb + 1)); [ $nb -ge 3 ] || [ "$1" = now ] || return 0; nb=0        # du/df every 3rd call (~6 s): cheap in QEMU too
    [ "$(stat -c %s $C/bootlog.txt 2>/dev/null || echo 0)" -gt 131072 ] && {
        { head -n 3 $C/bootlog.txt; echo "A6L_BOOTLOG (bootlog.txt cut)"; tail -c 65536 $C/bootlog.txt; } > $C/bl.tmp; mv -f $C/bl.tmp $C/bootlog.txt; }
    u=$(used_kb); f=$(free_kb); [ -n "$u" ] && [ -n "$f" ] || return 0
    if [ $u -gt $CAP_KB ] || [ $f -lt $FREE_KB ]; then
        if [ -d $P ]; then rm -rf $P; st "budget: used ${u} KiB free ${f} KiB -> previous boot deleted"; return 0; fi
        if [ "$lc" = on ]; then
            lc=off; stream_stop logcat; rm -f $C/props.txt $C/ps.txt; keep $C logcat $LH 1
            st "budget: used ${u} KiB free ${f} KiB -> logcat stream and snapshots stopped"
        fi
    fi
}
# sync every 0.5 s while the display group loads (a hard hang inside an insmod loses at most ~0.5 s of log), then every 2 s
t=0; h=0; bc=-1; s30=0; s90=0; ns=$SNAP_S; nb=0; budget now
while :; do
    if [ "$(getprop vendor.a6l.display)" = loading ] && [ $t -lt 120 ]; then sleep 0.5; h=$((h+1)); [ $h -ge 4 ] && { h=0; t=$((t+2)); }
    else sleep 2; t=$((t+2)); h=0; fi
    sync
    [ $h = 0 ] || continue                            # the rest every 2 s (the 0.5 s cadence is only for the sync)
    streams_health
    budget
    [ $t -ge $ns ] && { ns=$((t + SNAP_S)); snap; }
    if [ "$(getprop init.svc.a6l_modules_display)" = running ]; then
        [ $t -ge 30 ] && [ $s30 = 0 ] && { s30=1; blocked 30s; }
        [ $t -ge 90 ] && [ $s90 = 0 ] && { s90=1; blocked 90s; }
    fi
    if [ "$(getprop sys.boot_completed)" = 1 ]; then
        [ $bc -lt 0 ] && { bc=$t; st "sys.boot_completed seen at ${t}s"; }
        [ "$AFTER_BC" -gt 0 ] && [ $((t - bc)) -ge $AFTER_BC ] && break
    fi
done
snap; pm_trace stop; PM_STARTED=0; sleep 1; st "stop t=${t}s uptime=$(cut -d' ' -f1 /proc/uptime) used_kb=$(used_kb) free_kb=$(free_kb)"; sync
# init kills the readers (process group) when this oneshot service exits; outside init (tests) stop them here
streams_stop; setprop "$HEALTH_PROP" stopped; sleep 1; keep $C kmsg $KH $KT; keep $C logcat $LH $LT; sync
exit 0
