#!/system/bin/sh
# A6L libcamera recovery test (libcam2, 29 Sep 2026; docs/libcamera-plan-20260929.md s.8/s.10).
# Upstream libcamera v0.7.2 + A6L patches 0001-0007, simple pipeline + SoftISP (CPU debayer) + simple IPA, `cam` utility,
# NDK r27c aarch64 bionic (API 30), installed prefix /tmp/libcam2. ATTENDED ONLY, fresh V75-usb recovery boot.
# The camera kernel modules are loaded by the camera15 bundle (D15=/tmp/camera15, MODE=probe), this script only
# adds libcamera on top. Nothing here writes flash/eMMC. Pure toybox/mksh (no awk).
#   MODE=env|list|info|bars|capture|live|suite|all|off
#   CAM=rear|front|wide (or imx576|s5k3t1|hi846, a full camera id, or a numeric `cam --list` index; the index order
#       is NOT stable across runs: t37 CAM=2 was s5k3t1 in one run and hi846 in the next) default rear
#   WM=3 (qcom_camss.a6l_wm)  N=10 frames  SIZE=<WxH> (SoftISP output; default = what cam picks for the viewfinder role)
#   FMT=ABGR8888|RGB888|...  HEAP=<dma-heap name|udmabuf> (LIBCAMERA_DMA_HEAP override; default system heap, patch 0007)
#   LOG=<libcamera log levels>  TAG=<file tag> (default live-<CAM>)
# SoftISP output sizes (CPU debayer, RAW10 CSI2P: max = (W-8) x H of the sensor mode, NO scaling: a smaller SIZE is a
# centre CROP of the smallest sensor mode that covers it):
#   rear imx576 modes 2880x2156 (4:3 binned, full FOV) / 2880x1620 (16:9 binned) / 5760x4312 (full, ~99 MB per RGBA buffer)
#   front s5k3t1 2304x1728 only -> max 2296x1728.   wide hi846 1632x1224 / 1280x720 (does not stream yet)
export PATH=/tmp/bin:$PATH
L=${L:-/tmp/libcam2}; D15=${D15:-/tmp/camera15}; MODE=${MODE:-all}; CAM=${CAM:-rear}; N=${N:-10}; WM=${WM:-3}
OUT=${OUT:-/tmp/lc2}; mkdir -p $OUT
say() { echo "A6L_LC $*"; echo "A6L_LC $*" > /dev/kmsg 2>/dev/null; }
export LD_LIBRARY_PATH=$L/lib
export LIBCAMERA_IPA_MODULE_PATH=$L/lib/libcamera/ipa
export LIBCAMERA_IPA_PROXY_PATH=$L/libexec/libcamera
export LIBCAMERA_IPA_CONFIG_PATH=$L/share/libcamera/ipa
export LIBCAMERA_DATA_DIR=$L/share/libcamera
export LIBCAMERA_SYSCONF_DIR=$L/etc/libcamera
export LIBCAMERA_LOG_LEVELS=${LOG:-*:INFO,SimplePipeline:DEBUG,CameraSensor:DEBUG,IPASoft:DEBUG,IPAManager:DEBUG,IPAProxy:DEBUG,DmaBufAllocator:DEBUG}
export LIBCAMERA_LOG_COLOR=0 LIBCAMERA_PIPELINES_MATCH_LIST=simple
[ -n "$HEAP" ] && export LIBCAMERA_DMA_HEAP=$HEAP
chmod 755 $L/bin/* $L/libexec/libcamera/* 2>/dev/null
cam() { $L/bin/cam "$@"; }

nodes() {   # recovery has no ueventd: create every node libcamera needs from sysfs
  for p in /sys/class/video4linux/* /sys/bus/media/devices/* /sys/class/misc/udmabuf; do
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
# camera id from a name: match the DT node suffix in `cam --list` (IDs = DT paths, stable; indexes are not)
camid() {
  case $1 in
  rear|back|main|imx576|imx576_a6l) pat='camera@1a)';;
  front|selfie|s5k3t1) pat='camera@2d)';;
  wide|hi846) pat='camera@20)';;
  /*) echo "$1"; return;;
  *[!0-9]*) echo "$1"; return;;
  *) cam --list 2>/dev/null | grep "^$1: " | sed 's/.*(\(.*\))[^)]*$/\1/'; return;;
  esac
  cam --list 2>/dev/null | grep -F "$pat" | head -1 | sed 's/.*(\(.*\))[^)]*$/\1/'
}
camname() { case $1 in *camera@1a) echo rear;; *camera@2d) echo front;; *camera@20) echo wide;; *) echo c$CAM;; esac; }
env_check() {
  ok=1
  for f in /system/bin/linker64 /system/lib64/libc.so /system/lib64/libm.so /system/lib64/libdl.so; do
    [ -e $f ] || { say "ENV_MISSING $f"; ok=0; }; done
  bad=0; n=0   # toybox sha256sum: verify line by line
  while read sum name; do
    [ -n "$name" ] || continue
    got=$(sha256sum "$L/$name" 2>/dev/null | cut -d' ' -f1)
    [ "$got" = "$sum" ] && n=$((n+1)) || { say "SHA_BAD $name"; bad=1; }
  done < $L/SHA256SUMS
  [ $bad = 0 ] && say "SHA_PASS $n files" || { say "SHA_FAIL"; ok=0; }
  [ -f /sys/module/qcom_camss/parameters/a6l_wm ] || { say "ENV_FAIL qcom_camss (rom1) not loaded: run D=$D15 MODE=probe sh $D15/run-camera.sh first"; ok=0; }
  nodes
  say "NODES media=$(ls /dev/media* 2>/dev/null | wc -l) video=$(ls /dev/video* 2>/dev/null | wc -l) subdev=$(ls /dev/v4l-subdev* 2>/dev/null | wc -l) heaps=$(ls /dev/dma_heap 2>/dev/null | tr '\n' ' ') udmabuf=$([ -c /dev/udmabuf ] && echo y || echo n)"
  out=$(cam --version 2>&1 | grep -v linker | head -3); say "CAM_VERSION $out"
  case "$out" in *v0.7*) ;; *) say "ENV_FAIL cam does not run (linker/libs)"; ok=0;; esac
  [ $ok = 1 ] && say "ENV_PASS" || say "ENV_FAIL"
}
setwm() { echo $WM > /sys/module/qcom_camss/parameters/a6l_wm 2>/dev/null; say "a6l_wm=$(cat /sys/module/qcom_camss/parameters/a6l_wm)"; }
list() {
  cam --list > $OUT/list.txt 2> $OUT/list.log; cat $OUT/list.txt
  n=$(grep -c '^[0-9]*:' $OUT/list.txt); say "LIST cameras=$n $(grep '^[0-9]*:' $OUT/list.txt | tr '\n' ' ')"
  for c in rear front wide; do say "LIST_ID $c=$(camid $c)"; done
  grep -E "ERROR|WARN|Mandatory|not supported|sensor|matched|DmaBufAllocator" $OUT/list.log | head -30
  [ "$n" -ge 1 ] && say "LIST_PASS" || say "LIST_FAIL (see $OUT/list.log)"
}
info() {
  id=$(camid $CAM); nm=$(camname "$id")
  cam -c "$id" --info --list-properties --list-controls > $OUT/info-$nm.txt 2> $OUT/info-$nm.log
  say "INFO $nm id=$id"
  grep -E "Location|Rotation|Model|PixelArray|UnitCell|ExposureTime|AnalogueGain|TestPattern|S[RGB]*10|:" $OUT/info-$nm.txt | head -40
}
capture() {   # $1 tag, rest = extra cam args
  tag=$1; shift; rm -f $OUT/$tag-* $OUT/$tag.txt $OUT/$tag.log
  id=$(camid $CAM)
  [ -n "$id" ] || { say "CAPTURE_${tag}_FAIL no camera for CAM=$CAM (see MODE=list)"; return 1; }
  st="role=viewfinder${SIZE:+,width=${SIZE%x*},height=${SIZE#*x}}${FMT:+,pixelformat=$FMT}"
  t0=$(date +%s)
  timeout 90 $L/bin/cam -c "$id" --capture=$N --stream "$st" --file="$OUT/$tag-#.bin" --metadata "$@" > $OUT/$tag.txt 2> $OUT/$tag.log
  rc=$?; t1=$(date +%s)
  nf=$(ls $OUT/$tag-*.bin 2>/dev/null | wc -l); last=$(ls $OUT/$tag-*.bin 2>/dev/null | tail -1)
  sz=$([ -n "$last" ] && wc -c < "$last" | tr -d ' ')
  fps=$(grep -oE '[0-9]+\.[0-9]+ fps' $OUT/$tag.txt | tail -1)
  cfg=$(grep -m1 -oE 'configuring streams: \(0\) [0-9]+x[0-9]+-[A-Z0-9]+' $OUT/$tag.log | sed 's/.* //')
  inp=$(grep -m1 -oE 'Input [0-9]+x[0-9]+-[A-Z0-9-]+' $OUT/$tag.log | sed 's/Input //')
  heap=$(grep -oE 'Using /dev/[a-z_/,.-]+' $OUT/$tag.log | sort -u | sed 's#Using /dev/##' | tr '\n' ' ')
  say "CAPTURE $tag cam=$id rc=$rc frames=$nf size=$sz time=$((t1-t0))s $fps out=$cfg in=$inp heap=$heap"
  grep -E "ERROR|FATAL|Mandatory|failed|Failed" $OUT/$tag.log | head -15
  dmesg | grep -E "cma: .*alloc failed" | tail -2
  grep -m3 -E "ExposureTime|AnalogueGain|ColourGains" $OUT/$tag.txt
  [ $rc = 0 ] && [ "$nf" -ge 1 ] && say "CAPTURE_${tag}_PASS" || say "CAPTURE_${tag}_FAIL"
}
case $MODE in
env) env_check;;
list) nodes; list;;
info) nodes; info;;
bars) nodes; setwm; printf 'frames:\n  - 0:\n      TestPatternMode: 2\n' > $OUT/bars.yaml
      id=$(camid $CAM); capture ${TAG:-bars-$(camname "$id")} --script=$OUT/bars.yaml;;
capture|live) nodes; setwm; id=$(camid $CAM); capture ${TAG:-live-$(camname "$id")};;
suite)   # the libcam2 attended set: rear full-FOV (binned) + 16:9 + front + wide attempt
  nodes; setwm
  (CAM=rear; SIZE=2872x2156; capture rear-full)
  (CAM=rear; SIZE=2872x1620; capture rear-169)
  (CAM=rear; SIZE=1440x1078; capture rear-crop)
  (CAM=front; SIZE=2296x1728; capture front-full)
  (CAM=front; SIZE=; capture front-default)
  (CAM=wide; N=3; SIZE=; capture wide)
  dmesg | grep -iE "camss|imx576|s5k3t1|hi846|vfe|csid|cma" | tail -40 > $OUT/dmesg-tail.txt
  cd /tmp && tar cf /tmp/lc2.tar lc2 && say "DONE tar=/tmp/lc2.tar";;
all)
  env_check; setwm; list
  for c in rear front; do CAM=$c; info; capture live-$c; done
  dmesg | grep -iE "camss|imx576|s5k3t1|vfe|csid|cma" | tail -30 > $OUT/dmesg-tail.txt
  cd /tmp && tar cf /tmp/lc2.tar lc2 && say "DONE tar=/tmp/lc2.tar";;
off) D=$D15 MODE=off sh $D15/run-camera.sh;;
*) say "unknown MODE=$MODE";;
esac
