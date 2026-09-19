#!/system/bin/sh
# adsp_diag_r2.sh — bounded Hisense A6L ADSP start/stop diagnostic, revision 2.
#
# Rewritten from research/claude-adsp/tools/a6l_adsp_diag.sh to fix the defects recorded in
# docs/external-research-review-20260918.md:
#   1. the start write is asynchronous and watched, so a hung start cannot hang the script;
#   2. firmware and modules are verified against TRUSTED manifests (sha256sum -c), fail closed;
#   3. one EXIT trap performs bounded stop, restores the recovery setting and removes firmware;
#   4. the remoteproc is identified by name AND platform device, never by index;
#   5. every failure returns non-zero and writes RESULT; only kernel messages after our own
#      kmsg marker are judged, so old boot noise cannot pass or fail the run.
# It plays/records nothing and loads no APR/audio/codec module. NOT YET RUN ON THE PHONE.
#
# Inputs pushed by the host (all RAM): $DIR/modules/{order.txt,SHA256SUMS,*.ko},
#   $DIR/firmware/{SHA256SUMS,adsp.mdt,adsp.bNN}, optional $DIR/a6l_qrtr_lookup.
# Test hooks (mock tree only): SYS, PROCDT, FWDIR, KMSG, MOUNTS.
set -u
DIR=${DIR:-/tmp/adsp-diag}
HOLD_S=${HOLD_S:-20}
START_TIMEOUT_S=${START_TIMEOUT_S:-30}
STOP_TIMEOUT_S=${STOP_TIMEOUT_S:-15}
SYS=${SYS:-/sys}
PROCDT=${PROCDT:-/proc/device-tree}
FWDIR=${FWDIR:-/lib/firmware/qcom/hisense/a6l}
KMSG=${KMSG:-/dev/kmsg}
MOUNTS=${MOUNTS:-/proc/mounts}
ADSP_DEV=15700000.remoteproc
EV=$DIR/evidence
RESULT=FAIL-INCOMPLETE
RP=""; OLD_RECOVERY=""; STARTED=0; FW_INSTALLED=0
MARK="A6L_ADSP_DIAG_R2_$$_$(date +%s)"
mkdir -p "$EV" || exit 10
up() { cut -d' ' -f1 /proc/uptime 2>/dev/null || echo '?'; }
log() { echo "[$(up)] $*" | tee -a "$EV/diag.log"; }
since_mark() { dmesg 2>/dev/null | awk -v m="$MARK" 'f{print} index($0,m){f=1}'; }
bad_dmesg() {
    since_mark | grep -E -q 'watchdog received|fatal error received|start timed out|failed to authenticate|Unhandled context fault|Unexpected global fault|remoteproc.*crash|error [0-9-]+ (initializing|setting up) firmware|segment outside memory range|BUG:|Oops|Call trace'
}
state() { cat "$RP/state" 2>/dev/null || echo unreadable; }
snap() {
    since_mark > "$EV/dmesg-$1.txt"
    cat /proc/interrupts > "$EV/interrupts-$1.txt" 2>/dev/null
    ls -l "$SYS/bus/rpmsg/devices/" > "$EV/rpmsg-$1.txt" 2>&1
    for d in "$SYS"/class/remoteproc/remoteproc*; do
        [ -e "$d" ] || continue
        echo "$d name=$(cat $d/name 2>/dev/null) state=$(cat $d/state 2>/dev/null) firmware=$(cat $d/firmware 2>/dev/null) recovery=$(cat $d/recovery 2>/dev/null)"
    done > "$EV/remoteproc-$1.txt" 2>&1
}
# write_bounded <value> <file> <seconds> <tag>: never blocks longer than <seconds>.
write_bounded() {
    ( echo "$1" > "$2" ) 2>"$EV/$4.err" &
    wpid=$!; n=0
    while kill -0 "$wpid" 2>/dev/null && [ "$n" -lt "$3" ]; do sleep 1; n=$((n+1)); done
    if kill -0 "$wpid" 2>/dev/null; then
        log "$4: write still blocked after ${3}s (pid $wpid left behind; kernel owns it)"
        echo blocked > "$EV/$4.rc"; return 124
    fi
    wait "$wpid"; rc=$?; echo "$rc" > "$EV/$4.rc"; return "$rc"
}
cleanup() {
    trap - EXIT INT TERM
    if [ -n "$RP" ] && [ "$STARTED" -eq 1 ] && [ "$(state)" != offline ]; then
        log "cleanup: stopping ADSP (state=$(state))"
        write_bounded stop "$RP/state" "$STOP_TIMEOUT_S" stop
        n=0; while [ "$n" -lt "$STOP_TIMEOUT_S" ] && [ "$(state)" != offline ]; do sleep 1; n=$((n+1)); done
        if [ "$(state)" != offline ]; then log "cleanup: ADSP NOT offline (state=$(state)) - host must power-cycle"; RESULT="$RESULT+STOP-FAILED"; fi
    fi
    [ -n "$RP" ] && [ -n "$OLD_RECOVERY" ] && echo "$OLD_RECOVERY" > "$RP/recovery" 2>/dev/null
    if [ "$FW_INSTALLED" -eq 1 ]; then rm -f "$FWDIR"/adsp.mdt "$FWDIR"/adsp.b[0-9][0-9]; fi
    [ -n "$RP" ] && snap final
    log "=== RESULT: $RESULT"
    echo "$RESULT" > "$EV/RESULT"
    case "$RESULT" in PASS) exit 0;; *) exit 1;; esac
}
die() { RESULT="FAIL-$1"; shift; log "FAIL: $*"; exit 1; }   # EXIT trap turns this into cleanup
trap cleanup EXIT
trap 'RESULT=FAIL-INTERRUPTED; exit 1' INT TERM

