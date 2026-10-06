#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# run-power.sh (agent power, 26 Sep 2026) - attended power tests on the V74 recovery, as root, bundle dir D (default
# /tmp/power). Always `export PATH=/tmp/bin:$PATH` first. docs/power-20260926.md has the full procedure.
# CHARGER (test C2, CURRENT-AFFECTING from chg-on on, Pierre present, phone on the desk, not in a case):
#   MODE=chg-ro       apply a6l_chg_ovl (DT only), load qcom-spmi-rradc + pmi8998_fg -> READ-ONLY battery/usbin telemetry,
#                     FG hardware JEITA thresholds. qcom_smbx is NOT loaded (the PMIC keeps its bootloader charge settings).
#   MODE=chg-on       [ICL=500000] [FCC=1000000] insmod the A6L-patched qcom_smbx with fcc_max_ua=FCC (max 1000000 without
#                     FORCE=1) and jeita_hard=1 (writes the SMB2 init sequence, FCC cap, float 4.40 V, charging enabled), set the
#                     USB input limit to ICL (max 800000 without FORCE=1, never > 1000000), then start the monitor in the
#                     background: every 10 s for DUR=300 s -> /tmp/a6l-chg.log; ABORT (USB input suspend) at battery
#                     temp >= T_ABORT (default 50.0 C; stock JEITA hot = 55 C; start refused >= T_START_MAX 45.0 C, 28 Sep), charge current > 1.05 A twice, health != Good, or battery > 4.42 V.
#   MODE=chg-status   tail the monitor log.      MODE=chg-off   HVDCP off (back to 5 V) + USB input suspend now (stops charging; ADB keeps working).
# QUICK CHARGE (power28, 28 Sep; Pierre present + a Quick Charge 3.0 (or 2.0) WALL charger; docs/fastcharge-20260928.md):
#   MODE=qc           [QICL=1000000] [QV=9000000] [FCC=1500000] start ON THE LAPTOP CABLE (ADB). Loads the power28 qcom_smbx
#                     (hvdcp_enable=0, fcc_max_ua=FCC, jeita_hard=1, hvdcp_max_uv=QV, hvdcp_icl_ua=QICL) if not loaded, then
#                     hvdcp_enable=1 and starts the QC monitor in the background (every 5 s for DUR=1200 s -> /tmp/a6l-qc.log).
#                     Then move the cable to the QC wall charger: the driver sees QC2/QC3 (APSD), asks for 9 V, checks USBIN.
#                     Monitor ABORT (hvdcp_enable=0 -> 5 V, then USB input suspend): battery temp >= T_ABORT (50.0 C) or < 0 C,
#                     battery > 4.42 V, USBIN > 9.6 V, USBIN current > QICL + 10 % + 100 mA twice, battery current > FCC + 5 %
#                     + 50 mA twice, charger health not Good. Limits: QICL <= 1500000 (FORCE=1: <= 2000000), QV <= 9000000,
#                     FCC <= 1500000 (FORCE=1: <= 2400000). End of DUR: hvdcp_enable=0 (5 V), ICL 500 mA.
#   power29 (29 Sep): MODE=qc also stops the V75 USB watchdog (it re-connects the gadget) and, with QDISC=1 (default), the
#                     monitor samples every QSLEEP=2 s and removes the USB gadget D+ pull-up (UDC soft_connect=disconnect)
#                     as soon as the laptop cable is pulled, so APSD/HVDCP on the wall charger sees free D+/D- lines (the
#                     29 Sep QC1 test: both chargers were "DCP"-typed, HVDCP never started); if the driver still ends at
#                     5 V (state dcp-5v) it asks for ONE APSD rerun (hvdcp_rerun=1). Back on SDP/CDP (laptop) or at the
#                     end: soft_connect=connect + watchdog restarted. Every QCMON line carries the driver hvdcp_regs dump.
#   MODE=qc-status    tail /tmp/a6l-qc.log + driver hvdcp_status.      MODE=qc-off = MODE=chg-off.
#   MODE=chg-resume   clear the input suspend (charging at the current ICL).
# CPR (voltage):
#   MODE=fuserows     read ONLY qfprom rows 38 + 65..71 (the rows the stock kernel reads; never rows 0..37 which reset the
#                     phone on 24 Sep) -> /tmp/a6l-fuserows.bin. Stream `adb shell dmesg -w` on the laptop first.
#   MODE=cpr-check    (CPR kernel only, read-only) cpr3/cpufreq probe state + qcom_cpr3 debugfs corners -> compare on the
#                     laptop: python3 a6l_cpr_openloop.py --stock-log stock-cpr.txt --mainline cpr-check.txt
#   MODE=cpr-step     (CPR kernel only, VOLTAGE) userspace governor, step each policy through its frequencies up to
#                     PMAX0/PMAX4 (kHz), HOLD s each with load, abort above TMAX (default 70 C) on any thermal zone.
D=${D:-/tmp/power}; MODE=${MODE:-chg-ro}
say() { echo "A6L_PWR $*"; }
BAT=${BAT:-/sys/class/power_supply/qcom-battery}; CHG=${CHG:-/sys/class/power_supply/pm660-charger}; LOG=${LOG:-/tmp/a6l-chg.log}
HVP=${HVP:-/sys/module/qcom_smbx/parameters}; QLOG=${QLOG:-/tmp/a6l-qc.log}
UDCC=${UDCC:-/sys/class/udc}; SETPROP=${SETPROP:-setprop}; GETPROP=${GETPROP:-getprop}; WDMARK=${WDMARK:-/tmp/a6l-qc-wd-stopped}
udc_set() {	# udc_set connect|disconnect -> writes every UDC soft_connect, logs
    for u in $UDCC/*; do [ -e "$u/soft_connect" ] || continue; echo "$1" > "$u/soft_connect" 2>/dev/null && say "USB gadget ${u##*/} soft_connect=$1" || say "WARN soft_connect=$1 failed on ${u##*/}"; done
}
wd_restore() {	# restart the V75 USB watchdog if MODE=qc stopped it
    [ -e $WDMARK ] || return 0
    $SETPROP ctl.start a6lusbwd 2>/dev/null && say "USB watchdog a6lusbwd restarted" || say "WARN could not restart a6lusbwd"
    rm -f $WDMARK
}
v() { cat "$1" 2>/dev/null || echo NA; }
iio() {	# rradc processed channels (read-only conversions)
    for d in /sys/bus/iio/devices/iio:device*; do
        case "$(cat $d/name 2>/dev/null)" in *rradc*) ;; *) continue;; esac
        for f in $d/in_*_input $d/in_*_raw $d/in_*_scale $d/in_*_offset; do [ -e "$f" ] && printf '%s=%s ' "${f##*/}" "$(cat $f 2>/dev/null)"; done
    done
}
snap() {
    echo "t=$(cut -d' ' -f1 /proc/uptime) bat[status=$(v $BAT/status) cap=$(v $BAT/capacity) uV=$(v $BAT/voltage_now) uA=$(v $BAT/current_now) temp=$(v $BAT/temp)] chg[online=$(v $CHG/online) status=$(v $CHG/status) health=$(v $CHG/health) type=$(v $CHG/usb_type) icl=$(v $CHG/current_max) usbin_uA=$(v $CHG/current_now) usbin_uV=$(v $CHG/voltage_now)]"
}
maxtemp() { m=0; for z in /sys/class/thermal/thermal_zone*/temp; do t=$(cat $z 2>/dev/null); [ -n "$t" ] && [ "$t" -gt "$m" ] 2>/dev/null && m=$t; done; echo $m; }
case "$MODE" in
chg-ro)
    cd "$D" || exit 1
    if ! grep -q '^a6l_chg_ovl ' /proc/modules; then
        e=$(insmod ./a6l_chg_ovl.ko 2>&1) || say "WARN a6l_chg_ovl insmod failed: $e (see A6L_CHG_OVL dmesg below)"
    fi
    PM=/proc/device-tree/soc@0/spmi@800f000/pmic@0
    st_rr=$(tr -d '\0' < $PM/adc@4500/status 2>/dev/null); st_ch=$(tr -d '\0' < $PM/charger@1000/status 2>/dev/null)
    if [ "$st_rr" = okay ] && [ "$st_ch" = okay ] && [ -e $PM/battery@4000/power-supplies ] && [ -e $PM/charger@1000/monitored-battery ]; then
        say "CHG_DT_OK rradc=okay charger=okay fg.power-supplies=yes charger.monitored-battery=yes"
    else
        say "CHG_DT_FAIL rradc=${st_rr:-none} charger=${st_ch:-none} (overlay not applied: stop here, send dmesg)"
    fi
    grep -q '^qcom_spmi_rradc ' /proc/modules || insmod ./qcom-spmi-rradc.ko || say "WARN rradc insmod failed"
    grep -q '^pmi8998_fg ' /proc/modules || insmod ./pmi8998_fg.ko || say "WARN pmi8998_fg insmod failed"
    sleep 2; dmesg | grep -iE 'A6L_CHG_OVL|rradc|pmi8998|qcom-battery|fg|4500|4000.spmi' | tail -20 | sed 's/^/A6L_PWR_DMESG /'
    grep -q '^qcom_smbx ' /proc/modules && say "NOTE qcom_smbx already loaded (not read-only any more)"
    [ -d $BAT ] || { say "CHG_RO_FAIL no $BAT"; exit 1; }
    say "uevent: $(tr '\n' ' ' < $BAT/uevent)"
    say "FG hardware JEITA (C*10): cold=$(v $BAT/temp_min) cool=$(v $BAT/temp_alert_min) warm=$(v $BAT/temp_alert_max) hot=$(v $BAT/temp_max)  (27 Sep: 0/100/450/550)"
    r=$(iio); say "rradc: $r"
    case "$r" in *in_*) say "RRADC_OK $(echo "$r" | tr ' ' '\n' | grep -c '=') values";; *) say "RRADC_FAIL no pm660-rradc IIO values (driver not bound?)";; esac
    for i in 1 2 3; do say "RO $(snap)"; sleep 3; done
    say "CHG_RO_DONE (charging now = PMIC bootloader defaults; current_now < 0 means charging)" ;;
