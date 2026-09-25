#!/system/bin/sh
# A6L camera attended test, camfix2 revision (25 Sep 2026, bundle camera3 = firmware/extracted/camera-20260925).
# ATTENDED ONLY: switches the 2.8 V camera rail (gpio51), MCLKs and (new) the CSID rails pm660_l1 / pm660l_l1.
# Works on the V74 recovery: the camera DT comes from the runtime overlay extra/a6l_cam_ovl.ko (loaded first).
#   MODE=check|load|probe|list|bars|live|focus|off   SENSOR=imx576|hi846|s5k3t1   D=bundle dir (default /tmp/camera)
#   optional env: PHY= CSID= W= H= EXP= GAIN=
#   camfix2 runtime knobs (qcom_camss params, no reload): DBG=0|1|2 VFE_MIN=<Hz> CSID_IRQMASK=<0x..>
#   A kernel oops in an earlier run blocks bars/live (reboot first): the camss state is then undefined.
# Markers: "A6L_CAM <STEP>_PASS/_FAIL". Nothing here writes flash/eMMC.
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/camera}; MODE=${MODE:-probe}; SENSOR=${SENSOR:-imx576}
say() { echo "A6L_CAM $*"; }
chmod 755 $D/a6l_camcap 2>/dev/null
# CSID choice: sdm660 has 4 CSIDs but 2 VFEs; with the camfix camss any CSID works (ISPIF routes to VFE0),
# the defaults stay on CSID0/1 so an unpatched camss would still power up.
case "$SENSOR" in
imx576) PHY=${PHY:-0}; CSID=${CSID:-0}; W=${W:-2880}; H=${H:-2156}; BAYER=RGGB; EXP=${EXP:-2000}; GAIN=${GAIN:-512};;
hi846)  PHY=${PHY:-1}; CSID=${CSID:-0}; W=${W:-1632}; H=${H:-1224}; BAYER=GBRG; EXP=${EXP:-2000}; GAIN=${GAIN:-64};;
s5k3t1) PHY=${PHY:-2}; CSID=${CSID:-1}; W=${W:-2304}; H=${H:-1728}; BAYER=GRBG; EXP=${EXP:-1500}; GAIN=${GAIN:-128};;
*) say "unknown SENSOR=$SENSOR"; exit 2;;
esac

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
  ls /dev/media* /dev/video* /dev/v4l-subdev* 2>/dev/null | tr '\n' ' '; echo
}

load() {
  out=$(insmod $D/extra/a6l_cam_ovl.ko 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod a6l_cam_ovl: $out";; esac
  for m in $(cat $D/load-order.txt); do
    out=$(insmod $D/$m 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod $m: $out";; esac
  done
  sleep 3
  mknodes
}

mark() { dmesg | wc -l > /tmp/cam-dmesg-mark; }
since() { s=$(cat /tmp/cam-dmesg-mark 2>/dev/null || echo 0); dmesg | tail -n +$((s+1)); }

case "$MODE" in
check) check ;;
load|probe)
  check
  load
  dmesg | grep -iE "A6L_CAM_OVL|cci|camss|csiphy|ispif|imx576|s5k3t1|hi846|gt9769|chip id|supply" | tail -40
  dmesg | grep -q "Sony IMX576 chip id 0x576" && say IMX576_PROBE_PASS || say IMX576_PROBE_FAIL
  dmesg | grep -q "Samsung S5K3T1 chip id 0x3141" && say S5K3T1_PROBE_PASS || say S5K3T1_PROBE_FAIL
  dmesg | grep -q "hi846.*chip id 08 46" && say HI846_PROBE_PASS || say HI846_PROBE_FAIL
  dmesg | grep -q "GT9769 VCM initialised" && say GT9769_PROBE_PASS || say GT9769_PROBE_FAIL
  [ -f /sys/module/qcom_camss/parameters/a6l_dbg ] && say CAMSS_CAMFIX2_PASS || say "CAMSS_CAMFIX2_FAIL (old qcom-camss loaded: reboot)"
  dmesg | grep -q "supply vdda not found" && say CSID_RAILS_FAIL_dummy || say CSID_RAILS_PASS
  $D/a6l_camcap -l | grep -q "msm_ispif0" && say ISPIF_IN_GRAPH_PASS || say ISPIF_IN_GRAPH_FAIL
  say PROBE_DONE
  ;;