echo "$MARK" > "$KMSG" 2>/dev/null || die KMSG "cannot write kmsg marker"
log "=== A6L ADSP diagnostic r2; DIR=$DIR HOLD_S=$HOLD_S START_TIMEOUT_S=$START_TIMEOUT_S"
# ---- phase 0: read-only preflight --------------------------------------------------------
M=$(tr -d '\0' < "$PROCDT/chosen/hisense,a6l-adsp" 2>/dev/null)
S=$(tr -d '\0' < "$PROCDT/soc@0/remoteproc@15700000/status" 2>/dev/null)
log "DT marker=$M adsp_pil status=$S"
[ "$M" = diag1 ] || [ "$M" = diag1-ssccx ] || die IMAGE "DT marker missing: wrong image"
[ "$S" = okay ] || die IMAGE "adsp_pil not enabled in DT"
for d in smem smp2p-adsp firmware:scm 17911000.mailbox 1f40000.hwlock 5100000.iommu; do
    [ -e "$SYS/bus/platform/devices/$d/driver" ] || die PREREQ "prerequisite device not bound: $d"
done
# RAM-only paths: both the staging dir and the firmware dir must live on tmpfs/rootfs/ramfs.
for p in "$DIR" "$(dirname "$FWDIR")"; do
    mkdir -p "$p" || die PATH "cannot create $p"
    fs=$(awk -v p="$p" 'index(p,$2)==1 && length($2)>=l {l=length($2); t=$3} END{print t}' "$MOUNTS")
    case "$fs" in tmpfs|rootfs|ramfs) ;; *) die PATH "$p is on '$fs', not RAM";; esac
