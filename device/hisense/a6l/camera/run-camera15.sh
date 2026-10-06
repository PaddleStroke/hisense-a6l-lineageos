#!/system/bin/sh
# A6L camera CONFIRM test, camera15 (29 Sep 2026, bundle firmware/extracted/camera-20260929-rom = laptop v75/camera15).
# Confirms the clean ROM camera modules: qcom-camss rom1 (series camss-sdm660-rom1.patch = camfix5 functional fixes + the
# stock CSIPHY digital clocks cphy_csidK 200 MHz / csiK 310 MHz, NO diagnostics, one knob qcom_camss.a6l_wm, default 6)
# and the ROM sensor/CCI/VCM builds (camfix2 = what tools/stage-rom-v2-prebuilts.sh stages). docs/camera-rom-20260929.md
# ATTENDED ONLY: switches the 2.8 V camera rail (gpio51) and the MCLKs. Nothing here writes flash/eMMC.
# Camera DT = runtime overlay extra/a6l_cam_ovl.ko (byte-identical dtbo to the ROM a6l-camera-v75.dtso merge).
#   MODE=check|probe|bars|live|confirm|hi846diag|focus|off   SENSOR=imx576|s5k3t1|hi846   D=bundle dir (default /tmp/camera15)
#   optional env: WM=<a6l_wm> (6 default fixed+copy, 3 zero-copy stock-like, 0 upstream) N=<frames skipped, default 8>
#                 EXP= GAIN= (live) TAG= (file suffix)
#   MODE=confirm: detached-safe, < 3 min, rows (name:sensor:mode:wm):
#     ibars:imx576:bars:6 ilive:imx576:live:6 sbars:s5k3t1:bars:6 slive:s5k3t1:live:6 hbars:hi846:bars:6
#     ibars3:imx576:bars:3 sbars3:s5k3t1:bars:3 ibars0:imx576:bars:0      (VARS="..." overrides)
#     every row writes A6L_C15_BEGIN/END to /dev/kmsg; table /tmp/cam15.tab, summary /tmp/cam15.sum
#   MODE=hi846diag (OPTIONAL, LAST, only if hbars FAILs): rmmod the rom1 camss, insmod the camfix12 diagnostic camss
#     (extra/qcom-camss-camfix12.ko) and capture hi846 bars with CSID packet/ECC + CSIPHY clock diagnostics. Reboot after it.
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/camera15}; MODE=${MODE:-probe}; SENSOR=${SENSOR:-imx576}
say() { echo "A6L_CAM $*"; }
kmsg() { echo "$*" > /dev/kmsg 2>/dev/null; }
chmod 755 $D/a6l_camcap 2>/dev/null
P=/sys/module/qcom_camss/parameters
setsensor() {
case "$SENSOR" in
imx576) PHY=0; CSID=0; W=2880; H=2156; BPL=3600; BAYER=RGGB; EXP=${EXP:-2000}; GAIN=${GAIN:-512};;
hi846)  PHY=1; CSID=0; W=1632; H=1224; BPL=2040; BAYER=GBRG; EXP=${EXP:-2000}; GAIN=${GAIN:-64};;
s5k3t1) PHY=2; CSID=1; W=2304; H=1728; BPL=2880; BAYER=GRBG; EXP=${EXP:-1500}; GAIN=${GAIN:-128};;
*) say "unknown SENSOR=$SENSOR"; exit 2;;
esac
}
setsensor

