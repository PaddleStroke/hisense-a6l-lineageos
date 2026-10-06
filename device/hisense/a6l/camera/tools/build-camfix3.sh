#!/bin/bash
# camfix3 build (25 Sep 2026): camfix + camfix2 + camfix3 patches -> qcom-camss.ko only (sensor/other modules are
# byte-identical to camera-20260925) -> firmware/extracted/camera-20260925b + laptop v75/camera4.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
B=/home/a6l/camfix3; A=$W/firmware/extracted/camera-20260925b; OLD=$W/firmware/extracted/camera-20260925
P=$W/device/hisense/a6l/kernel/camera
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
rm -rf $B; mkdir -p $B/camss $B/orig $B/src-snapshot $A/extra
step sources+patch
cp -r $K/drivers/media/platform/qcom/camss/. $B/camss/
cp -r $K/drivers/media/platform/qcom/camss $B/orig/camss; cp $K/drivers/media/i2c/hi846.c $B/hi846.c
python3 $P/patches/camfix_patch.py $B/camss $B/hi846.c || { echo CAMFIX_PATCH_FAIL; exit 1; }
python3 $P/patches/camfix2_patch.py $B/camss || { echo CAMFIX2_PATCH_FAIL; exit 1; }
python3 $P/patches/camfix3_patch.py $B/camss || { echo CAMFIX3_PATCH_FAIL; exit 1; }
(cd $B && diff -ru orig/camss camss) > $P/patches/camss-sdm660-camfix3-cumulative.patch
wc -l $P/patches/*.patch
step camss module
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/camss -k modules 2>&1 | grep -E "error|warning|undefined" | head -40
ls -la $B/camss/qcom-camss.ko || { echo CAMFIX3_BUILD_FAIL; exit 1; }
modinfo $B/camss/qcom-camss.ko | grep -E "vermagic|^parm"
llvm-strip --strip-debug $B/camss/qcom-camss.ko
step assemble $A
cp $B/camss/*.c $B/camss/*.h $B/src-snapshot/ 2>/dev/null
for f in $(cd $OLD && ls); do [ -f $OLD/$f ] && cp $OLD/$f $A/; done
cp $OLD/extra/a6l_cam_ovl.ko $A/extra/
cp $B/camss/qcom-camss.ko $A/
cp $W/device/hisense/a6l/camera/run-camera.sh $A/
(cd $A && sha256sum *.ko extra/a6l_cam_ovl.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS; cat SHA256SUMS; ls | wc -l)
step laptop stage
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
cd $W/.relay
./lap.sh 30 'mkdir -p ~/A6L-usb-20260915/v75/camera4/extra && echo LAPTOP_MKDIR_OK'
timeout 120 $SCP 'C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260925b/*' a6l-laptop:A6L-usb-20260915/v75/camera4/ 2>&1 | tail -3
timeout 60 $SCP 'C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260925b/extra/a6l_cam_ovl.ko' a6l-laptop:A6L-usb-20260915/v75/camera4/extra/ 2>&1 | tail -3
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera4 && echo bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")'
echo CAMFIX3_BUILD_DONE
