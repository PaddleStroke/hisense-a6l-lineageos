#!/system/bin/sh
# A6L camera17: hi846 (rear wide/aux) round 2 (29 Sep 2026, bundle firmware/extracted/camera-20260929-hi846b = laptop
# v75/camera17). docs/hi846-20260929.md "Round 2".
# t37 ROOT CAUSE (offline, from the camera16 logs): the hi846 never started streaming. hi846_set_ctrl() leaked the '1'
# returned by pm_runtime_get_if_in_use() -> __v4l2_ctrl_handler_setup() returned 1 -> hi846_start_streaming() gave up before
# MODE_SELECT 0x0a00=1, hi846_set_stream() stopped it again and returned +1 (camss ignores ret > 0). camera17 hi846.ko fixes
# that (hi846.a6l_fix=1, default) and keeps a negative-control row (a6l_fix=0).
# ALL diagnostics are runtime params of the single loaded modules: NO rmmod, NO module swap, ever (t37: rmmod qcom_camss hung).
#   qcom_camss.a6l_c17 (CSID packet counters ON/MID/OFF, CSIPHY readback), a6l_c17_settle, a6l_c17_lpos, a6l_c17_phy
#   hi846.a6l_fix, a6l_rd (3 = dump at +20 ms and +520 ms and at stream off), a6l_rdx, a6l_mclk, a6l_stock
# ATTENDED ONLY: switches the 2.8 V camera rail (gpio51) and the MCLKs. Nothing here writes flash/eMMC.
# Camera DT = runtime overlay extra/a6l_cam_ovl.ko hi846_lanes=2 (fixed for the boot; first MODE=probe of a fresh boot).
#   MODE=check|probe|bars|live|h17   SENSOR=imx576|s5k3t1|hi846   D=bundle dir (default /tmp/camera17)
#   optional env for bars/live: HW= HH= CSIDX= WM= N= EXP= GAIN= TAG= KN="fix=0,settle=20,lpos=0x32,mclk=19200000,stock=1,c17=3"
#   MODE=h17: detached-safe, ~2 min, rows name:sensor:mode:W:H:csid:knobs; A6L_C17_BEGIN/END per row,
#     A6L_C17_SWEEP_BEGIN/END + A6L_C17_DONE to /dev/kmsg; table /tmp/cam17.tab, summary /tmp/cam17.sum.
#     If h2bars fails, VARIANT rows follow automatically (settle 0x14, lane positions, MCLK 19.2 MHz, stock 0x0076).
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/camera17}; MODE=${MODE:-probe}; SENSOR=${SENSOR:-imx576}
say() { echo "A6L_CAM $*"; }
kmsg() { echo "$*" > /dev/kmsg 2>/dev/null; }
chmod 755 $D/a6l_camcap 2>/dev/null
P=/sys/module/qcom_camss/parameters
HP=/sys/module/hi846/parameters
EP=/sys/firmware/devicetree/base/soc@0/cci@ca0c000/i2c-bus@1/camera@20/port/endpoint/data-lanes
dtlanes() { [ -f $EP ] && echo $(( $(wc -c < $EP) / 4 )) || echo 0; }
setsensor() {
case "$SENSOR" in
imx576) PHY=0; CSID=0; W=2880; H=2156; BAYER=RGGB; EXP=${EXP:-2000}; GAIN=${GAIN:-512};;
hi846)  PHY=1; CSID=0; W=${HW:-1632}; H=${HH:-1224}; BAYER=GBRG; EXP=${EXP:-2000}; GAIN=${GAIN:-64};;
s5k3t1) PHY=2; CSID=1; W=2304; H=1728; BAYER=GRBG; EXP=${EXP:-1500}; GAIN=${GAIN:-128};;
*) say "unknown SENSOR=$SENSOR"; exit 2;;
esac
[ -n "$CSIDX" ] && CSID=$CSIDX
BPL=$((W * 10 / 8))
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
  out=$(insmod $D/extra/a6l_cam_ovl.ko hi846_lanes=2 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod a6l_cam_ovl: $out";; esac
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
distinct() {  # distinct byte values in <len> bytes at <offset> of file (1 = constant filler)
  dd if=$1 bs=1 skip=$2 count=$3 2>/dev/null | od -An -v -tx1 | tr -s ' ' '\n' | grep -v '^$' | sort -u | wc -l
}
clkstate() {
  grep -q " /sys/kernel/debug " /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
  grep -E "^ *(csiphy_clk_src|camss_csiphy[0-2]_clk|camss_cphy_csid[0-3]_clk|csi[0-3]_clk_src|camss_csi[0-3]_clk|csi[0-3]phytimer_clk_src|vfe0_clk_src|mclk[0-3]_clk_src|camss_mclk[0-3]_clk) " /sys/kernel/debug/clk/clk_summary 2>/dev/null |
    sed 's/^ *//' | tr -s ' ' | cut -d' ' -f1,2,5 | sed 's/ / en=/; s/\(en=[^ ]*\) /\1 rate=/; s/$/;/' | tr '\n' ' '
}
wp() { [ -f $1 ] && echo $2 > $1 2>/dev/null; }
knobs() {  # defaults every row, then KN="k=v,k=v" overrides (runtime params only)
  wp $HP/a6l_fix 1; wp $HP/a6l_rd 3; wp $HP/a6l_stock 0; wp $HP/a6l_mclk 24000000; wp $HP/a6l_rdx 0x0a02,0x0b00,0x0f04,0x0f08,0x0f30,0x0f32,0x0918,0x0919
  wp $P/a6l_c17 3; wp $P/a6l_c17_settle 0; wp $P/a6l_c17_lpos 0; wp $P/a6l_c17_phy 1; wp $P/a6l_c17_ms 400
  for kv in $(echo "$KN" | tr ',' ' '); do k=${kv%%=*}; v=${kv#*=}
    case "$k" in
    fix|rd|stock|mclk|rdx) wp $HP/a6l_$k $v ;;
    c17) wp $P/a6l_c17 $v ;; settle|lpos|phy|ms) wp $P/a6l_c17_$k $v ;;
    -|"") ;; *) say "unknown knob $k";;
    esac
  done
  say "KNOBS fix=$(cat $HP/a6l_fix 2>/dev/null) rd=$(cat $HP/a6l_rd 2>/dev/null) stock=$(cat $HP/a6l_stock 2>/dev/null) mclk=$(cat $HP/a6l_mclk 2>/dev/null) c17=$(cat $P/a6l_c17 2>/dev/null) settle=$(cat $P/a6l_c17_settle 2>/dev/null) lpos=$(cat $P/a6l_c17_lpos 2>/dev/null) phy=$(cat $P/a6l_c17_phy 2>/dev/null)"
}