check() {   # toybox sha256sum has no --ignore-missing: verify line by line
  bad=0; n=0
  while read sum name; do
    [ -n "$name" ] || continue
    if [ ! -f "$D/$name" ]; then say "SHA_MISSING $name"; bad=1; continue; fi
    got=$(sha256sum "$D/$name" | cut -d' ' -f1)
    if [ "$got" = "$sum" ]; then n=$((n+1)); else say "SHA_BAD $name"; bad=1; fi
  done < $D/SHA256SUMS
  [ $bad = 0 ] && say "SHA_PASS $n files" || say "SHA_FAIL"
}
mknodes() {  # recovery has no ueventd: create /dev nodes from sysfs
  for p in /sys/class/video4linux/* /sys/bus/media/devices/*; do
    [ -f $p/dev ] || continue
    n=${p##*/}; mm=$(cat $p/dev)
    [ -c /dev/$n ] || mknod /dev/$n c ${mm%%:*} ${mm##*:}
  done
}
load() {
  out=$(insmod $D/extra/a6l_cam_ovl.ko 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod a6l_cam_ovl: $out";; esac
  for m in $(cat $D/load-order.txt); do
    out=$(insmod $D/$m 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod $m: $out";; esac
  done
  sleep 3; mknodes
}
MKN=0
mark() { MKN=$((MKN+1)); MK="A6L_MARK_$$_$(date +%s)_$MKN"; echo "$MK" > /dev/kmsg; echo "$MK" > /tmp/cam-dmesg-mark; }
since() {
  m=$(cat /tmp/cam-dmesg-mark 2>/dev/null); dmesg > /tmp/cam-dmesg-now
  n=$(grep -n "^\[[ 0-9.]*\] $m\$" /tmp/cam-dmesg-now | tail -n 1 | cut -d: -f1)
  if [ -n "$n" ]; then tail -n +$((n+1)) /tmp/cam-dmesg-now; else cat /tmp/cam-dmesg-now; fi
}
distinct() {  # distinct byte values in <len> bytes at <offset> of file (1 = constant filler, colour bars/real image >> 1)
  dd if=$1 bs=1 skip=$2 count=$3 2>/dev/null | od -An -v -tx1 | tr -s ' ' '\n' | grep -v '^$' | sort -u | wc -l
}
clkstate() {  # the fix itself: csiphy_clk_src / camss_csiphyK / cphy_csidK / csiK rates + enables while streaming
  grep -q " /sys/kernel/debug " /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
  grep -E "^ *(csiphy_clk_src|camss_csiphy[0-2]_clk|camss_cphy_csid[0-3]_clk|csi[0-3]_clk_src|camss_csi[0-3]_clk|csi[0-3]phytimer_clk_src|vfe0_clk_src) " /sys/kernel/debug/clk/clk_summary 2>/dev/null |
    awk '{printf "%s en=%s prep=%s rate=%s; ", $1, $2, $3, $5}'
}

capture() {  # MODE (bars|live), SENSOR, WM, TAG -> /tmp/cam-<sensor>-<mode><tag>.{raw,log,dmesg,out}
  setsensor; mknodes
  if [ "$MODE" = bars ]; then TP="-t 2"; else TP="-t 0 -e $EXP -g $GAIN"; fi
  OUT=/tmp/cam-$SENSOR-$MODE$TAG.raw; LOG=/tmp/cam-$SENSOR-$MODE$TAG.log; rm -f $OUT
  if dmesg | grep -qE "Internal error: Oops|Unable to handle kernel"; then say "PREVIOUS_OOPS_IN_DMESG: reboot first"; return 3; fi
  [ -n "$WM" ] && echo $WM > $P/a6l_wm
  say "PARAMS a6l_wm=$(cat $P/a6l_wm 2>/dev/null) sensor=$SENSOR phy=$PHY csid=$CSID ${W}x$H mode=$MODE"
  mark
  ( sleep 3; say "CLK $(clkstate)" > /tmp/cam-clk-$$.txt ) &
  t0=$(date +%s)
  timeout 60 $D/a6l_camcap -s $SENSOR -p $PHY -c $CSID -W $W -H $H $TP -n ${N:-8} -o $OUT > $LOG 2>&1; rc=$?
  t1=$(date +%s); wait
  cat /tmp/cam-clk-$$.txt 2>/dev/null; rm -f /tmp/cam-clk-$$.txt
  say "CAMCAP rc=$rc secs=$((t1-t0))"; grep -v linker $LOG | grep -E "^(video|frame|TIMEOUT|A6L_CAMCAP|fmt|missing|no )" | tail -14
  since > /tmp/cam-$SENSOR-$MODE$TAG.dmesg
  F=/tmp/cam-$SENSOR-$MODE$TAG.dmesg
  grep -E "camss|csid|csiphy|ispif|vfe|VFE|$SENSOR|imx576|s5k3t1|hi846|smmu|Oops|Unable to handle|A6L_SNS" $F | tail -40
  grep -qE "Internal error: Oops|Unable to handle kernel" $F && say "KERNEL_OOPS_FAIL (reboot before the next camera test)"
  grep -q "CSIPHY[0-9]: no clock\|set_rate(.*) failed\|enable failed" $F && say "CLOCKFIX_WARN $(grep -o 'CSIPHY[0-9]: .*' $F | head -2 | tr '\n' ' ')"
  since | grep -q "SENSOR_STREAMING" && say "SENSOR_TX_PASS (frame counter moving)"
  since | grep -q "SENSOR_NOT_COUNTING" && say "SENSOR_TX_FAIL (frame counter static)"
  nf=$(grep -c "^frame " $LOG); seqs=$(grep "^frame " $LOG | awk '{print $4}' | tr '\n' ' ')
  f1=$(grep "^frame " $LOG | sed -n 2p | awk '{print $NF}'); fl=$(grep "^frame " $LOG | tail -1 | awk '{print $NF}')
  s1=$(grep "^frame " $LOG | sed -n 2p | awk '{print $4}'); sl=$(grep "^frame " $LOG | tail -1 | awk '{print $4}')
  fps=$(awk -v a="$f1" -v b="$fl" -v n="$nf" -v s="$s1" -v t="$sl" 'BEGIN{ if (b>a) printf "dq_fps=%.1f vfe_fps=%.1f", (n-2)/(b-a), (t-s)/(b-a); else print "dq_fps=- vfe_fps=-" }')
  say "FRAMES dequeued=$nf seq=[$seqs] $fps"
  b=$(grep -o "bpl [0-9]*" $LOG | head -1 | cut -d' ' -f2); [ -n "$b" ] && BPL=$b
  if [ -s $OUT ]; then
    sz=$(wc -c < $OUT); d0=$(distinct $OUT 0 $BPL); dm=$(distinct $OUT $(( (H/2)*BPL )) $BPL); dl=$(distinct $OUT $(( (H-1)*BPL )) $BPL)
    say "RAW $OUT bytes=$sz expect=$((BPL*H)) distinct_bytes line0=$d0 mid=$dm last=$dl (1 = constant filler; bars >= 8)"
    ok=1; [ "$sz" = $((BPL*H)) ] || ok=0; [ $d0 -gt 4 ] && [ $dl -gt 4 ] || ok=0; [ $rc = 0 ] || ok=0
    [ $ok = 1 ] && say "CAPTURE_${SENSOR}_${MODE}${TAG}_PASS" || say "CAPTURE_${SENSOR}_${MODE}${TAG}_SUSPECT (check the PNG)"
    say "host: adb pull $OUT && python3 raw10_to_png.py $(basename $OUT) $W $H $BPL $BAYER $(basename $OUT .raw).png"
  else
    say "CAPTURE_${SENSOR}_${MODE}${TAG}_FAIL"
  fi
}

case "$MODE" in
check) check ;;
load|probe)
  check; load
  dmesg | grep -iE "A6L_CAM_OVL|cci|camss|csiphy|ispif|imx576|s5k3t1|hi846|gt9769|chip id|supply" | tail -30
  dmesg | grep -q "Sony IMX576 chip id 0x576" && say IMX576_PROBE_PASS || say IMX576_PROBE_FAIL
  dmesg | grep -q "Samsung S5K3T1 chip id 0x3141" && say S5K3T1_PROBE_PASS || say S5K3T1_PROBE_FAIL
  dmesg | grep -q "hi846.*chip id 08 46" && say HI846_PROBE_PASS || say HI846_PROBE_FAIL
  dmesg | grep -q "GT9769 VCM initialised" && say GT9769_PROBE_PASS || say GT9769_PROBE_FAIL
  # the ROM module: a6l_wm only, none of the camfix6-12 diagnostic knobs
  if [ -f $P/a6l_wm ] && [ ! -f $P/a6l_v12 ] && [ ! -f $P/a6l_dbg ] && [ ! -f $P/a6l_vfe_min ]; then say "CAMSS_ROM1_PASS a6l_wm=$(cat $P/a6l_wm)"
  else say "CAMSS_ROM1_FAIL (another qcom-camss is loaded: reboot, then MODE=probe with camera15)"; fi
  [ -f /sys/module/imx576_a6l/parameters/a6l_regs ] && say "SENSOR_ROM_FAIL (camfix11 sensor modules loaded: reboot)" || say SENSOR_ROM_PASS
  say "SRCVERSION camss=$(cat /sys/module/qcom_camss/srcversion 2>/dev/null) imx576=$(cat /sys/module/imx576_a6l/srcversion 2>/dev/null) s5k3t1=$(cat /sys/module/s5k3t1/srcversion 2>/dev/null)"
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  say "CLK_AT_PROBE $(clkstate)"
  $D/a6l_camcap -l | grep -q "msm_ispif0" && say ISPIF_IN_GRAPH_PASS || say ISPIF_IN_GRAPH_FAIL
  say PROBE_DONE
  ;;