list) mknodes; $D/a6l_camcap -l; say LIST_DONE ;;
bars|live)
  mknodes
  if [ "$MODE" = bars ]; then TP="-t 2"; else TP="-t 0 -e $EXP -g $GAIN"; fi
  OUT=/tmp/cam-$SENSOR-$MODE.raw; rm -f $OUT
  if dmesg | grep -qE "Internal error: Oops|Unable to handle kernel"; then
    say "PREVIOUS_OOPS_IN_DMESG: reboot the phone (fresh V74 recovery) before capturing"; exit 3
  fi
  P=/sys/module/qcom_camss/parameters
  [ -n "$DBG" ] && echo $DBG > $P/a6l_dbg
  [ -n "$VFE_MIN" ] && echo $VFE_MIN > $P/a6l_vfe_min
  [ -n "$CSID_IRQMASK" ] && echo $((CSID_IRQMASK)) > $P/a6l_csid_irqmask
  for f in a6l_dbg a6l_vfe_min a6l_csid_irqmask a6l_phy_fast a6l_csid_fast; do printf '%s=%s ' $f "$(cat $P/$f 2>/dev/null)"; done; echo
  TO=""; command -v timeout >/dev/null 2>&1 && TO="timeout 60"
  mark
  $TO $D/a6l_camcap -s $SENSOR -p $PHY -c $CSID -W $W -H $H $TP -n 4 -o $OUT > /tmp/cam-$SENSOR-$MODE.log 2>&1
  echo "a6l_camcap rc=$?"
  cat /tmp/cam-$SENSOR-$MODE.log
  since > /tmp/cam-$SENSOR-$MODE.dmesg
  grep -E "A6L_|camss|csid|csiphy|ispif|vfe|VFE|$SENSOR|imx576|s5k3t1|hi846|smmu|Oops|Unable to handle" /tmp/cam-$SENSOR-$MODE.dmesg | tail -160
  grep -qE "Internal error: Oops|Unable to handle kernel" /tmp/cam-$SENSOR-$MODE.dmesg && say "KERNEL_OOPS_FAIL (reboot before the next camera test; send /tmp/cam-$SENSOR-$MODE.dmesg)"
  grep -q "A6L_VFE0 ERR" /tmp/cam-$SENSOR-$MODE.dmesg && say "VFE_ERR seen (bus overflow / violation, see A6L_VFE0 ERR)"
  grep -q "A6L_VFE0_STOP" /tmp/cam-$SENSOR-$MODE.dmesg && say "DIAG $(grep -o 'A6L_VFE0_STOP.*' /tmp/cam-$SENSOR-$MODE.dmesg | tail -1)"
  since | grep -q "SENSOR_STREAMING" && say "SENSOR_TX_PASS (frame counter moving)"
  since | grep -q "SENSOR_NOT_COUNTING" && say "SENSOR_TX_FAIL (frame counter static: sensor not streaming)"
  if [ -s $OUT ]; then
    BPL=$(grep -o "bpl [0-9]*" /tmp/cam-$SENSOR-$MODE.log | tail -1 | cut -d' ' -f2)
    say "CAPTURE_${SENSOR}_${MODE}_PASS $OUT $(wc -c < $OUT) bytes bpl=$BPL"
    say "host: adb pull $OUT && python3 raw10_to_png.py $(basename $OUT) $W $H $BPL $BAYER cam-$SENSOR-$MODE.png"
  else
    say "CAPTURE_${SENSOR}_${MODE}_FAIL"
  fi
  ;;
focus)
  mknodes
  $D/a6l_camcap -s gt9769 -f ${STEPS:-0,256,512,768,1023,512,0} && say FOCUS_DONE ask Pierre: lens movement heard/seen? || say FOCUS_FAIL
  ;;
off)
  for m in imx576_a6l s5k3t1 hi846 gt9769; do rmmod $m 2>/dev/null; done
  say "OFF_DONE sensors removed (rail gpio51 released by runtime PM)"
  ;;
*) say "unknown MODE=$MODE"; exit 2;;
esac