done
[ -e "$FWDIR/adsp.mdt" ] && die ORDER "firmware already installed before module load (auto_boot would fire uncontrolled)"
( cd "$DIR/firmware" && sha256sum -c SHA256SUMS ) > "$EV/firmware-verify.txt" 2>&1 || die HASH "firmware does not match trusted manifest"
( cd "$DIR/modules" && sha256sum -c SHA256SUMS ) > "$EV/modules-verify.txt" 2>&1 || die HASH "modules do not match trusted manifest"
[ "$(grep -c ': OK$' "$EV/firmware-verify.txt")" -ge 21 ] || die HASH "firmware manifest lists fewer than 21 files"
snap before
# ---- phase 1: modules --------------------------------------------------------------------
while read -r ko; do
    case "$ko" in ''|\#*) continue;; esac
    grep -q " [*]\{0,1\}$ko\$" "$DIR/modules/SHA256SUMS" || die MODULE "$ko is not in the trusted manifest"
    insmod "$DIR/modules/$ko" 2>>"$EV/insmod.err" || die MODULE "insmod failed: $ko ($(tail -n 1 "$EV/insmod.err"))"
    log "insmod ok: $ko"
done < "$DIR/modules/order.txt"
n=0
while [ "$n" -lt 10 ] && [ -z "$RP" ]; do
    for d in "$SYS"/class/remoteproc/remoteproc*; do
        [ "$(cat "$d/name" 2>/dev/null)" = adsp ] || continue
        case "$(readlink -f "$d/device" 2>/dev/null)" in *"$ADSP_DEV"*) RP=$d;; esac
    done
    [ -n "$RP" ] || { sleep 1; n=$((n+1)); }
done
[ -n "$RP" ] || die IDENTITY "no remoteproc named adsp backed by $ADSP_DEV"
log "remoteproc: $RP state=$(state)"
[ "$(state)" = offline ] || die STATE "unexpected state before firmware install: $(state)"
OLD_RECOVERY=$(cat "$RP/recovery" 2>/dev/null)
snap probed
# ---- phase 2: firmware install + bounded start -------------------------------------------
mkdir -p "$FWDIR" && cp "$DIR"/firmware/adsp.mdt "$DIR"/firmware/adsp.b[0-9][0-9] "$FWDIR"/ || die COPY "firmware copy"
FW_INSTALLED=1
echo disabled > "$RP/recovery" || die RECOVERY "cannot disable recovery"
[ "$(cat "$RP/recovery")" = disabled ] || die RECOVERY "recovery did not read back disabled"
echo qcom/hisense/a6l/adsp.mdt > "$RP/firmware" || die FWNAME "cannot set firmware name"
log "starting ADSP (bounded ${START_TIMEOUT_S}s)"
STARTED=1
write_bounded start "$RP/state" "$START_TIMEOUT_S" start; src=$?
log "start write rc=$src state=$(state)"
snap started
[ "$src" -eq 124 ] && die START-HUNG "start write blocked"
[ "$src" -eq 0 ] || die START "start write failed rc=$src"
[ "$(state)" = running ] || die START "state=$(state) after successful start write"
bad_dmesg && die START-DMESG "stop condition in kernel log after start"
RESULT=FAIL-HOLD
[ -x "$DIR/a6l_qrtr_lookup" ] && "$DIR/a6l_qrtr_lookup" 3000 > "$EV/qrtr-services.txt" 2>&1
n=0
while [ "$n" -lt "$HOLD_S" ]; do
    bad_dmesg && die HOLD-DMESG "stop condition during hold at ${n}s"
    [ "$(state)" = running ] || die HOLD-STATE "state changed during hold: $(state)"
    sleep 1; n=$((n+1))
done
[ -x "$DIR/a6l_qrtr_lookup" ] && "$DIR/a6l_qrtr_lookup" 3000 > "$EV/qrtr-services-end.txt" 2>&1
snap hold
# ---- phase 3: bounded stop (cleanup re-checks) -------------------------------------------
write_bounded stop "$RP/state" "$STOP_TIMEOUT_S" stop || die STOP "stop write rc=$?"
n=0; while [ "$n" -lt "$STOP_TIMEOUT_S" ] && [ "$(state)" != offline ]; do sleep 1; n=$((n+1)); done
[ "$(state)" = offline ] || die STOP "ADSP did not reach offline within ${STOP_TIMEOUT_S}s"
bad_dmesg && die STOP-DMESG "stop condition in kernel log after stop"
RESULT=PASS
exit 0
