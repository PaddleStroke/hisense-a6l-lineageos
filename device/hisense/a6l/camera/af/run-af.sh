#!/system/bin/sh
# A6L camera phase 3: GT9769 VCM autofocus recovery test (docs/camera-af-video-20260929.md). ATTENDED ONLY.
# Needs the camera modules loaded by the camera15 bundle (D15=/tmp/camera15, MODE=probe) exactly like run-libcam.sh.
# Nothing here writes flash/eMMC. The EEPROM is only READ (a6l_afotp: 2-byte address pointer + read).
# Pure toybox/mksh (no awk).
#   MODE=env|otp|click|sweep|af|cont|all
#     env   : bundle SHA, nodes, VCM subdev + its i2c bus, cam version
#     otp   : read the imx576 module EEPROM (CCI0 0x58), decode the stock AF calibration -> A6L_AF_OTP / _YAML
#     click : VCM 0 -> 1023 -> 0 in 8 steps (listen / watch the lens), no camera stream
#     sweep : for each lens position (POS) capture with the NO-AF bundle (SL=/tmp/libcam2) and print the sharpness
#             of the last frame (a6l_afsharp): A6L_AF_SWEEP pos=<dac> sharp=<v>, then A6L_AF_SWEEP_BEST
#     af    : AF bundle (L=/tmp/libcamaf): AfMode Auto + AfTrigger Start, IPA trace (A6L_AF pos/sharp per step,
#             A6L_AF_RESULT), then VERIFY=1: manual captures at the AF result and at infinity, sharpness of both
#     cont  : AF bundle, AfMode Continuous for N=300 frames (~10 s): point at something near, then far
#     all   : env otp sweep af
#   CAM=rear (only the rear imx576 has a VCM)  SIZE=1440x1078 (SoftISP centre crop, proven 30 fps)  FMT=ABGR8888
#   POS="0 64 ... 1023" (sweep positions)  FINE=1 (sweep: +-48 around the best step 16)  WM=3  OUT=/tmp/afout
# Test scene: a printed page / textured object ~20-30 cm away in good light, phone steady (a stand), then far.
export PATH=/tmp/bin:$PATH
L=${L:-/tmp/libcamaf}; SL=${SL:-/tmp/libcam2}; D15=${D15:-/tmp/camera15}; MODE=${MODE:-all}; CAM=${CAM:-rear}
WM=${WM:-3}; SIZE=${SIZE:-1440x1078}; FMT=${FMT:-ABGR8888}; OUT=${OUT:-/tmp/afout}; mkdir -p $OUT
POS=${POS:-0 64 128 192 256 320 384 448 512 576 640 704 768 832 896 960 1023}
say() { echo "A6L_AF $*"; echo "A6L_AF $*" > /dev/kmsg 2>/dev/null; }
chmod 755 $L/bin/* $L/libexec/libcamera/* $SL/bin/* $SL/libexec/libcamera/* 2>/dev/null

libenv() {   # $1 = bundle prefix
  export LD_LIBRARY_PATH=$1/lib LIBCAMERA_IPA_MODULE_PATH=$1/lib/libcamera/ipa
  export LIBCAMERA_IPA_PROXY_PATH=$1/libexec/libcamera LIBCAMERA_IPA_CONFIG_PATH=$1/share/libcamera/ipa
  export LIBCAMERA_DATA_DIR=$1/share/libcamera LIBCAMERA_SYSCONF_DIR=$1/etc/libcamera
  export LIBCAMERA_LOG_COLOR=0 LIBCAMERA_PIPELINES_MATCH_LIST=simple
  export LIBCAMERA_LOG_LEVELS=${LOG:-*:WARN,SoftwareIsp:INFO,IPASoftAf:DEBUG}
}
nodes() {   # recovery has no ueventd (same as run-libcam.sh)
  for p in /sys/class/video4linux/* /sys/bus/media/devices/* /sys/class/misc/udmabuf /sys/class/i2c-dev/*; do
    [ -f $p/dev ] || continue; n=${p##*/}; mm=$(cat $p/dev)
    [ -c /dev/$n ] || mknod /dev/$n c ${mm%%:*} ${mm##*:}
  done
  mkdir -p /dev/dma_heap
  for p in /sys/class/dma_heap/*; do
    [ -f $p/dev ] || continue; n=${p##*/}; mm=$(cat $p/dev)
    [ -c /dev/dma_heap/$n ] || mknod /dev/dma_heap/$n c ${mm%%:*} ${mm##*:}
  done
  chmod 666 /dev/dma_heap/* /dev/udmabuf 2>/dev/null
}
camid() {   # rear camera id from `cam --list` (DT node camera@1a)
  $1/bin/cam --list 2>/dev/null | grep -F 'camera@1a)' | head -1 | sed 's/.*(\(.*\))[^)]*$/\1/'
}
vcm_find() {
  VCM=; VBUS=
  for p in /sys/class/video4linux/v4l-subdev*; do
    grep -q gt9769 $p/name 2>/dev/null && VCM=/dev/${p##*/}
  done
  for p in /sys/bus/i2c/drivers/gt9769/*-000c; do [ -e "$p" ] && { d=${p##*/}; VBUS=${d%-000c}; }; done
  [ -n "$VCM" ] && [ -n "$VBUS" ] && say "VCM $VCM i2c-$VBUS" && return 0
  say "VCM_MISSING (gt9769 not probed: run D=$D15 MODE=probe sh $D15/run-camera.sh first)"; return 1
}
hold_vcm() {   # keep the subdev open: runtime PM stays active (VAF/AVDD on, lens is not parked between steps)
  [ -n "$HELD" ] && return 0
  exec 7<$VCM && HELD=1 && say "VCM_HELD $VCM"
}
setpos() { $D15/a6l_camcap -s gt9769 -f $1 2>/dev/null | grep -q "A6L_CAMCAP_FOCUS $1"; }
setwm() { echo $WM > /sys/module/qcom_camss/parameters/a6l_wm 2>/dev/null; }
sha_check() {   # $1 bundle
  bad=0; n=0
  while read sum name; do
    [ -n "$name" ] || continue
    got=$(sha256sum "$1/$name" 2>/dev/null | cut -d' ' -f1)
    [ "$got" = "$sum" ] && n=$((n+1)) || { say "SHA_BAD $1/$name"; bad=1; }
  done < $1/SHA256SUMS
  [ $bad = 0 ] && say "SHA_PASS $1 $n files" || say "SHA_FAIL $1"
  return $bad
}
# one capture of 4 frames; prints the sharpness of the last frame. $1 bundle $2 tag $3 pos-label [$4 script]
shot() {
  libenv $1; id=$(camid $1)
  [ -n "$id" ] || { say "SHOT_FAIL $2 no rear camera in cam --list"; return 1; }
  rm -f $OUT/$2-*.bin
  W=${SIZE%x*}; H=${SIZE#*x}
  timeout 60 $1/bin/cam -c "$id" --capture=${SN:-4} --stream "role=viewfinder,width=$W,height=$H,pixelformat=$FMT" \
    --file="$OUT/$2-#.bin" ${4:+--script=$4} > $OUT/$2.txt 2> $OUT/$2.log
  last=$(ls $OUT/$2-*.bin 2>/dev/null | tail -1)
  [ -n "$last" ] || { say "SHOT_FAIL $2 no frame (see $OUT/$2.log)"; grep -E "ERROR|FATAL" $OUT/$2.log | head -5; return 1; }
  sz=$(wc -c < $last | tr -d ' '); stride=$((sz / H))
  r=$($L/bin/a6l_afsharp -f $FMT -w $W -h $H -s $stride -p $3 $last) || { say "SHARP_FAIL $2"; return 1; }
  # keep only the last frame of each shot
  for f in $OUT/$2-*.bin; do [ "$f" = "$last" ] || rm -f $f; done
  echo "$r"
}
field() { echo "$2" | tr ' ' '\n' | grep "^$1=" | head -1 | cut -d= -f2; }

env_mode() {
  ok=1; nodes
  sha_check $L || ok=0
  [ -f $SL/SHA256SUMS ] && { sha_check $SL || ok=0; } || say "NO_SWEEP_BUNDLE $SL (sweep needs the libcam2 bundle)"
  [ -x $D15/a6l_camcap ] || { say "NO_CAMCAP $D15/a6l_camcap"; ok=0; }
  [ -f /sys/module/qcom_camss/parameters/a6l_wm ] || { say "ENV_FAIL qcom_camss not loaded"; ok=0; }
  vcm_find || ok=0
  [ -n "$VBUS" ] && { [ -c /dev/i2c-$VBUS ] && say "I2C_DEV /dev/i2c-$VBUS" || { say "NO_I2C_DEV i2c-$VBUS"; ok=0; }; }
  libenv $L; v=$($L/bin/cam --version 2>&1 | grep -v linker | head -1); say "CAM_VERSION $v"
  [ $ok = 1 ] && say "ENV_PASS" || say "ENV_FAIL"
}
otp_mode() {
  nodes; vcm_find || return 1; hold_vcm
  $L/bin/a6l_afotp -b $VBUS -o $OUT/eeprom-imx576.bin > $OUT/otp.txt 2>&1; rc=$?
  if [ $rc = 3 ]; then say "OTP_RETRY chunk=1"; $L/bin/a6l_afotp -b $VBUS -n 1 -o $OUT/eeprom-imx576.bin > $OUT/otp.txt 2>&1; rc=$?; fi
  if [ $rc = 3 ] && [ -z "$OTP_NOSTREAM" ]; then   # EEPROM may need the sensor powered: read while streaming
    say "OTP_RETRY while streaming"
    ( SN=60 shot $SL otpstream 0 >/dev/null 2>&1 ) & sleep 3
    $L/bin/a6l_afotp -b $VBUS -o $OUT/eeprom-imx576.bin > $OUT/otp.txt 2>&1; rc=$?; wait
  fi
  cat $OUT/otp.txt
  case $rc in 0) say "OTP_PASS";; 1) say "OTP_READ_OK_AF_INVALID";; *) say "OTP_FAIL rc=$rc";; esac
}
click_mode() {
  nodes; vcm_find || return 1; hold_vcm
  for p in 0 256 512 768 1023 512 0; do setpos $p && say "CLICK pos=$p" || say "CLICK_FAIL pos=$p"; done
  say "CLICK_DONE (Pierre: did the lens move / click?)"
}
sweep_mode() {
  nodes; setwm; vcm_find || return 1; hold_vcm
  [ -f $SL/SHA256SUMS ] || { say "SWEEP_FAIL no $SL bundle"; return 1; }
  : > $OUT/sweep.txt
  one() {
    setpos $1 || { say "SWEEP_SETPOS_FAIL $1"; return 1; }
    r=$(shot $SL sw$1 $1) || return 1
    s=$(field sharp "$r"); m=$(field mean "$r")
    echo "$1 $s $m" >> $OUT/sweep.txt; say "SWEEP pos=$1 sharp=$s mean=$m"
  }
  for p in $POS; do one $p; done
  best=$(sort -k2 -g -r $OUT/sweep.txt | head -1); worst=$(sort -k2 -g $OUT/sweep.txt | head -1)
  bp=${best%% *}
  if [ -n "$FINE" ] && [ -n "$bp" ]; then
    for d in -48 -32 -16 16 32 48; do p=$((bp + d)); [ $p -ge 0 ] && [ $p -le 1023 ] && one $p; done
    best=$(sort -k2 -g -r $OUT/sweep.txt | head -1)
  fi
  set -- $best; bp=$1; bs=$2; set -- $worst; ws=$2
  n=$(grep -c . $OUT/sweep.txt)
  # ratio best/worst in per-mille without awk: scale both to integers (1e6)
  bi=$(echo $bs | sed 's/\.//;s/^0*//'); wi=$(echo $ws | sed 's/\.//;s/^0*//')
  ratio=$([ -n "$wi" ] && [ "$wi" -gt 0 ] && echo $((bi * 1000 / wi)) || echo 0)
  say "SWEEP_BEST pos=$bp sharp=$bs worst=$ws ratio_permille=$ratio samples=$n"
  [ $ratio -ge 1200 ] && say "SWEEP_PASS (focus changes with the lens: stock infinity..macro ~227..627)" \
    || say "SWEEP_FAIL (flat curve: lens not moving, scene without texture, or too dark)"
  setpos $bp
}
af_mode() {   # $1 = auto|cont
  nodes; setwm; vcm_find || return 1; hold_vcm; libenv $L
  id=$(camid $L); [ -n "$id" ] || { say "AF_FAIL no rear camera"; return 1; }
  W=${SIZE%x*}; H=${SIZE#*x}
  if [ "$1" = cont ]; then S=$L/af-script-continuous.yaml; NF=${N:-300}; tag=cont; else S=$L/af-script-auto.yaml; NF=${N:-150}; tag=auto; fi
  t0=$(date +%s)
  timeout 120 $L/bin/cam -c "$id" --capture=$NF --stream "role=viewfinder,width=$W,height=$H,pixelformat=$FMT" \
    --script=$S --metadata > $OUT/af-$tag.txt 2> $OUT/af-$tag.log; rc=$?; t1=$(date +%s)
  grep -q "Focus lens" $OUT/af-$tag.log && say "AF_LENS_PASS $(grep -m1 -o 'Focus lens.*' $OUT/af-$tag.log)" \
    || say "AF_LENS_FAIL (no lens ancillary link: libcamera did not find the VCM, AF controls absent)"
  grep -m1 -o "A6L_AF_INIT.*" $OUT/af-$tag.log | sed 's/^/A6L_AF /'
  grep -o "A6L_AF pos=[0-9]* sharp=[0-9.e+-]* state=[a-z]*" $OUT/af-$tag.log | sed 's/^A6L_AF /A6L_AF TRACE /' > $OUT/af-$tag-trace.txt
  cat $OUT/af-$tag-trace.txt
  grep -o "A6L_AF_RESULT.*" $OUT/af-$tag.log > $OUT/af-$tag-result.txt
  sed 's/^/A6L_AF /' $OUT/af-$tag-result.txt
  say "AF_RUN $tag rc=$rc frames=$NF time=$((t1-t0))s steps=$(grep -c . $OUT/af-$tag-trace.txt) scans=$(grep -c . $OUT/af-$tag-result.txt)"
  grep -m3 -E "AfState|LensPosition" $OUT/af-$tag.txt | head -3
  grep -E "ERROR|FATAL" $OUT/af-$tag.log | head -5
  last=$(tail -1 $OUT/af-$tag-result.txt)
  case "$last" in *state=focused*) say "AF_${tag}_PASS";; *) say "AF_${tag}_FAIL";; esac
  if [ "$tag" = auto ] && [ "${VERIFY:-1}" = 1 ]; then
    d=$(field dioptres "$last")
    [ -n "$d" ] || d=0
    printf 'frames:\n  - 0:\n      AfMode: 0\n      LensPosition: %s\n' "$d" > $OUT/man-af.yaml
    printf 'frames:\n  - 0:\n      AfMode: 0\n      LensPosition: 0.0\n' > $OUT/man-inf.yaml
    ra=$(SN=12 shot $L vaf af-$d $OUT/man-af.yaml); ri=$(SN=12 shot $L vinf inf-0 $OUT/man-inf.yaml)
    sa=$(field sharp "$ra"); si=$(field sharp "$ri")
    say "VERIFY af dioptres=$d sharp=$sa | infinity sharp=$si"
  fi
}

case $MODE in
env) env_mode;;
otp) otp_mode;;
click) click_mode;;
sweep) sweep_mode;;
af) af_mode auto;;
cont) af_mode cont;;
all) env_mode; otp_mode; sweep_mode; af_mode auto
     cd /tmp && tar cf /tmp/afout.tar ${OUT#/tmp/} && say "DONE tar=/tmp/afout.tar";;
*) say "unknown MODE=$MODE";;
esac
