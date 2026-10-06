#!/bin/bash
# camera16 build (29 Sep 2026, docs/hi846-20260929.md): hi846 2-lane investigation.
#  - a6l_cam_ovl.ko with hi846_lanes=4 (default: a6l-camera-v75.dtbo, unchanged) | 2 (a6l-camera-v75-hi846-2lane.dtbo)
#  - hi846.ko = mainline 7.2.3 + hi846-4lane-default.patch + camfix16_hi846_patch.py (knob a6l_rd, default 0)
#  - qcom-camss.ko = rom1 (unchanged, copied from camera-20260929-rom), camfix12 diag camss kept in extra/
# M= builds only against v67 (never touches out-a6l-rom-r5, the v67 .config or the Android out dir).
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
P=$W/device/hisense/a6l/kernel/camera
SRC=$W/firmware/extracted/camera-20260929-rom; A=$W/firmware/extracted/camera-20260929-hi846
B=$(cat /home/a6l/h846-16.last 2>/dev/null || echo /home/a6l/h846-16)
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
mkdir -p $B/ovl $B/h846 $B/h846base
step dtbo
for v in a6l-camera-v75 a6l-camera-v75-hi846-2lane; do
  cpp -nostdinc -I $K/include -I $K/scripts/dtc/include-prefixes -undef -D__DTS__ -x assembler-with-cpp $W/device/hisense/a6l/kernel/$v.dtso -o $B/$v.pp
  $O/scripts/dtc/dtc -@ -q -I dts -O dtb -o $B/$v.dtbo $B/$v.pp
done
cmp $B/a6l-camera-v75.dtbo $SRC/a6l-camera-v75.dtbo && echo ORIG_DTBO_REPRO_PASS || { echo ORIG_DTBO_REPRO_FAIL; exit 1; }
step overlay module
cp $P/ovl/Kbuild $P/ovl/a6l_cam_ovl.c $B/ovl/
{ echo "/* generated from a6l-camera-v75.dtbo */"; echo "static const unsigned char a6l_cam_dtbo[] __aligned(8) = {"; xxd -i < $B/a6l-camera-v75.dtbo; echo "};"; } > $B/ovl/a6l_cam_dtbo.h
{ echo "/* generated from a6l-camera-v75-hi846-2lane.dtbo */"; echo "static const unsigned char a6l_cam_dtbo_h2[] __aligned(8) = {"; xxd -i < $B/a6l-camera-v75-hi846-2lane.dtbo; echo "};"; } > $B/ovl/a6l_cam_dtbo_h2.h
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/ovl modules > $B/ovl-build.log 2>&1; tail -3 $B/ovl-build.log
echo "ovl warnings: $(grep -c 'warning:' $B/ovl-build.log)"
step hi846 base repro + camfix16
cp $K/drivers/media/i2c/hi846.c $B/h846base/hi846.c
(cd $B/h846base && patch -p0 --no-backup-if-mismatch hi846.c < $P/patches/hi846-4lane-default.patch) || { echo HI846_BASE_PATCH_FAIL; exit 1; }
cp $B/h846base/hi846.c $B/h846/hi846.c
python3 $P/patches/camfix16_hi846_patch.py $B/h846/hi846.c || exit 1
echo 'obj-m += hi846.o' > $B/h846/Kbuild; echo 'obj-m += hi846.o' > $B/h846base/Kbuild
for d in h846base h846; do make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/$d modules > $B/$d-build.log 2>&1; echo "$d warnings: $(grep -c 'warning:' $B/$d-build.log) errors: $(grep -c 'error' $B/$d-build.log)"; done
grep -E "warning:|error" $B/h846-build.log | head
(cd $B/h846base && diff -u hi846.c ../h846/hi846.c) > $P/patches/hi846-camfix16.patch; wc -l $P/patches/hi846-camfix16.patch
echo "srcversion bundle hi846=$(modinfo -F srcversion $SRC/hi846.ko) base-repro=$(modinfo -F srcversion $B/h846base/hi846.ko) camfix16=$(modinfo -F srcversion $B/h846/hi846.ko)"
for m in $B/ovl/a6l_cam_ovl.ko $B/h846/hi846.ko; do modinfo $m | grep -E "^vermagic|^parm|^depends"; 
  for s in $(llvm-nm -u $m | awk '{print $2}'); do grep -q -w "$s" $O/Module.symvers || echo "UNRESOLVED $s"; done; done
llvm-strip --strip-debug $B/ovl/a6l_cam_ovl.ko $B/h846/hi846.ko
step assemble $A
mkdir -p $A/extra
for f in $(cd $SRC && ls); do [ -f $SRC/$f ] && cp $SRC/$f $A/; done
cp $SRC/extra/qcom-camss-camfix12.ko $A/extra/
cp $SRC/extra/a6l_cam_ovl.ko $A/extra/a6l_cam_ovl-camera15.ko
cp $B/ovl/a6l_cam_ovl.ko $A/extra/a6l_cam_ovl.ko
cp $B/h846/hi846.ko $A/hi846.ko
cp $SRC/hi846.ko $A/extra/hi846-rom.ko
cp $B/a6l-camera-v75-hi846-2lane.dtbo $A/
cp $W/device/hisense/a6l/camera/run-camera16.sh $A/run-camera.sh
(cd $A && sha256sum *.ko extra/*.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS; sha256sum -c SHA256SUMS | grep -c ': OK$'; wc -l < SHA256SUMS; sha256sum qcom-camss.ko hi846.ko extra/a6l_cam_ovl.ko a6l-camera-v75-hi846-2lane.dtbo run-camera.sh SHA256SUMS)
echo "B=$B"; echo CAMERA16_BUILD_DONE
