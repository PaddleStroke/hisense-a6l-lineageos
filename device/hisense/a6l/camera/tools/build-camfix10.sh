#!/bin/bash
# camfix10 build (30 Sep 2026): camfix + camfix2..camfix9b + camfix10 (CSID TPG split, IOVA/CSID/CSIPHY/SMMU per-frame
# diagnostics, contiguous slots, stock IRQ_MASK_1; qcom_camss.a6l_v10 / a6l_v10_tpg) -> qcom-camss.ko
# Output: firmware/extracted/camera-20260930-camfix10 (all other files byte-identical to camera-20260930-camfix9).
# Laptop staging (camera12): tools/stage-camfix10-laptop.sh.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
B=/home/a6l/camfix10; A=$W/firmware/extracted/camera-20260930-camfix10; OLD=$W/firmware/extracted/camera-20260930-camfix9
P=$W/device/hisense/a6l/kernel/camera
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
[ -e $B ] && B=$B-$(date +%H%M%S)  # never delete an earlier build dir
mkdir -p $B/camss $B/orig $B/src-snapshot $A/extra
step sources+patch
cp -r $K/drivers/media/platform/qcom/camss/. $B/camss/
cp -r $K/drivers/media/platform/qcom/camss $B/orig/camss; cp $K/drivers/media/i2c/hi846.c $B/hi846.c
for f in camfix camfix2 camfix3 camfix4 camfix5 camfix6 camfix7 camfix8 camfix9 camfix9b camfix10; do
  if [ $f = camfix ]; then python3 $P/patches/${f}_patch.py $B/camss $B/hi846.c; else python3 $P/patches/${f}_patch.py $B/camss; fi \
    > $B/$f.patchlog 2>&1 || { cat $B/$f.patchlog; echo ${f^^}_PATCH_FAIL; exit 1; }
done
cat $B/camfix10.patchlog
(cd $B && diff -ru orig/camss camss) > $P/patches/camss-sdm660-camfix10-cumulative.patch
wc -l $P/patches/camss-sdm660-camfix10-cumulative.patch
step camss module W=1
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/camss -k modules > $B/camss-build.log 2>&1
grep -E "error|undefined" $B/camss-build.log | head -20
echo "warnings total $(grep -c 'warning:' $B/camss-build.log), in a6l code: $(grep 'warning:' $B/camss-build.log | grep -c -i 'a6l')"
grep 'warning:' $B/camss-build.log | head -10
ls -la $B/camss/qcom-camss.ko || { echo CAMFIX10_BUILD_FAIL; exit 1; }
modinfo $B/camss/qcom-camss.ko | grep -E "^vermagic|^parm: *a6l_(v6|v7|v8|v9|v10|v10_tpg|stk)"
llvm-nm -u $B/camss/qcom-camss.ko | grep -E "clk_|udelay|__const_udelay|vfe_get|vfe_put|iommu|crc32|vmalloc_to_page" | tr '\n' ' '; echo
for s in $(llvm-nm -u $B/camss/qcom-camss.ko | awk '{print $2}' | grep -E "^(clk_|__clk_|of_clk_|clk_hw_|iommu_|crc32|vmalloc_to_page)"); do grep -q -w "$s" $O/Module.symvers && echo "sym $s OK" || echo "sym $s MISSING"; done
llvm-strip --strip-debug $B/camss/qcom-camss.ko
step assemble $A
cp $B/camss/*.c $B/camss/*.h $B/src-snapshot/ 2>/dev/null
for f in $(cd $OLD && ls); do [ -f $OLD/$f ] && cp $OLD/$f $A/; done
cp $OLD/extra/a6l_cam_ovl.ko $A/extra/
cp $B/camss/qcom-camss.ko $A/
cp $W/device/hisense/a6l/camera/run-camera.sh $A/
(cd $A && sha256sum *.ko extra/a6l_cam_ovl.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS; cat SHA256SUMS; ls | wc -l)
echo "B=$B"
echo CAMFIX10_BUILD_DONE