chg-on)
    cd "$D" || exit 1
    [ -d $BAT ] || { say "CHG_ON_FAIL run MODE=chg-ro first"; exit 1; }
    ICL=${ICL:-500000}; FCC=${FCC:-1000000}
    [ "$ICL" -gt 1000000 ] && { say "refused: ICL $ICL > 1000000"; exit 1; }
    [ "$FCC" -gt 1950000 ] && { say "refused: FCC $FCC > 1950000"; exit 1; }
    [ "$FCC" -gt 1000000 ] && [ "${FORCE:-0}" != 1 ] && { say "refused: FCC $FCC > 1000000 without FORCE=1"; exit 1; }
    [ "$ICL" -gt 800000 ] && [ "${FORCE:-0}" != 1 ] && { say "refused: ICL $ICL > 800000 without FORCE=1"; exit 1; }
    t=$(v $BAT/temp); [ "$t" != NA ] && [ "$t" -ge ${T_START_MAX:-450} ] && { say "refused: battery already at $t (>= ${T_START_MAX:-450}/10 C, stock JEITA warm zone); let it cool"; exit 1; }
    [ "$t" != NA ] && [ "$t" -lt 100 ] && { say "refused: battery at $t (< 10.0 C, JEITA cool zone); warm it to room temperature"; exit 1; }
    say "BEFORE $(snap)"
    grep -q fcc_max_ua ./qcom_smbx.ko || { say "refused: ./qcom_smbx.ko is not the A6L-patched build (no fcc_max_ua parameter)"; exit 1; }
    grep -q '^qcom_smbx ' /proc/modules || insmod ./qcom_smbx.ko fcc_max_ua=$FCC jeita_hard=1 || { say "CHG_ON_FAIL qcom_smbx insmod"; exit 1; }
    p=$(cat /sys/module/qcom_smbx/parameters/fcc_max_ua 2>/dev/null)
    [ "$p" = "$FCC" ] || say "WARN loaded qcom_smbx has fcc_max_ua=${p:-none} (was it loaded before this run?)"
    i=0; while [ $i -lt 20 ] && [ ! -e $CHG/current_max ]; do sleep 0.2; i=$((i+1)); done
    [ -e $CHG/current_max ] || { say "CHG_ON_FAIL no $CHG (probe deferred? rradc?)"; dmesg | grep -iE 'smb|charger|iio' | tail -8; exit 1; }
    sleep 1   # let smb_status_change_work set its per-type ICL first, then override it
    echo $ICL > $CHG/current_max; say "ICL set: $(v $CHG/current_max) uA (type $(v $CHG/usb_type)); FCC cap $(v /sys/module/qcom_smbx/parameters/fcc_max_ua) uA, JEITA hard $(v /sys/module/qcom_smbx/parameters/jeita_hard)"
    dmesg | grep -iE 'smb|pm660-charger|charger' | tail -6 | sed 's/^/A6L_PWR_DMESG /'
    MODE=chg-mon ICL=$ICL nohup sh "$D/run-power.sh" > $LOG 2>&1 < /dev/null &
    sleep 1; say "monitor started (pid $!), log $LOG; poll with MODE=chg-status; stop with MODE=chg-off" ;;