bars|live) capture ;;
confirm)
  SUM=/tmp/cam15.sum; TAB=/tmp/cam15.tab; : > $SUM
  [ -f $P/a6l_wm ] && [ ! -f $P/a6l_v12 ] || { say "CONFIRM_ABORT the rom1 qcom-camss is not loaded (reboot, MODE=probe)"; exit 3; }
  DEFV="ibars:imx576:bars:6 ilive:imx576:live:6 sbars:s5k3t1:bars:6 slive:s5k3t1:live:6 hbars:hi846:bars:6 ibars3:imx576:bars:3 sbars3:s5k3t1:bars:3 ibars0:imx576:bars:0"
  printf "%-8s %-7s %-5s %-3s %-9s %-40s %s\n" name sensor mode wm capture frames raw > $TAB
  for e in ${VARS:-$DEFV}; do
    vn=${e%%:*}; r=${e#*:}; SENSOR=${r%%:*}; r=${r#*:}; MODE=${r%%:*}; WM=${r#*:}
    TAG=-c15-$vn
    kmsg "A6L_C15_BEGIN $vn sensor=$SENSOR mode=$MODE wm=$WM"; say "C15_BEGIN $vn"
    capture > /tmp/cam-$SENSOR-$MODE$TAG.out 2>&1; rc=$?
    O=/tmp/cam-$SENSOR-$MODE$TAG.out
    cap=$(grep -o "CAPTURE_[A-Za-z0-9_-]*_\(PASS\|FAIL\|SUSPECT\)" $O | sed 's/.*_//'); fr=$(grep -o "FRAMES .*" $O | cut -c8-60); raw=$(grep -o "distinct_bytes.*(1 =" $O | sed 's/distinct_bytes //; s/ (1 =//')
    printf "%-8s %-7s %-5s %-3s %-9s %-40s %s\n" $vn $SENSOR $MODE $WM "${cap:-?}" "$fr" "$raw" >> $TAB
    grep -E "A6L_CAM (CAPTURE|FRAMES|RAW|CLK |CLOCKFIX|KERNEL_OOPS|SENSOR_TX|CAMCAP)" $O | sed "s/^/$vn /" >> $SUM
    kmsg "A6L_C15_END $vn capture=${cap:-?} $fr raw $raw"; say "C15_END $vn ${cap:-?}"
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && { kmsg "A6L_C15_ABORT oops"; break; }; sleep 1
  done
  echo 6 > $P/a6l_wm
  say C15_TABLE; cat $TAB; kmsg "A6L_C15_DONE"; say "C15_DONE (pull /tmp/cam-*-c15-*.raw/.out/.dmesg + /tmp/cam15.*)"
  ;;
hi846diag)
  # camfix12 diagnostic camss for the hi846 only: CSID STATS/ECC halves, headers, CSIPHY clock tree (A6L_V12/V11 lines)
  SENSOR=hi846; setsensor
  for m in imx576_a6l s5k3t1 hi846 gt9769; do rmmod $m 2>/dev/null; done
  rmmod qcom_camss 2>&1 | head -2; [ -d /sys/module/qcom_camss ] && { say "HI846DIAG_ABORT qcom_camss still loaded (in use): reboot, probe, then hi846diag first"; exit 3; }
  insmod $D/extra/qcom-camss-camfix12.ko 2>&1 | head -2
  for m in imx576_a6l.ko s5k3t1.ko hi846.ko gt9769.ko; do insmod $D/$m 2>&1 | grep -v "File exists" | head -1; done
  sleep 3; mknodes
  [ -f $P/a6l_v12 ] || { say "HI846DIAG_ABORT camfix12 camss not loaded"; exit 3; }
  echo 6 > $P/a6l_wm; echo 0x13 > $P/a6l_v12; echo 1 > $P/a6l_v11; echo 1 > $P/a6l_v10; echo 128 > $P/a6l_v6
  for s in ${HSETTLE:-0 14}; do
    echo $s > $P/a6l_v11_settle; MODE=bars; TAG=-c15-hdiag-st$s; capture > /tmp/cam-hi846-bars$TAG.out 2>&1
    grep -E "A6L_CAM (CAPTURE|FRAMES|RAW|CAMCAP)" /tmp/cam-hi846-bars$TAG.out
    grep -o "A6L_V1[0-2] \(SUM\|HDR long\|CSIPHY1 CLK ON\|CSIPHY1 stock\|CSID0 v11\|CSID0 rail\|P sof#[1-4]\).*" /tmp/cam-hi846-bars$TAG.dmesg | head -14 | sed 's/^/A6L_CAM HDIAG /'
    grep -o "A6L_CSIPHY1 link_freq.*\|A6L_VFE0_FRAMES.*\|A6L_SNS_[A-Z]*.*" /tmp/cam-hi846-bars$TAG.dmesg | tail -4 | sed 's/^/A6L_CAM HDIAG /'
  done
  say "HI846DIAG_DONE (camfix12 camss now loaded: REBOOT before any ROM-module test)"
  ;;
focus) mknodes; $D/a6l_camcap -s gt9769 -f ${STEPS:-0,256,512,768,1023,512,0} && say FOCUS_DONE || say FOCUS_FAIL ;;
off) for m in imx576_a6l s5k3t1 hi846 gt9769; do rmmod $m 2>/dev/null; done; say "OFF_DONE sensors removed" ;;
*) say "unknown MODE=$MODE"; exit 2;;
esac