capture() {  # MODE (bars|live), SENSOR, WM, TAG, KN -> /tmp/cam-<sensor>-<mode><tag>.{raw,log,dmesg,out}
  setsensor; mknodes
  if [ "$MODE" = bars ]; then TP="-t 2"; else TP="-t 0 -e $EXP -g $GAIN"; fi
  OUT=/tmp/cam-$SENSOR-$MODE$TAG.raw; LOG=/tmp/cam-$SENSOR-$MODE$TAG.log; rm -f $OUT
  if dmesg | grep -qE "Internal error: Oops|Unable to handle kernel"; then say "PREVIOUS_OOPS_IN_DMESG: reboot first"; return 3; fi
  [ -n "$WM" ] && echo $WM > $P/a6l_wm
  knobs
  say "PARAMS a6l_wm=$(cat $P/a6l_wm 2>/dev/null) sensor=$SENSOR phy=$PHY csid=$CSID ${W}x$H mode=$MODE dt_hi846_lanes=$(dtlanes)"
  mark
  ( sleep 3; say "CLK $(clkstate)" > /tmp/cam-clk-$$.txt ) &
  t0=$(date +%s)
  timeout 60 $D/a6l_camcap -s $SENSOR -p $PHY -c $CSID -W $W -H $H $TP -n ${N:-8} -o $OUT > $LOG 2>&1; rc=$?
  t1=$(date +%s); wait
  cat /tmp/cam-clk-$$.txt 2>/dev/null; rm -f /tmp/cam-clk-$$.txt
  say "CAMCAP rc=$rc secs=$((t1-t0))"; grep -v linker $LOG | grep -E "^(video|frame|TIMEOUT|A6L_CAMCAP|fmt|missing|no |ctrl|linked)" | tail -16
  since > /tmp/cam-$SENSOR-$MODE$TAG.dmesg
  F=/tmp/cam-$SENSOR-$MODE$TAG.dmesg
  grep -E "camss|csid|csiphy|ispif|vfe|VFE|$SENSOR|imx576|s5k3t1|hi846|A6L_H846|A6L_C17|smmu|Oops|Unable to handle|A6L_SNS" $F | grep -v "^.*Modules linked" | tail -40
  grep -qE "Internal error: Oops|Unable to handle kernel" $F && say "KERNEL_OOPS_FAIL (reboot before the next camera test)"
  grep -q "VFE sof timeout" $F && say "NO_SOF (VFE sof timeout: nothing reached the VFE)"
  grep -q "call_s_stream" $F && say "S_STREAM_WARN (a subdev s_stream(1) returned non-zero)"
  grep -o "A6L_H846 START_FAIL.*\|A6L_H846 ctrl_handler_setup returned.*" $F | sed 's/^/A6L_CAM /'
  grep -o "A6L_H846 [A-Z0-9]* lanes=.*" $F | sed 's/^/A6L_CAM /' | cut -c1-400
  grep -o "A6L_H846 [A-Z0-9]* mclk=.*\|A6L_H846 MCLK set.*" $F | sed 's/^/A6L_CAM /'
  grep -o "A6L_C17 .*" $F | sed 's/^/A6L_CAM /' | cut -c1-420
  grep -q "A6L_H846 ON .* 0a00=01" $F && say "SENSOR_STREAM_ON_PASS (hi846 0x0a00=01)"
  [ "$SENSOR" = hi846 ] && ! grep -q "A6L_H846 ON .* 0a00=01" $F && say "SENSOR_STREAM_ON_FAIL (hi846 never reached 0x0a00=01)"
  pk=$(grep -o "A6L_C17 CSID[0-9] MID#3 .*tot [0-9]* (+[0-9]*)" $F | tail -1 | sed 's/.*(+//; s/)//')
  say "PKTS mid3_delta=${pk:-?}"
  nf=$(grep -c "^frame " $LOG); seqs=$(grep "^frame " $LOG | cut -d' ' -f4 | tr '\n' ' ')
  say "FRAMES dequeued=$nf seq=[$seqs]"
  b=$(grep -o "bpl [0-9]*" $LOG | head -1 | cut -d' ' -f2); [ -n "$b" ] && BPL=$b
  if [ -s $OUT ]; then
    sz=$(wc -c < $OUT); d0=$(distinct $OUT 0 $BPL); dm=$(distinct $OUT $(( (H/2)*BPL )) $BPL); dl=$(distinct $OUT $(( (H-1)*BPL )) $BPL)
    say "RAW $OUT bytes=$sz expect=$((BPL*H)) distinct_bytes line0=$d0 mid=$dm last=$dl (1 = constant filler)"
    ok=1; [ "$sz" = $((BPL*H)) ] || ok=0; [ $d0 -gt 1 ] && [ $dl -gt 1 ] || ok=0; [ $rc = 0 ] || ok=0; [ $nf -ge $(( ${N:-8} + 1 )) ] || ok=0
    [ $ok = 1 ] && say "CAPTURE_${SENSOR}_${MODE}${TAG}_PASS" || say "CAPTURE_${SENSOR}_${MODE}${TAG}_SUSPECT (check the PNG)"
    say "host: adb pull $OUT && python3 raw10_to_png.py $(basename $OUT) $W $H $BPL $BAYER $(basename $OUT .raw).png"
  else
    say "CAPTURE_${SENSOR}_${MODE}${TAG}_FAIL"
  fi
}