chg-mon)
    DUR=${DUR:-300}; n=0; hot=0; over=0; pass=""; say "MON start ICL=$ICL DUR=$DUR"
    while [ $n -le $DUR ]; do
        s=$(snap); t=$(v $BAT/temp); ua=$(v $BAT/current_now); uv=$(v $BAT/voltage_now); h=$(v $CHG/health)
        say "MON $s"
        why=""
        [ "$t" != NA ] && [ "$t" -ge ${T_ABORT:-500} ] && why="battery temp $t >= ${T_ABORT:-500}/10 C"
        [ "$t" != NA ] && [ "$t" -lt 0 ] && why="battery temp $t < 0 C"
        [ "$uv" != NA ] && [ "$uv" -gt 4420000 ] && why="battery voltage $uv > 4.42 V"
        [ "$h" != NA ] && [ "$h" != Good ] && [ "$h" != Unknown ] && why="charger health $h"
        if [ "$ua" != NA ] && [ "$ua" -lt -1050000 ]; then over=$((over+1)); else over=0; fi
        [ $over -ge 2 ] && why="charge current $ua uA beyond 1.05 A twice"
        o=$(v $CHG/online); st=$(v $BAT/status)
        [ "$o" = 1 ] && [ "$st" = Charging ] && [ "$ua" != NA ] && [ "$ua" -lt 0 ] && [ -z "$pass" ] && { pass=1; say "CHG_ON_PASS charging: online=1 status=Charging uA=$ua"; }
        if [ -n "$why" ]; then echo 0 > $CHG/status; say "ABORT $why -> USB input suspended; $(snap)"; exit 3; fi
        sleep 10; n=$((n+10))
    done
    [ "${END:-keep}" = suspend ] && echo 0 > $CHG/status
    echo 500000 > $CHG/current_max 2>/dev/null
    say "MON_DONE $(snap) (ICL back to 500 mA; END=${END:-keep})" ;;
