#!/system/bin/sh
# A6L camera16: hi846 (rear wide/aux, SK Hynix Hi-846) 2-lane test (29 Sep 2026, bundle firmware/extracted/camera-20260929-hi846
# = laptop v75/camera16). docs/hi846-20260929.md.
# Finding: stock runs the hi846 on 2 MIPI data lanes (libmmcamera_hi846_hmct.so csi_params lane_cnt=2 settle=0x14; its
# init+res0 tables = mainline hi846_init_2lane). Our DT/overlay and the camera15 test used 4 lanes -> no SOF (t36 hbars).
# ATTENDED ONLY: switches the 2.8 V camera rail (gpio51) and the MCLKs. Nothing here writes flash/eMMC.
# Modules: qcom-camss = rom1 (unchanged, ROM), imx576/s5k3t1/gt9769/cci = ROM builds, hi846 = ROM source + knob a6l_rd.
# Camera DT = runtime overlay extra/a6l_cam_ovl.ko hi846_lanes=$HL (HL=2 default here; HL=4 = the camera15/ROM dtbo).
#   The DT lane count is fixed for the whole boot (the first MODE=probe of the boot applies it). Fresh boot per HL.
#   MODE=check|probe|bars|live|h16|hdiag|off   SENSOR=imx576|s5k3t1|hi846   D=bundle dir (default /tmp/camera16)
#   optional env: HL=2|4 (probe only)  HW=/HH= (hi846 size: 1632x1224 default, 1280x720, 640x480 [2-lane only])
#                 CSIDX= (override the CSID, e.g. 1)  WM= N= EXP= GAIN= TAG=
#   MODE=h16: detached-safe, < 2 min (+ ~30 s AUTODIAG), rows name:sensor:mode:W:H:csid; every row writes A6L_C16_BEGIN/END
#     and the sweep A6L_C16_SWEEP_BEGIN/END to /dev/kmsg; table /tmp/cam16.tab, summary /tmp/cam16.sum.
#     AUTODIAG=1 (default): if NO hi846 row passes, finish with hdiag (camfix12 diagnostic camss) -> REBOOT afterwards.
#   MODE=hdiag: rmmod the rom1 camss, insmod extra/qcom-camss-camfix12.ko, hi846 bars with CSID packet/ECC/CSIPHY clock
#     diagnostics at settle 0 (formula) and 14 (HSETTLE="0 14"). Reboot after it.
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/camera16}; MODE=${MODE:-probe}; SENSOR=${SENSOR:-imx576}
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
  out=$(insmod $D/extra/a6l_cam_ovl.ko hi846_lanes=${HL:-2} 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod a6l_cam_ovl: $out";; esac
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
clkstate() {  # no awk in toybox (camera15 printed an empty CLK line): sed/tr/cut only
  grep -q " /sys/kernel/debug " /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
  grep -E "^ *(csiphy_clk_src|camss_csiphy[0-2]_clk|camss_cphy_csid[0-3]_clk|csi[0-3]_clk_src|camss_csi[0-3]_clk|csi[0-3]phytimer_clk_src|vfe0_clk_src) " /sys/kernel/debug/clk/clk_summary 2>/dev/null |
    sed 's/^ *//' | tr -s ' ' | cut -d' ' -f1,2,5 | sed 's/ / en=/; s/\(en=[^ ]*\) /\1 rate=/; s/$/;/' | tr '\n' ' '
}

capture() {  # MODE (bars|live), SENSOR, WM, TAG -> /tmp/cam-<sensor>-<mode><tag>.{raw,log,dmesg,out}
  setsensor; mknodes
  if [ "$MODE" = bars ]; then TP="-t 2"; else TP="-t 0 -e $EXP -g $GAIN"; fi
  OUT=/tmp/cam-$SENSOR-$MODE$TAG.raw; LOG=/tmp/cam-$SENSOR-$MODE$TAG.log; rm -f $OUT
  if dmesg | grep -qE "Internal error: Oops|Unable to handle kernel"; then say "PREVIOUS_OOPS_IN_DMESG: reboot first"; return 3; fi
  [ -n "$WM" ] && echo $WM > $P/a6l_wm
  [ -f $HP/a6l_rd ] && echo 1 > $HP/a6l_rd
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
  grep -E "camss|csid|csiphy|ispif|vfe|VFE|$SENSOR|imx576|s5k3t1|hi846|A6L_H846|smmu|Oops|Unable to handle|A6L_SNS" $F | grep -v "^.*Modules linked" | tail -30
  grep -qE "Internal error: Oops|Unable to handle kernel" $F && say "KERNEL_OOPS_FAIL (reboot before the next camera test)"
  grep -q "VFE sof timeout" $F && say "NO_SOF (VFE sof timeout: nothing reached the VFE)"
  grep -o "A6L_H846 [A-Z]* lanes=[0-9] mode=[0-9x]* link_freq=[0-9]*" $F | sed 's/^/A6L_CAM /'
  grep -q "A6L_H846 ON .* 0a00=01" $F && say "SENSOR_STREAM_ON_PASS (hi846 0x0a00=01)"
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

hdiag() {
  # camfix12 diagnostic camss for the hi846 only: CSID STATS/ECC halves, headers, CSIPHY clock tree (A6L_V12/V11 lines)
  SENSOR=hi846; HW=; HH=; CSIDX=; setsensor
  kmsg "A6L_C16_HDIAG_BEGIN dt_hi846_lanes=$(dtlanes)"
  for m in imx576_a6l s5k3t1 hi846 gt9769; do rmmod $m 2>/dev/null; done
  rmmod qcom_camss 2>&1 | head -2; [ -d /sys/module/qcom_camss ] && { say "HDIAG_ABORT qcom_camss still loaded (in use): reboot, probe, then hdiag"; kmsg "A6L_C16_HDIAG_ABORT"; return 3; }
  insmod $D/extra/qcom-camss-camfix12.ko 2>&1 | head -2
  for m in imx576_a6l.ko s5k3t1.ko hi846.ko gt9769.ko; do insmod $D/$m 2>&1 | grep -v "File exists" | head -1; done
  sleep 3; mknodes
  [ -f $P/a6l_v12 ] || { say "HDIAG_ABORT camfix12 camss not loaded"; kmsg "A6L_C16_HDIAG_ABORT"; return 3; }
  echo 6 > $P/a6l_wm; echo 0x13 > $P/a6l_v12; echo 1 > $P/a6l_v11; echo 1 > $P/a6l_v10; echo 128 > $P/a6l_v6
  for s in ${HSETTLE:-0 14}; do
    echo $s > $P/a6l_v11_settle; MODE=bars; WM=6; TAG=-c16-hdiag-st$s
    kmsg "A6L_C16_BEGIN hdiag-st$s"
    capture > /tmp/cam-hi846-bars$TAG.out 2>&1
    grep -E "A6L_CAM (CAPTURE|FRAMES|RAW|CAMCAP|NO_SOF|SENSOR_STREAM)" /tmp/cam-hi846-bars$TAG.out
    grep -oE "A6L_V1[0-2] (SUM|HDR long|CSIPHY1 CLK ON|CSIPHY1 stock|CSID0 v11|CSID0 rail|P sof#[1-4]).*" /tmp/cam-hi846-bars$TAG.dmesg | head -14 | sed 's/^/A6L_CAM HDIAG /'
    grep -oE "(A6L_CSIPHY1 link_freq|A6L_VFE0_FRAMES|A6L_SNS_[A-Z]*).*" /tmp/cam-hi846-bars$TAG.dmesg | tail -4 | sed 's/^/A6L_CAM HDIAG /'
    kmsg "A6L_C16_END hdiag-st$s $(grep -o 'FRAMES dequeued=[0-9]*' /tmp/cam-hi846-bars$TAG.out)"
  done
  kmsg "A6L_C16_HDIAG_DONE"
  say "HDIAG_DONE (camfix12 camss now loaded: REBOOT before any ROM-module test)"
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
  [ "$L" = "${HL:-2}" ] && [ "$hl" = "${HL:-2}" ] && say "HI846_LANES_PASS ${HL:-2}" || say "HI846_LANES_FAIL (wanted ${HL:-2}: overlay already applied this boot with another value? reboot)"
  if [ -f $P/a6l_wm ] && [ ! -f $P/a6l_v12 ] && [ ! -f $P/a6l_dbg ] && [ ! -f $P/a6l_vfe_min ]; then say "CAMSS_ROM1_PASS a6l_wm=$(cat $P/a6l_wm)"
  else say "CAMSS_ROM1_FAIL (another qcom-camss is loaded: reboot, then MODE=probe with camera16)"; fi
  [ -f $HP/a6l_rd ] && say "HI846_CAMFIX16_PASS" || say "HI846_CAMFIX16_FAIL (older hi846.ko loaded: reboot)"
  say "SRCVERSION camss=$(cat /sys/module/qcom_camss/srcversion 2>/dev/null) hi846=$(cat /sys/module/hi846/srcversion 2>/dev/null) imx576=$(cat /sys/module/imx576_a6l/srcversion 2>/dev/null)"
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  say "CLK_AT_PROBE $(clkstate)"
  $D/a6l_camcap -l | grep -q "msm_ispif0" && say ISPIF_IN_GRAPH_PASS || say ISPIF_IN_GRAPH_FAIL
  say PROBE_DONE
  ;;
bars|live) capture ;;
h16)
  SUM=/tmp/cam16.sum; TAB=/tmp/cam16.tab; : > $SUM
  [ -f $P/a6l_wm ] && [ ! -f $P/a6l_v12 ] || { say "H16_ABORT the rom1 qcom-camss is not loaded (reboot, MODE=probe)"; exit 3; }
  L=$(dtlanes)
  if [ "$L" = 2 ]; then
    DEFV="ibars:imx576:bars:0:0:0 h2bars:hi846:bars:1632:1224:0 h2b720:hi846:bars:1280:720:0 h2b640:hi846:bars:640:480:0 h2bc1:hi846:bars:1632:1224:1 h2live:hi846:live:1632:1224:0 sbars:s5k3t1:bars:0:0:1"
  else
    DEFV="ibars:imx576:bars:0:0:0 h4bars:hi846:bars:1632:1224:0 h4b720:hi846:bars:1280:720:0 h4bc1:hi846:bars:1632:1224:1 sbars:s5k3t1:bars:0:0:1"
  fi
  kmsg "A6L_C16_SWEEP_BEGIN dt_hi846_lanes=$L rows=$(echo ${VARS:-$DEFV} | wc -w)"
  printf "%-8s %-7s %-5s %-10s %-4s %-8s %-40s %s\n" name sensor mode size csid capture frames raw > $TAB
  hpass=0
  for e in ${VARS:-$DEFV}; do
    vn=${e%%:*}; r=${e#*:}; SENSOR=${r%%:*}; r=${r#*:}; MODE=${r%%:*}; r=${r#*:}; w=${r%%:*}; r=${r#*:}; h=${r%%:*}; c=${r#*:}
    HW=; HH=; CSIDX=; [ "$w" != 0 ] && HW=$w; [ "$h" != 0 ] && HH=$h; [ "$SENSOR" = hi846 ] && CSIDX=$c
    WM=6; TAG=-c16-$vn
    kmsg "A6L_C16_BEGIN $vn sensor=$SENSOR mode=$MODE size=${w}x$h csid=$c"; say "C16_BEGIN $vn"
    capture > /tmp/cam-$SENSOR-$MODE$TAG.out 2>&1; rc=$?
    O=/tmp/cam-$SENSOR-$MODE$TAG.out
    cap=$(grep -oE "CAPTURE_[A-Za-z0-9_-]*_(PASS|FAIL|SUSPECT)" $O | sed 's/.*_//'); fr=$(grep -o "FRAMES .*" $O | cut -c8-60); raw=$(grep -o "distinct_bytes.*(1 =" $O | sed 's/distinct_bytes //; s/ (1 =//')
    [ "$SENSOR" = hi846 ] && [ "$cap" = PASS ] && hpass=$((hpass+1))
    printf "%-8s %-7s %-5s %-10s %-4s %-8s %-40s %s\n" $vn $SENSOR $MODE ${w}x$h $c "${cap:-?}" "$fr" "$raw" >> $TAB
    grep -E "A6L_CAM (CAPTURE|FRAMES|RAW|CLK |KERNEL_OOPS|NO_SOF|SENSOR_STREAM|CAMCAP|A6L_H846)" $O | sed "s/^/$vn /" >> $SUM
    kmsg "A6L_C16_END $vn capture=${cap:-?} $fr raw $raw"; say "C16_END $vn ${cap:-?}"
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && { kmsg "A6L_C16_ABORT oops"; break; }; sleep 1
  done
  echo 6 > $P/a6l_wm
  say C16_TABLE; cat $TAB; kmsg "A6L_C16_SWEEP_END dt_hi846_lanes=$L hi846_pass=$hpass"
  if [ $hpass = 0 ] && [ "${AUTODIAG:-1}" = 1 ] && ! grep -q KERNEL_OOPS_FAIL /tmp/cam-*-c16-*.out 2>/dev/null; then
    say "no hi846 row passed: AUTODIAG hdiag (reboot afterwards)"; hdiag > /tmp/cam16-hdiag.out 2>&1; cat /tmp/cam16-hdiag.out | grep "A6L_CAM"
  fi
  kmsg "A6L_C16_DONE"; say "C16_DONE (pull /tmp/c16.tar: see docs/hi846-20260929.md)"
  ;;
hdiag|hi846diag) hdiag ;;
focus) mknodes; $D/a6l_camcap -s gt9769 -f ${STEPS:-0,256,512,768,1023,512,0} && say FOCUS_DONE || say FOCUS_FAIL ;;
off) for m in imx576_a6l s5k3t1 hi846 gt9769; do rmmod $m 2>/dev/null; done; say "OFF_DONE sensors removed" ;;
*) say "unknown MODE=$MODE"; exit 2;;
esac
