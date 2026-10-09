#!/vendor/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# a6l-chg-guard (agent power, 26 Sep 2026): software JEITA + input-current policy for the mainline qcom_smbx charger,
# which has none (it programs FCC 1.95 A and float 4.4 V once and never looks at the battery temperature).
# Thresholds = stock DT (fg-gen3 qcom,fg-jeita-thresholds = <0 10 44 55>, hyst 2 C; qpnp-smb2 warm-fcc 0.9 A,
# warm-fv-comp 0.3 V, cool-fcc 0.85 A, usb-icl 2.0 A). Only two knobs exist in the driver: the USB input limit
# (current_max) and the USB input suspend (status 0/1). pwr27 (27 Sep): thresholds aligned with the FG hardware JEITA
# actually programmed on this phone (0/10/45/55 C, read by run-power.sh chg-ro); qcom_smbx (A6L patch) also enables
# the PMIC JEITA hard limit (0/55 C) and takes fcc_max_ua. So:
#   T <  0 C  or T >= 55 C   -> COLD/HOT : input suspended (no charging); resumes at 2 C / below 53 C
#   0 <= T < 10 C            -> COOL     : input limit min(source, 700 mA) (~0.85 A into the battery)
#   10 <= T < 45 C           -> NORMAL   : input limit by charger type (SDP 500 mA, CDP 1.5 A, DCP 2.0 A = stock usb-icl)
#   45 <= T < 55 C           -> WARM     : input limit min(source, 750 mA) (~0.9 A into the battery = stock warm-fcc), and input suspended while Vbat >= 4.10 V (stock warm float
#                                          4.4 - 0.3 V; resumes below 4.05 V) because the float voltage cannot be lowered
#   Vbat >= 4.42 V (any T)   -> OV guard : input suspended until 4.35 V
# State goes to the kernel log (A6L_CHG_GUARD) and to vendor.a6l.chg.state. Started by init after the charger group
# (persist.vendor.a6l.charger=1). ONESHOT=1 evaluates once (tests).
# merge r4 (28 Sep 2026): ROM limits = stock values: qcom_smbx fcc_max_ua=2400000 (rom/modules/charger.txt), float 4.40 V,
# DCP input 2.0 A; warm/cool FCC 0.9/0.85 A are approximated by the input limit (the driver has no runtime FCC knob).
# r5 review fixes (28 Sep 2026, hardware-review F1/F2):
#  F1 the requested input limit is min(source budget, thermal budget) in EVERY zone; unknown/SDP stays 500 mA
#     (the thermal limit can only lower the source budget, never raise it). The kernel may still clamp lower (HVDCP/AICL).
#  F2 the suspend state is reconciled: desired vs applied. The applied state only changes after a successful write;
#     a failed write is retried (RETRY immediate attempts per period, every period) and logged (first + every 20th),
#     and the applied state becomes unknown. At startup the applied state is unknown, so the desired state is always
#     written explicitly (the power_supply 'status' text is not a readback of the USBIN suspend bit).
#     vendor.a6l.chg.state = <zone>/<desired>/<applied|?> (+ /err while a write is failing).
# r5 bug hunt power (29 Sep 2026):
#  P1 qcom_smbx reprograms the input limit itself ~1.5 s after every plug-in / APSD result (smb_status_change_work:
#     SDP 500 mA, CDP 1.5 A, DCP 2.0 A), which overrode the cool/warm limit until the next 15 s period. The period is
#     now slept in TICK slices (1 s); a change of online/usb_type, or an effective limit (current_max) above the
#     requested one, re-evaluates at once.
#  P2 a telemetry glitch (nodata) no longer resets the zone / warm-voltage hysteresis: the last valid zone is kept, so
#     e.g. hot (resume only below 53 C) does not resume at 54 C after one failed temperature read.
# fastcharge-rom (29 Sep 2026, docs/fastcharge-rom-20260929.md): Quick Charge (qcom_smbx hvdcp_enable=1 in charger.txt,
# adapter 5-9 V, ICL at 9 V = hvdcp_icl_ua 2.0 A = stock hvdcp-usb-icl). The same input limit gives ~1.8x the battery
# current at 9 V, so the thermal/OV limits above are only valid at 5 V:
#  Q1 hvdcp_enable=0 (driver: back to 5 V) is written FIRST, and the guard waits (<= 4 s) for the driver to leave
#     verify/active, before any cool/warm input limit or input suspend (cold/hot/warm-voltage hold/OV/no telemetry).
#  Q2 hvdcp_enable=1 only in the normal zone with nothing suspended, AFTER the 5 V limits were written; re-enabled only
#     inside 12.0-43.0 C (2 C hysteresis, so no APSD rerun per 0.1 C around 10/45 C). persist.vendor.a6l.chg.qc=0 keeps 5 V.
#  Q3 while the driver owns the input limit (HVDCP idle/wait/verify: <= 1 A until 9 V is verified) the guard does not write
#     current_max in the normal zone (its 2.0 A write would bypass the ramp limit); its lower limits still go through.
#  Q4 USB gadget D+ pull-up: the ROM keeps the UDC soft-connected (a6l_manual_usb), so on a wall charger BC1.2/APSD sees
#     OCP/FLOAT instead of DCP and the QC handshake (D+ 0.6 V) cannot run. Unplugged or [DCP] -> soft_connect disconnect
#     (re-asserted every evaluation on DCP), [SDP]/[CDP] -> connect (adb/MTP enumerate ~1-2 s after plug-in); stock only
#     starts the peripheral on SDP/CDP. Once per plug-in, when the driver settled at dcp-5v with the pull-up already off,
#     one hvdcp_rerun (new APSD with free D+/D-). persist.vendor.a6l.chg.pullup=0 leaves the gadget alone. SIGTERM
#     (stop a6l_chg_guard) reconnects the pull-up first.
#  Q5 hvdcp_status state changes go to the kernel log (A6L_CHG_GUARD qc ...) and to vendor.a6l.chg.qc.
#  Q6 r6c (30 Sep 2026, docs/rom-r6c-20260930.md): the gadget owners (init.a6l.usb.rc early-adb rule, gadget HAL) bind the
#     UDC AND release the D+ hold themselves. The guard only overrides the pull-up of a gadget that is bound (UDC
#     `function` non-empty) at this AND the previous evaluation (never while it is being bound), and writes `connect` only
#     to undo its own `disconnect`. r6b: with no gadget bound it re-wrote soft_connect at every evaluation ("udc a800000.usb:
#     soft-connect without a gadget driver", -EOPNOTSUPP, every ~19 s).
#  Q7 r6d (30 Sep 2026, docs/rom-r6d-20260930.md): r6c on the laptop port logged "usb: soft_connect disconnect (online=0
#     [Unknown] SDP DCP CDP)" while adb was bound (sys.usb.state=adb) and the UDC dropped off the bus ("UDC had already
#     stopped", adbd re-read its descriptors): pm660-charger reported online=0/[Unknown] for a moment on a USB host. The
#     guard now disconnects ONLY on a confirmed wall charger: online=1 AND usb_type [DCP] AND the UDC has not seen a host
#     (/sys/class/udc/<udc>/state not configured/addressed/default/suspended). Unplugged (online!=1), [Unknown], [SDP],
#     [CDP] or any other type never disconnect; our own earlier disconnect is kept while unplugged (the next APSD sees free
#     D+/D-, as before) and undone on anything but [DCP] (or on a host-side bus state, e.g. a DCP misreport on a host).
#  Q8 (9 Oct 2026, firmware/extracted/bms-20261009): moving the cable from the laptop to a wall charger leaves the UDC
#     state "configured" (the manual-connect UDC never sees the host go away), so host_seen() stayed true on the wall
#     charger, the pull-up was never released, APSD kept reporting OCP and QC never ran (5 V only). The APSD result is
#     fresher than the UDC state: on [DCP] with an APSD result of OCP/FLOAT (D+/D- disturbed; a USB host gives SDP/CDP
#     and the driver refuses hvdcp_rerun there) the stale host state is ignored. Self-correcting: if the rerun reports
#     SDP/CDP (a host after all), usb_type leaves [DCP] and the pull-up is reconnected by the branch below.
#  Q9 (9 Oct 2026, round 24 regression): at boot on the LAPTOP the APSD ran with the gadget pull-up already up and read
#     OCP, Q8 trusted it, disconnected, and USB stayed off (no adb until a re-plug). Now Q8 applies only after this guard
#     saw the cable unplugged (online != 1) at least once, i.e. never on the boot-time state; and a safety net
#     reconnects any pull-up we removed once it has been off for PU_MAX (4) evaluations (~1 min) without QC reaching
#     wait/verify/active, and then leaves the pull-up alone until the next unplug.
BAT=${BAT:-/sys/class/power_supply/qcom-battery}; CHG=${CHG:-/sys/class/power_supply/pm660-charger}
PERIOD=${PERIOD:-15}; RETRY=${RETRY:-3}
# P1: the period is slept in TICKS slices of TICK s (default 1 s; a fractional PERIOD = one slice, as before)
case "$PERIOD" in *[!0-9]*) TICK=${TICK:-$PERIOD};; *) TICK=${TICK:-1};; esac
case "$PERIOD$TICK" in *[!0-9]*) TICKS=${TICKS:-1};; *) TICKS=${TICKS:-$((PERIOD / TICK))};; esac
[ "$TICKS" -ge 1 ] 2>/dev/null || TICKS=1
COLD=0; COOL=100; WARM=450; HOT=550; HYST=20          # deci-degC (pwr27: warm 45 C = FG hardware JEITA read on the phone 27 Sep)
WARM_VMAX=4100000; WARM_VRES=4050000; OV=4420000; OV_RES=4350000
COOL_ICL=700000; WARM_ICL=750000; SRC_DEFAULT=500000
HVP=${HVP:-/sys/module/qcom_smbx/parameters}; UDCC=${UDCC:-/sys/class/udc}
HVHYST=20; HV_WAIT_N=${HV_WAIT_N:-20}; HV_WAIT_STEP=${HV_WAIT_STEP:-0.2}
log() { echo "A6L_CHG_GUARD $*"; }
rd() { cat "$1" 2>/dev/null; }
# write in a subshell: a failed redirection must not abort the daemon; the exit status reports the write result
wr() { [ -e "$1" ] && ( echo "$2" > "$1" ) 2>/dev/null; }
state=""; zone=""; want=0; applied=""; serr=0; ierr=0; last=""; whold=0; icl=""; seen=""
pu=""; pu_off=0; rr=0; hvlast=""; gb_prev=0
# Q1-Q5 helpers. Properties are read at every evaluation (setprop takes effect without a restart); env overrides (tests).
prop() { getprop "$1" 2>/dev/null; }
qc_on() { v=${QC:-$(prop persist.vendor.a6l.chg.qc)}; [ "$v" != 0 ]; }
apsd_disturbed() {  # Q8: APSD result (hvdcp_status apsd=0xSS/0xRR) has OCP (0x02) or FLOAT (0x10)
    x=$(rd $HVP/hvdcp_status); x=${x#*apsd=0x}; x=${x#*/0x}; x=${x%% *}
    case "$x" in [0-9a-fA-F][0-9a-fA-F]) [ $(( 0x$x & 0x12 )) -ne 0 ];; *) return 1;; esac
}
PU_MAX=${PU_MAX:-4}
pu_managed() { v=${PULLUP:-$(prop persist.vendor.a6l.chg.pullup)}; [ "$v" != 0 ]; }
hv_avail() { [ -e $HVP/hvdcp_enable ]; }
hv_get() { case "$(rd $HVP/hvdcp_enable)" in Y|1) echo 1;; N|0) echo 0;; esac; }
hv_state() { x=$(rd $HVP/hvdcp_status); x=${x#state=}; echo "${x%% *}"; }
hv_set() {          # $1 = 0/1, $2 = reason. 0: wait for the driver to be back at 5 V (it owns FORCE_5V / QC3 decrements)
    hv_avail || return 0
    [ "$(hv_get)" = "$1" ] && return 0
    if ! wr $HVP/hvdcp_enable $1; then log "ERROR: write hvdcp_enable=$1 failed ($2)"; return 1; fi
    log "qc: hvdcp_enable=$1 ($2)"
    [ "$1" = 1 ] && return 0
    k=0
    while [ $k -lt $HV_WAIT_N ]; do
        case "$(hv_state)" in verify|active) sleep $HV_WAIT_STEP; k=$((k + 1));; *) return 0;; esac
    done
    log "ERROR: still $(hv_state) ${HV_WAIT_N}x${HV_WAIT_STEP}s after hvdcp_enable=0; applying the 5 V limits anyway"
    return 1
}
hv_want() {         # Q2: normal zone, nothing suspended, policy on; re-enable only inside 12.0-43.0 C
    qc_on && hv_avail || return 1
    [ "$state" = normal ] && [ "$want" = 0 ] || return 1
    [ "$(hv_get)" = 1 ] && return 0
    [ "$t" -ge $((COOL + HVHYST)) ] && [ "$t" -lt $((WARM - HVHYST)) ]
}
udc_name() { u=${UDC:-}; [ -n "$u" ] || u=$(ls $UDCC 2>/dev/null | head -n 1); echo "$u"; }   # the single UDC (a800000.usb)
host_seen() {       # Q7: the UDC saw a USB host (bus reset or later): never a wall charger, never disconnect
    u=$(udc_name); case "$(rd $UDCC/$u/state | tr -d ' \n')" in configured|addressed|default|suspended) return 0;; esac; return 1
}
gadget_bound() {    # Q6: a gadget driver is bound to the UDC (configfs g1 via init or the HAL)
    u=$(udc_name); [ -n "$u" ] && [ -n "$(rd $UDCC/$u/function | tr -d ' \n')" ]
}
pu_write() {        # $1 = connect/disconnect; Q6: only on a gadget bound at this and the previous evaluation
    u=$(udc_name)
    [ -n "$u" ] && [ -e $UDCC/$u/soft_connect ] || return 1
    [ "$gb" = 1 ] && [ "$gb_prev" = 1 ] || return 1
    [ "$1" = connect ] && [ "$pu" != disconnect ] && return 0        # never pull up what the gadget owner did not hand us
    if wr $UDCC/$u/soft_connect $1; then
        [ "$pu" != "$1" ] && log "usb: soft_connect $1 (online=$(rd $CHG/online) $(rd $CHG/usb_type))"
        pu=$1; return 0
    fi
    return 1
}
pullup() {          # Q4: gadget D+ pull-up off on wall chargers / unplugged, on for USB hosts; one APSD rerun per plug-in
    gb_prev=${gb:-0}; gadget_bound && gb=1 || gb=0
    [ $gb = 0 ] && [ "$pu" = disconnect ] && pu=""                  # unbound: the next bind (owner) pulls up itself
    if ! pu_managed; then [ "$pu" = disconnect ] && pu_write connect; return 0; fi
    ty=$(rd $CHG/usb_type)
    if [ "$(rd $CHG/online)" != 1 ]; then
        rr=0; pu_off=0; unplug_seen=1; pu_hold=0                      # Q7: unplugged/unknown never disconnects
        host_seen && [ "$pu" = disconnect ] && pu_write connect
    elif [ "${pu_hold:-0}" = 1 ]; then
        [ "$pu" = disconnect ] && pu_write connect                    # Q9: safety net fired; hands off until unplug
    elif host_seen && ! { [ "${unplug_seen:-0}" = 1 ] && case "$ty" in *'[DCP]'*) apsd_disturbed;; *) false;; esac; }; then
        pu_off=0; [ "$pu" = disconnect ] && pu_write connect         # Q7: a host is on the bus, whatever usb_type says
    else case "$ty" in
        *'[DCP]'*)
            pu_write disconnect && pu_off=$((pu_off + 1))
            if [ $pu_off -ge $PU_MAX ]; then                          # Q9: never leave USB dead
                case "$(hv_state)" in wait|verify|active) ;; *)
                    pu_hold=1; pu_write connect; log "usb: pull-up restored after $pu_off checks without QC ($(rd $HVP/hvdcp_status))";;
                esac
            fi
            if [ $rr = 0 ] && [ $pu_off -ge 2 ] && [ "$(hv_get)" = 1 ] && [ "$(hv_state)" = dcp-5v ]; then
                rr=1; wr $HVP/hvdcp_rerun 1 && log "qc: APSD rerun requested with the USB pull-up off ($(rd $HVP/hvdcp_status))"
            fi;;
        *) pu_off=0; [ "$pu" = disconnect ] && pu_write connect;;    # SDP/CDP/[Unknown]/other: undo our own disconnect only
    esac; fi
}
qc_mon() {          # Q5
    hv_avail || return 0
    st=$(hv_state); [ "$st" = "$hvlast" ] && return 0
    hvlast=$st; log "qc $(rd $HVP/hvdcp_status)"; setprop vendor.a6l.chg.qc "$st" 2>/dev/null
}
trap 'gb_prev=${gb:-0}; gadget_bound && gb=1 || gb=0; [ "$pu" = disconnect ] && pu_write connect; exit 0' TERM INT
src_key() { echo "$(rd $CHG/online)|$(rd $CHG/usb_type)|$(hv_state)"; }
source_icl() {      # input budget of the detected source; anything unknown is treated as SDP
    case "$(rd $CHG/usb_type)" in
        *'[DCP]'*) echo 2000000;; *'[CDP]'*) echo 1500000;; *) echo $SRC_DEFAULT;;
    esac
}
apply_susp() {      # $1 = desired input suspend (1) / resume (0); status node: 0 = suspend, 1 = resume
    [ "$applied" = "$1" ] && return 0
    v=$((1 - $1)); i=0
    while [ $i -lt $RETRY ]; do
        if wr $CHG/status $v; then
            [ $serr -gt 0 ] && log "suspend=$1 applied after $serr failed round(s)"
            applied=$1; serr=0; return 0
        fi
        i=$((i + 1))
    done
    applied=""; serr=$((serr + 1))
    if [ $serr = 1 ] || [ $((serr % 20)) = 0 ]; then
        log "ERROR: write $CHG/status=$v (suspend=$1) failed ($RETRY attempts, round $serr); applied state unknown, retrying"
    fi
    return 1
}
apply_icl() {
    [ "$(rd $CHG/current_max)" = "$1" ] && { ierr=0; return 0; }
    if wr $CHG/current_max $1; then ierr=0; return 0; fi
    ierr=$((ierr + 1))
    if [ $ierr = 1 ] || [ $((ierr % 20)) = 0 ]; then log "ERROR: write $CHG/current_max=$1 failed (round $ierr)"; fi
    return 1
}
while :; do
    t=$(rd $BAT/temp); vb=$(rd $BAT/voltage_now); on=$(rd $CHG/online)
    case "$t$vb" in *[!0-9-]*) t="";; esac          # non-numeric telemetry = no data
    if [ -z "$t" ] || [ -z "$vb" ] || [ ! -e $CHG/current_max ]; then
        [ "$state" != nodata ] && log "no battery/charger data (fg or qcom_smbx not bound): suspending input as a precaution"
        state=nodata; want=1; icl=""; hv_set 0 nodata; apply_susp 1      # P2: zone + warm hold kept for the next valid sample
    else
        prev=$zone
        # temperature zone with hysteresis around the suspend zones
        if [ "$t" -lt $COLD ] || { [ "$prev" = cold ] && [ "$t" -lt $((COLD + HYST)) ]; }; then state=cold
        elif [ "$t" -ge $HOT ] || { [ "$prev" = hot ] && [ "$t" -ge $((HOT - HYST)) ]; }; then state=hot
        elif [ "$t" -lt $COOL ]; then state=cool
        elif [ "$t" -ge $WARM ]; then state=warm
        else state=normal; fi
        zone=$state
        want=0
        case $state in cold|hot) want=1;; esac
        # warm: suspend at >= 4.10 V, hold (desired state, not the applied one) until < 4.05 V
        if [ $state = warm ] && { [ "$vb" -ge $WARM_VMAX ] || { [ $whold = 1 ] && [ "$prev" = warm ] && [ "$vb" -ge $WARM_VRES ]; }; }; then
            whold=1; want=1
        else whold=0; fi
        if [ "$vb" -ge $OV ] || { [ "$last" = ov ] && [ "$vb" -ge $OV_RES ]; }; then want=1; last=ov; else [ "$last" = ov ] && last=""; fi
        src=$(source_icl)
        case $state in cool|cold|hot) th=$COOL_ICL;; warm) th=$WARM_ICL;; *) th=$src;; esac
        icl=$src; [ $th -lt $icl ] && icl=$th        # F1: effective = min(source budget, thermal budget)
        if hv_want; then
            # Q2/Q3: 5 V limits first, then QC; the driver owns the input limit during its wait/verify ramp (<= 1 A)
            case "$(hv_state)" in idle|wait|verify) [ "$(hv_get)" = 1 ] || apply_icl $icl;; *) apply_icl $icl;; esac
            apply_susp $want
            [ "$applied" = 0 ] && hv_set 1 "$state T=$t"
        else
            hv_set 0 "$state T=$t suspend=$want${last:+ ov}"      # Q1: back to 5 V BEFORE the lower limit / suspend
            apply_icl $icl
            apply_susp $want
        fi
        tag="$state icl=$icl suspended=$want applied=${applied:-?} online=$on T=$t Vbat=$vb${last:+ ov}"
        [ "$tag" != "$lasttag" ] && [ "${state}${want}${applied}${icl}" != "$lastkey" ] && log "$tag"
        lastkey="${state}${want}${applied}${icl}"; lasttag=$tag
    fi
    pullup; qc_mon
    err=""; [ "$applied" != "$want" ] && err=/err
    setprop vendor.a6l.chg.state "$state/$want/${applied:-?}$err" 2>/dev/null
    [ "${ONESHOT:-0}" = 1 ] && exit 0
    # P1: sleep the period in slices; wake early on a source change or when the kernel raised the input limit
    seen=$(src_key); n=0
    while [ $n -lt $TICKS ]; do
        sleep $TICK; n=$((n + 1))
        [ "$(src_key)" != "$seen" ] && break
        if [ -n "$icl" ] && [ $ierr = 0 ]; then
            cur=$(rd $CHG/current_max)
            case "$cur" in ''|*[!0-9]*) ;; *) [ "$cur" -gt "$icl" ] && break;; esac
        fi
    done
done