runrows() {  # $1 = rows
  for e in $1; do
    vn=${e%%:*}; r=${e#*:}; SENSOR=${r%%:*}; r=${r#*:}; MODE=${r%%:*}; r=${r#*:}; w=${r%%:*}; r=${r#*:}; h=${r%%:*}; r=${r#*:}; c=${r%%:*}; KN=${r#*:}
    HW=; HH=; CSIDX=; [ "$w" != 0 ] && HW=$w; [ "$h" != 0 ] && HH=$h; [ "$SENSOR" = hi846 ] && CSIDX=$c
    WM=6; TAG=-c17-$vn
    kmsg "A6L_C17_BEGIN $vn sensor=$SENSOR mode=$MODE size=${w}x$h csid=$c knobs=$KN"; say "C17_BEGIN $vn"
    capture > /tmp/cam-$SENSOR-$MODE$TAG.out 2>&1; rc=$?
    O=/tmp/cam-$SENSOR-$MODE$TAG.out
    cap=$(grep -oE "CAPTURE_[A-Za-z0-9_-]*_(PASS|FAIL|SUSPECT)" $O | sed 's/.*_//'); fr=$(grep -o "FRAMES .*" $O | cut -c8-60)
    son=$(grep -q SENSOR_STREAM_ON_PASS $O && echo on || { [ "$SENSOR" = hi846 ] && echo OFF || echo -; })
    pk=$(grep -o "PKTS mid3_delta=.*" $O | cut -d= -f2)
    [ "$SENSOR" = hi846 ] && [ "$cap" = PASS ] && [ "$KN" = - ] && hpass=$((hpass+1))
    printf "%-8s %-7s %-5s %-10s %-4s %-22s %-8s %-4s %-8s %s\n" $vn $SENSOR $MODE ${w}x$h $c "$KN" "${cap:-?}" $son "${pk:-?}" "$fr" >> $TAB
    grep -E "A6L_CAM (CAPTURE|FRAMES|RAW|CLK |KERNEL_OOPS|NO_SOF|S_STREAM_WARN|SENSOR_STREAM|CAMCAP|KNOBS|PKTS|A6L_H846|A6L_C17)" $O | sed "s/^/$vn /" >> $SUM
    kmsg "A6L_C17_END $vn capture=${cap:-?} sensor=$son pkts=${pk:-?} $fr"; say "C17_END $vn ${cap:-?} sensor=$son pkts=${pk:-?}"
    [ $rc = 3 ] && return 3; grep -q KERNEL_OOPS_FAIL $O && { kmsg "A6L_C17_ABORT oops"; return 3; }; sleep 1
  done
  return 0
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
  L=$(dtlanes); hl=$(dmesg | grep -o "hi846.*using [0-9] mipi lanes" | tail -1 | grep -o "using [0-9]" | cut -d' ' -f2)
  say "HI846_LANES dt=$L driver=${hl:-?} overlay_param=$(cat /sys/module/a6l_cam_ovl/parameters/hi846_lanes 2>/dev/null)"
  [ "$L" = 2 ] && [ "$hl" = 2 ] && say "HI846_LANES_PASS 2" || say "HI846_LANES_FAIL (overlay already applied this boot with 4 lanes? reboot)"
  [ -f $P/a6l_wm ] && [ -f $P/a6l_c17 ] && say "CAMSS_DIAG17_PASS a6l_wm=$(cat $P/a6l_wm) a6l_c17=$(cat $P/a6l_c17)" || say "CAMSS_DIAG17_FAIL (another qcom-camss is loaded this boot: reboot, then MODE=probe with camera17)"
  [ -f $HP/a6l_fix ] && say "HI846_CAMFIX17_PASS a6l_fix=$(cat $HP/a6l_fix)" || say "HI846_CAMFIX17_FAIL (older hi846.ko loaded this boot: reboot)"
  say "SRCVERSION camss=$(cat /sys/module/qcom_camss/srcversion 2>/dev/null) hi846=$(cat /sys/module/hi846/srcversion 2>/dev/null) imx576=$(cat /sys/module/imx576_a6l/srcversion 2>/dev/null)"
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  say "CLK_AT_PROBE $(clkstate)"
  $D/a6l_camcap -l | grep -q "msm_ispif0" && say ISPIF_IN_GRAPH_PASS || say ISPIF_IN_GRAPH_FAIL
  say PROBE_DONE
  ;;
bars|live) KN=${KN:--}; capture ;;
h17)
  SUM=/tmp/cam17.sum; TAB=/tmp/cam17.tab; : > $SUM; hpass=0
  [ -f $P/a6l_c17 ] && [ -f $HP/a6l_fix ] || { say "H17_ABORT camera17 modules not loaded (reboot, MODE=probe)"; exit 3; }
  L=$(dtlanes); [ "$L" = 2 ] || { say "H17_ABORT DT hi846 lanes=$L (want 2: reboot, MODE=probe)"; exit 3; }
  BASE="ibars:imx576:bars:0:0:0:- h2bars:hi846:bars:1632:1224:0:- h2live:hi846:live:1632:1224:0:- h2b720:hi846:bars:1280:720:0:- h2b640:hi846:bars:640:480:0:- h2bc1:hi846:bars:1632:1224:1:- h2nofix:hi846:bars:1632:1224:0:fix=0 sbars:s5k3t1:bars:0:0:1:-"
  VARI="h2st20:hi846:bars:1632:1224:0:settle=20 h2lp32:hi846:bars:1632:1224:0:lpos=0x32 h2lp01:hi846:bars:1632:1224:0:lpos=0x01 h2m19:hi846:bars:640:480:0:mclk=19200000 h2stk:hi846:bars:1632:1224:0:stock=1 sbars2:s5k3t1:bars:0:0:1:-"
  kmsg "A6L_C17_SWEEP_BEGIN dt_hi846_lanes=$L"
  printf "%-8s %-7s %-5s %-10s %-4s %-22s %-8s %-4s %-8s %s\n" name sensor mode size csid knobs capture sens pkts frames > $TAB
  runrows "${VARS:-$BASE}"; rr=$?
  if [ $rr = 0 ] && [ -z "$VARS" ] && ! grep -q "^h2bars .*PASS" $TAB && [ "${VARIANTS:-1}" = 1 ]; then
    kmsg "A6L_C17_VARIANTS h2bars failed"; say "h2bars failed: VARIANT rows"; runrows "$VARI"
  fi
  KN=-; knobs > /dev/null; echo 6 > $P/a6l_wm
  say C17_TABLE; cat $TAB; kmsg "A6L_C17_SWEEP_END dt_hi846_lanes=$L hi846_pass=$hpass"
  kmsg "A6L_C17_DONE"; say "C17_DONE (pull /tmp/c17.tar: see docs/hi846-20260929.md round 2)"
  ;;
focus) mknodes; $D/a6l_camcap -s gt9769 -f ${STEPS:-0,256,512,768,1023,512,0} && say FOCUS_DONE || say FOCUS_FAIL ;;
*) say "unknown MODE=$MODE (camera17 has no hdiag/off: no rmmod ever)"; exit 2;;
esac