chg-status) tail -${N:-8} $LOG ;;
chg-off|qc-off)
    if [ -e $HVP/hvdcp_enable ] && [ "$(v $HVP/hvdcp_enable)" != N ]; then
        echo 0 > $HVP/hvdcp_enable; sleep ${QOFF_WAIT:-3}; say "HVDCP off -> 5 V: $(v $HVP/hvdcp_status)"
    fi
    wd_restore
    echo 0 > $CHG/status && say "USB input SUSPENDED (not charging) $(snap)" ;;
qc)
    cd "$D" || exit 1
    [ -d $BAT ] || { say "QC_FAIL run MODE=chg-ro first"; exit 1; }
    QICL=${QICL:-1000000}; QV=${QV:-9000000}; FCC=${FCC:-1500000}
    [ "$QV" -gt 9000000 ] && { say "refused: QV $QV > 9000000 (PM660 max, 12 V never)"; exit 1; }
    [ "$QICL" -gt 2000000 ] && { say "refused: QICL $QICL > 2000000 (stock hvdcp-usb-icl)"; exit 1; }
    [ "$QICL" -gt 1500000 ] && [ "${FORCE:-0}" != 1 ] && { say "refused: QICL $QICL > 1500000 without FORCE=1"; exit 1; }
    [ "$FCC" -gt 2400000 ] && { say "refused: FCC $FCC > 2400000 (stock)"; exit 1; }
    [ "$FCC" -gt 1500000 ] && [ "${FORCE:-0}" != 1 ] && { say "refused: FCC $FCC > 1500000 without FORCE=1"; exit 1; }
    t=$(v $BAT/temp); [ "$t" != NA ] && [ "$t" -ge ${T_START_MAX:-450} ] && { say "refused: battery already at $t (>= ${T_START_MAX:-450}/10 C, stock JEITA warm zone); let it cool"; exit 1; }
    [ "$t" != NA ] && [ "$t" -lt 100 ] && { say "refused: battery at $t (< 10.0 C, JEITA cool zone); warm it to room temperature"; exit 1; }
    if grep -q '^qcom_smbx ' /proc/modules 2>/dev/null; then
        [ -e $HVP/hvdcp_enable ] || { say "refused: the loaded qcom_smbx has no HVDCP (chg-on loaded an older build): fresh boot, MODE=chg-ro, then MODE=qc"; exit 1; }
        say "qcom_smbx already loaded: fcc_max_ua=$(v $HVP/fcc_max_ua) hvdcp_max_uv=$(v $HVP/hvdcp_max_uv) (insmod values kept)"
    else
        grep -q hvdcp_enable ./qcom_smbx.ko || { say "refused: ./qcom_smbx.ko is not the power28 build (no hvdcp_enable parameter)"; exit 1; }
        insmod ./qcom_smbx.ko fcc_max_ua=$FCC jeita_hard=1 hvdcp_enable=0 hvdcp_max_uv=$QV hvdcp_icl_ua=$QICL || { say "QC_FAIL qcom_smbx insmod"; exit 1; }
        i=0; while [ $i -lt 20 ] && [ ! -e $CHG/current_max ]; do sleep 0.2; i=$((i+1)); done
        [ -e $CHG/current_max ] || { say "QC_FAIL no $CHG (probe deferred? rradc?)"; exit 1; }
    fi
    [ -e $HVP/hvdcp_rerun ] && [ -e $HVP/hvdcp_regs ] || say "WARN the loaded qcom_smbx has no hvdcp_rerun/hvdcp_regs (not the power29 build): no APSD rerun, no register dump"
    if [ "${QDISC:-1}" = 1 ]; then
        w=$($GETPROP init.svc.a6lusbwd 2>/dev/null)
        if [ "$w" = running ]; then $SETPROP ctl.stop a6lusbwd 2>/dev/null && { : > $WDMARK; say "USB watchdog a6lusbwd stopped for the QC test (restarted by the monitor at the end)"; } || say "WARN could not stop a6lusbwd (it may re-connect the gadget on the wall charger)"
        else say "USB watchdog a6lusbwd: ${w:-not present}"; fi
    fi
    echo $QICL > $HVP/hvdcp_icl_ua; echo 1 > $HVP/hvdcp_enable
    say "HVDCP on: max $(v $HVP/hvdcp_max_uv) uV, ICL at 9 V $(v $HVP/hvdcp_icl_ua) uA, FCC cap $(v $HVP/fcc_max_ua) uA, JEITA hard $(v $HVP/jeita_hard)"
    say "status: $(v $HVP/hvdcp_status)"
    say "BEFORE $(snap)"
    say "regs $(v $HVP/hvdcp_regs)"
    dmesg | grep -iE 'A6L: |A6L_HVDCP' | tail -4 | sed 's/^/A6L_PWR_DMESG /'
    MODE=qc-mon QICL=$(v $HVP/hvdcp_icl_ua) nohup sh "$D/run-power.sh" > $QLOG 2>&1 < /dev/null &
    sleep 1; say "QC monitor started (pid $!), log $QLOG. NOW: unplug the laptop cable, count to 5, plug the phone into the Quick Charge wall charger."
    say "Back on the laptop: MODE=qc-status (log), MODE=chg-off (5 V + suspend)." ;;
