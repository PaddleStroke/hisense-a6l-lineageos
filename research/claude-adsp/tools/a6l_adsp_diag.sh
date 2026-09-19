#!/system/bin/sh
# a6l_adsp_diag.sh — bounded Hisense A6L ADSP startup diagnostic (PROPOSAL).
#
# Runs on the phone inside the existing diagnostic RAM userspace, over the
# authenticated ADB session, on an image whose DT carries
# patches/a6l-adsp-diag.dtso (marker /chosen/hisense,a6l-adsp = "diag1").
# It starts the ADSP once through the pinned qcom_q6v5_pas driver, records
# authentication/startup and service-discovery evidence, and stops it cleanly.
#
# It does NOT: play or record audio, load any q6afe/q6asm/APR/codec/sound
# module, touch the modem/CDSP/Wi-Fi, write to eMMC, or unload modules.
# All evidence goes to a tmpfs directory that the host pulls afterwards.
#
# Inputs (pushed by the host before running):
#   $DIR/modules/<order>.ko  (see tools/adsp-diag-modules.txt; exact prep-bundle hashes)
#   $DIR/firmware/adsp.mdt adsp.b02..adsp.b21  (peripheral-prep-20260917 hashes)
#   $DIR/a6l_qrtr_lookup (optional static helper from tools/a6l_qrtr_lookup.c)
# Environment: DIR (default /tmp/adsp-diag), HOLD_S (default 20), START_TIMEOUT_S (30)
set -u
DIR=${DIR:-/tmp/adsp-diag}
HOLD_S=${HOLD_S:-20}
START_TIMEOUT_S=${START_TIMEOUT_S:-30}
FWDIR=/lib/firmware/qcom/hisense/a6l
RP=""            # /sys/class/remoteproc/remoteprocN for the "adsp" instance
EV=$DIR/evidence
RESULT=UNKNOWN
mkdir -p "$EV" || exit 1
log() { echo "[$(cat /proc/uptime | cut -d' ' -f1)] $*" | tee -a "$EV/diag.log"; }
snap() { # snap <tag>: dmesg + interrupts + genpd + rpmsg/remoteproc state
	dmesg > "$EV/dmesg-$1.txt" 2>/dev/null
	cat /proc/interrupts > "$EV/interrupts-$1.txt" 2>/dev/null
	[ -r /sys/kernel/debug/pm_genpd/pm_genpd_summary ] && cat /sys/kernel/debug/pm_genpd/pm_genpd_summary > "$EV/genpd-$1.txt"
	ls -l /sys/bus/rpmsg/devices/ > "$EV/rpmsg-$1.txt" 2>&1
	for d in /sys/class/remoteproc/remoteproc*; do
		[ -e "$d" ] || continue
		echo "$d name=$(cat $d/name 2>/dev/null) state=$(cat $d/state 2>/dev/null) firmware=$(cat $d/firmware 2>/dev/null) recovery=$(cat $d/recovery 2>/dev/null)"
	done > "$EV/remoteproc-$1.txt" 2>&1
	cat /proc/meminfo > "$EV/meminfo-$1.txt" 2>/dev/null
}
fail() { RESULT=FAIL; log "FAIL: $*"; }
bad_dmesg() { # returns 0 if a stop condition appeared in dmesg since boot
	dmesg 2>/dev/null | grep -E -q 'watchdog received|fatal error received|start timed out|failed to authenticate|Unhandled context fault|Unexpected global fault|remoteproc.*crash|error [0-9-]+ (initializing|setting up) firmware|segment outside memory range|BUG:|Oops|Call trace'
}

log "=== A6L ADSP diagnostic start; DIR=$DIR HOLD_S=$HOLD_S"
# ---- phase 0: preflight (read-only) -----------------------------------------
M=$(cat /proc/device-tree/chosen/hisense,a6l-adsp 2>/dev/null | tr -d '\0')
S=$(cat /proc/device-tree/soc@0/remoteproc@15700000/status 2>/dev/null | tr -d '\0')
F=$(cat /proc/device-tree/soc@0/remoteproc@15700000/firmware-name 2>/dev/null | tr -d '\0')
log "DT marker=$M adsp_pil status=$S firmware-name=$F"
[ "$M" = "diag1" ] || [ "$M" = "diag1-ssccx" ] || { fail "DT marker missing: wrong image"; exit 2; }
[ "$S" = "okay" ] || { fail "adsp_pil not enabled in DT"; exit 2; }
for d in smem smp2p-adsp firmware:scm 17911000.mailbox 1f40000.hwlock 5100000.iommu; do
	if [ -e "/sys/bus/platform/devices/$d/driver" ]; then log "bound: $d -> $(basename $(readlink /sys/bus/platform/devices/$d/driver))"; else fail "prerequisite device not bound: $d"; fi
