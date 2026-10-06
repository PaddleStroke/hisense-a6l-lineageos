#!/bin/bash
# camfix7 build (29 Sep 2026): camfix + camfix2..camfix7 -> qcom-camss.ko, plus imx576_a6l.ko (a6l_hts line-length
# override; built together with v4l2-cci like camfix1 so the CCI symbol CRCs match the bundled v4l2-cci.ko).
# Output: firmware/extracted/camera-20260929-camfix7 (all other files byte-identical to camera-20260928-camfix6).
# Laptop staging (camera9) is a separate step: tools/stage-camfix7-laptop.sh.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
B=/home/a6l/camfix7; A=$W/firmware/extracted/camera-20260929-camfix7; OLD=$W/firmware/extracted/camera-20260928-camfix6
P=$W/device/hisense/a6l/kernel/camera
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
[ -e $B ] && B=$B-$(date +%H%M%S)  # never delete an earlier build dir
mkdir -p $B/camss $B/orig $B/mod $B/src-snapshot $A/extra
step sources+patch
cp -r $K/drivers/media/platform/qcom/camss/. $B/camss/
cp -r $K/drivers/media/platform/qcom/camss $B/orig/camss; cp $K/drivers/media/i2c/hi846.c $B/hi846.c
for f in camfix camfix2 camfix3 camfix4 camfix5 camfix6 camfix7; do
  if [ $f = camfix ]; then python3 $P/patches/${f}_patch.py $B/camss $B/hi846.c; else python3 $P/patches/${f}_patch.py $B/camss; fi \
    > $B/$f.patchlog 2>&1 || { cat $B/$f.patchlog; echo ${f^^}_PATCH_FAIL; exit 1; }
done
tail -1 $B/camfix7.patchlog
(cd $B && diff -ru orig/camss camss) > $P/patches/camss-sdm660-camfix7-cumulative.patch
wc -l $P/patches/camss-sdm660-camfix7-cumulative.patch
step camss module W=1
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/camss -k modules > $B/camss-build.log 2>&1
grep -E "error|undefined" $B/camss-build.log | head -20
echo "warnings total $(grep -c 'warning:' $B/camss-build.log), in a6l code: $(grep 'warning:' $B/camss-build.log | grep -c -i 'a6l')"
grep 'warning:' $B/camss-build.log | grep -i a6l | head -10
ls -la $B/camss/qcom-camss.ko || { echo CAMFIX7_BUILD_FAIL; exit 1; }
step imx576 module
cp $P/imx576_a6l.c $B/mod/; cp $K/drivers/media/v4l2-core/v4l2-cci.c $B/mod/
printf 'ccflags-y += -DCONFIG_V4L2_CCI_MODULE=1 -DCONFIG_V4L2_CCI_I2C_MODULE=1\nobj-m += v4l2-cci.o imx576_a6l.o\n' > $B/mod/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/mod -k modules > $B/mod-build.log 2>&1
grep -E "error|warning|undefined" $B/mod-build.log | head -20
ls -la $B/mod/imx576_a6l.ko || { echo IMX576_BUILD_FAIL; exit 1; }
for m in $B/camss/qcom-camss.ko $B/mod/imx576_a6l.ko; do modinfo $m | grep -E "^vermagic|^parm: *a6l_(v6|v7|hts)"; done
llvm-strip --strip-debug $B/camss/qcom-camss.ko $B/mod/imx576_a6l.ko
step assemble $A
cp $B/camss/*.c $B/camss/*.h $B/src-snapshot/ 2>/dev/null; cp $B/mod/imx576_a6l.c $B/src-snapshot/
for f in $(cd $OLD && ls); do [ -f $OLD/$f ] && cp $OLD/$f $A/; done
cp $OLD/extra/a6l_cam_ovl.ko $A/extra/
cp $B/camss/qcom-camss.ko $B/mod/imx576_a6l.ko $A/
cp $W/device/hisense/a6l/camera/run-camera.sh $A/
(cd $A && sha256sum *.ko extra/a6l_cam_ovl.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS; cat SHA256SUMS; ls | wc -l)
echo "B=$B"
echo CAMFIX7_BUILD_DONE