qc-mon)
    DUR=${DUR:-1200}; n=0; bi=0; ii=0; pass=""; drv=""; fcc=$(v $HVP/fcc_max_ua); [ "$fcc" = NA ] && fcc=1000000
    QDISC=${QDISC:-1}; QSTEP=${QSTEP:-2}; disc=""; rer=""; wall=0; vmax=0
    say "QCMON start QICL=$QICL FCC=$fcc DUR=$DUR QDISC=$QDISC step=${QSTEP}s"
    num() { echo "$2" | sed -n "s/.*$1=\(-*[0-9][0-9]*\).*/\1/p"; }
    qc_restore() { [ -n "$disc" ] && { udc_set connect; disc=""; }; wd_restore; }
    while [ $n -le $DUR ]; do
        hs=$(v $HVP/hvdcp_status); t=$(v $BAT/temp); ua=$(v $BAT/current_now); uv=$(v $BAT/voltage_now); h=$(v $CHG/health)
        qv=$(num usbin_uV "$hs"); qi=$(num usbin_uA "$hs"); [ -z "$qv" ] && qv=NA; [ -z "$qi" ] && qi=NA
        [ "$qv" != NA ] && [ "$qv" -gt "$vmax" ] && vmax=$qv
        say "QCMON $(snap) hv[$hs] regs[$(v $HVP/hvdcp_regs)]"
        # power29: USB gadget pull-up off while not on a USB host, back on for SDP/CDP; one APSD rerun if still 5 V
        o=$(v $CHG/online); ty=$(v $CHG/usb_type)
        if [ "$QDISC" = 1 ]; then
            if [ "$o" != 1 ]; then
                [ -z "$disc" ] && { udc_set disconnect; disc=1; }; rer=""
            else
                case "$ty" in
                *"[SDP]"*|*"[CDP]"*) [ -n "$disc" ] && { udc_set connect; disc=""; }; rer="" ;;
                *"[DCP]"*)
                    wall=1
                    [ -z "$disc" ] && { udc_set disconnect; disc=1; }
                    case "$hs" in *state=dcp-5v*)
                        if [ -z "$rer" ] && [ -e $HVP/hvdcp_rerun ]; then
                            rer=1; sleep 1; echo 1 > $HVP/hvdcp_rerun 2>/dev/null
                            say "QC_RERUN APSD re-run requested (gadget pull-up off): $hs"
                        fi ;;
                    esac ;;
                esac
            fi
        fi
        why=""
        [ "$t" != NA ] && [ "$t" -ge ${T_ABORT:-500} ] && why="battery temp $t >= ${T_ABORT:-500}/10 C"
        [ "$t" != NA ] && [ "$t" -lt 0 ] && why="battery temp $t < 0 C"
        [ "$uv" != NA ] && [ "$uv" -gt 4420000 ] && why="battery voltage $uv > 4.42 V"
        [ "$qv" != NA ] && [ "$qv" -gt 9600000 ] && why="USBIN $qv uV > 9.6 V"
        [ "$h" != NA ] && [ "$h" != Good ] && [ "$h" != Unknown ] && why="charger health $h"
        if [ "$ua" != NA ] && [ "$ua" -lt $((-(fcc + fcc / 20 + 50000))) ]; then bi=$((bi+1)); else bi=0; fi
        [ $bi -ge 2 ] && why="battery charge current $ua uA beyond FCC $fcc + 5 % twice"
        if [ "$qi" != NA ] && [ "$qi" -gt $((QICL + QICL / 10 + 100000)) ]; then ii=$((ii+1)); else ii=0; fi
        [ $ii -ge 2 ] && why="USBIN current $qi uA beyond ICL $QICL + 10 % twice"
        case "$hs" in *state=active*)
            [ -z "$pass" ] && [ "$qv" != NA ] && [ "$qv" -ge 8400000 ] && [ "$qv" -le 9600000 ] && \
                { pass=1; say "QC_PASS HVDCP active: $(echo "$hs" | cut -d' ' -f2) usbin_uV=$qv usbin_uA=$qi bat_uA=$ua temp=$t"; } ;;
        *state=failed*)
            [ -z "$drv" ] && { drv=1; say "QC_DRIVER_ABORT (driver went back to 5 V by itself): $hs"; } ;;
        esac
        if [ -n "$why" ]; then
            echo 0 > $HVP/hvdcp_enable 2>/dev/null; sleep ${QOFF_WAIT:-3}; echo 0 > $CHG/status
            say "ABORT $why -> HVDCP off (5 V) + USB input suspended; $(snap) hv[$(v $HVP/hvdcp_status)]"; qc_restore; exit 3
        fi
        sleep ${QSLEEP:-2}; n=$((n+QSTEP))
    done
    echo 0 > $HVP/hvdcp_enable 2>/dev/null; sleep ${QOFF_WAIT:-3}
    echo 500000 > $CHG/current_max 2>/dev/null
    say "QCMON_DONE $(snap) hv[$(v $HVP/hvdcp_status)] regs[$(v $HVP/hvdcp_regs)] (HVDCP off -> 5 V, ICL 500 mA)"
    if [ -n "$pass" ]; then say "QC_RESULT PASS max usbin ${vmax} uV"
    elif [ "$wall" = 1 ]; then say "QC_RESULT FAIL wall charger seen, no QC (max usbin ${vmax} uV): send a6l-qc.log + dmesg A6L_HVDCP"
    else say "QC_RESULT NONE no wall charger seen"; fi
    qc_restore ;;