done
[ "$RESULT" = FAIL ] && exit 2
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
cd "$DIR/firmware" && sha256sum adsp.mdt adsp.b* > "$EV/firmware-sha256.txt" 2>&1 && cd /
[ -e "$FWDIR/adsp.mdt" ] && { fail "firmware already installed before module load (auto_boot would fire uncontrolled)"; exit 2; }
snap before
# ---- phase 1: modules (order file, each line: file) ------------------------
while read -r ko; do
	[ -z "$ko" ] && continue
	case "$ko" in \#*) continue;; esac
	[ -f "$DIR/modules/$ko" ] || { fail "missing module $ko"; break; }
	insmod "$DIR/modules/$ko" 2>>"$EV/insmod.err" && log "insmod ok: $ko" || { fail "insmod failed: $ko ($(tail -1 $EV/insmod.err))"; break; }
done < "$DIR/modules/order.txt"
[ "$RESULT" = FAIL ] && { snap modfail; exit 3; }
sleep 3
for d in /sys/class/remoteproc/remoteproc*; do [ "$(cat $d/name 2>/dev/null)" = "adsp" ] && RP=$d; done
[ -n "$RP" ] || { fail "no remoteproc named adsp after qcom_q6v5_pas load"; snap noproc; exit 3; }
log "remoteproc: $RP state=$(cat $RP/state) firmware=$(cat $RP/firmware)"
[ "$(cat $RP/state)" = "offline" ] || { fail "unexpected state before firmware install: $(cat $RP/state)"; snap unexpected; exit 3; }
dmesg | grep -E 'remoteproc|qcom_q6v5_pas|request_firmware|15700000' > "$EV/dmesg-after-probe-filtered.txt"
snap probed
# ---- phase 2: install firmware into tmpfs, controlled start ---------------
mkdir -p "$FWDIR" && cp "$DIR"/firmware/adsp.mdt "$DIR"/firmware/adsp.b* "$FWDIR"/ || { fail "firmware copy"; exit 3; }
echo disabled > "$RP/recovery"   # observe a crash once; do not auto-restart
echo qcom/hisense/a6l/adsp.mdt > "$RP/firmware" 2>/dev/null
log "starting ADSP via $RP/state"
T0=$(date +%s)
echo start > "$RP/state" 2>"$EV/start.err"; RC=$?
log "start write rc=$RC state=$(cat $RP/state)"
i=0
while [ $i -lt $START_TIMEOUT_S ]; do
	st=$(cat "$RP/state"); [ "$st" = "running" ] && break
	bad_dmesg && break
	sleep 1; i=$((i+1))
done
log "post-start: state=$(cat $RP/state) after ${i}s"
snap started
if [ "$(cat $RP/state)" != "running" ] || bad_dmesg; then
	fail "ADSP did not reach running cleanly (state=$(cat $RP/state))"
	dmesg | grep -E 'remoteproc|q6v5|pas|smmu|glink|smp2p|scm' | tail -60 > "$EV/failure-excerpt.txt"
else
	RESULT=PASS-START
	# service discovery evidence while running (read-only)
	[ -x "$DIR/a6l_qrtr_lookup" ] && "$DIR/a6l_qrtr_lookup" 3000 > "$EV/qrtr-services.txt" 2>&1
	ls -l /sys/bus/rpmsg/devices/ > "$EV/rpmsg-running.txt" 2>&1
	i=0
	while [ $i -lt $HOLD_S ]; do
		bad_dmesg && { fail "stop condition in dmesg during hold at ${i}s"; break; }
		[ "$(cat $RP/state)" = "running" ] || { fail "state changed during hold: $(cat $RP/state)"; break; }
		sleep 1; i=$((i+1))
	done
	log "hold finished after ${i}s state=$(cat $RP/state)"
	[ -x "$DIR/a6l_qrtr_lookup" ] && "$DIR/a6l_qrtr_lookup" 3000 > "$EV/qrtr-services-end.txt" 2>&1
	snap hold
fi
# ---- phase 3: clean stop -----------------------------------------------------
if [ "$(cat $RP/state)" != "offline" ]; then
	log "stopping ADSP"
	echo stop > "$RP/state" 2>"$EV/stop.err"; log "stop write rc=$?"
	i=0; while [ $i -lt 15 ] && [ "$(cat $RP/state)" != "offline" ]; do sleep 1; i=$((i+1)); done
	log "post-stop: state=$(cat $RP/state) after ${i}s"
	[ "$(cat $RP/state)" = "offline" ] || fail "ADSP did not stop within 15s"
fi
snap stopped
dmesg | grep -E 'remoteproc|q6v5|qcom_q6v5_pas|mdt|scm|smmu|glink|smp2p|sysmon|qrtr|rpmsg|pd-mapper|pdr' > "$EV/dmesg-adsp-filtered.txt"
[ "$RESULT" = PASS-START ] && [ "$(cat $RP/state)" = "offline" ] && RESULT=PASS
log "=== RESULT: $RESULT"
echo "$RESULT" > "$EV/RESULT"
exit 0