qc-status) tail -${N:-8} $QLOG; say "hvdcp_status: $(v $HVP/hvdcp_status)" ;;
chg-resume) echo 1 > $CHG/status && say "USB input resumed $(snap)" ;;
fuserows)
    cd "$D" || exit 1
    say "reading ONLY qfprom rows 38 and 65..71 (stock-read rows); if the phone resets, the last 'reading row' line names the row"
    chmod 755 ./a6l_fuserows 2>/dev/null; ./a6l_fuserows /tmp/a6l-fuserows.bin && say "FUSEROWS_DONE pull /tmp/a6l-fuserows.bin; laptop: python3 a6l_cpr_openloop.py a6l-fuserows.bin --offset 0 --stock-log stock-cpr.txt" ;;
cpr-check)
    mount -t debugfs none /sys/kernel/debug 2>/dev/null
    say "kernel $(uname -r) image=$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-image 2>/dev/null)"
    dmesg | grep -iE 'cpr|cpufreq|osm|apc|qfprom|nvmem' | tail -40 | sed 's/^/A6L_PWR_DMESG /'
    for t in /sys/kernel/debug/qcom_cpr3/thread*; do [ -e "$t" ] && sed "s|^|A6L_CPR_DBG ${t##*/}: |" $t; done
    for p in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d $p ] && say "CPUFREQ ${p##*/} cpus=$(v $p/related_cpus) cur=$(v $p/scaling_cur_freq) avail=$(v $p/scaling_available_frequencies) gov=$(v $p/scaling_governor)"
    done
    say "max thermal zone $(maxtemp) mC"; say CPR_CHECK_DONE ;;
cpr-step)
    TMAX=${TMAX:-70000}; HOLD=${HOLD:-5}
    for p in /sys/devices/system/cpu/cpufreq/policy*; do
        [ -d $p ] || continue; pn=${p##*/}; lim=$(eval echo \${PMAX${pn#policy}:-1747200})
        echo userspace > $p/scaling_governor || { say "FAIL no userspace governor on $pn"; exit 1; }
        lo=$(v $p/cpuinfo_min_freq)
        for f in $(v $p/scaling_available_frequencies); do
            [ "$f" -gt "$lim" ] && break
            echo $f > $p/scaling_setspeed
            c=$(echo "$(v $p/related_cpus)" | cut -d' ' -f1)
            ( i=0; while [ $i -lt 400000 ]; do i=$((i+1)); done ) & bp=$!
            taskset -p $((1 << c)) $bp > /dev/null 2>&1
            sleep $HOLD; kill $bp 2>/dev/null
            m=$(maxtemp); say "STEP $pn set=$f cur=$(v $p/scaling_cur_freq) maxtemp=$m"
            if [ "$m" -ge "$TMAX" ]; then echo $lo > $p/scaling_setspeed; say "ABORT thermal $m >= $TMAX -> $pn back to $lo"; exit 3; fi
        done
        echo $lo > $p/scaling_setspeed; say "$pn done -> $lo"
    done
    say CPR_STEP_DONE ;;
*) say "unknown MODE $MODE"; exit 2 ;;
esac
