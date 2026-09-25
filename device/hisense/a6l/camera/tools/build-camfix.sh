#!/bin/bash
# camfix build (24 Sep 2026): patched qcom-camss + hi846, sensor modules with frame-counter debug, camera dtbo
# (+ CSID rails), runtime overlay module, a6l_camcap -> firmware/extracted/camera-20260924b + laptop v75/camera2.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
B=/home/a6l/camfix; A=$W/firmware/extracted/camera-20260924b; OLD=$W/firmware/extracted/camera-20260924
P=$W/device/hisense/a6l/kernel/camera
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
rm -rf $B; mkdir -p $B/mod $B/camss $B/orig $B/ovl $A/extra
step sources+patch
cp $P/imx576_a6l.c $P/s5k3t1.c $P/gt9769.c $B/mod/
cp $K/drivers/i2c/busses/i2c-qcom-cci.c $K/drivers/media/i2c/hi846.c $K/drivers/media/v4l2-core/v4l2-cci.c $B/mod/
cp -r $K/drivers/media/platform/qcom/camss/. $B/camss/
cp -r $K/drivers/media/platform/qcom/camss $B/orig/camss; cp $K/drivers/media/i2c/hi846.c $B/orig/
python3 $P/patches/camfix_patch.py $B/camss $B/mod/hi846.c || { echo CAMFIX_PATCH_FAIL; exit 1; }
(cd $B/orig && diff -u camss/camss.c $B/camss/camss.c; diff -u camss/camss-csiphy.c $B/camss/camss-csiphy.c;
 diff -u camss/camss-csid.c $B/camss/camss-csid.c; diff -u camss/camss-csiphy-3ph-1-0.c $B/camss/camss-csiphy-3ph-1-0.c) > $P/patches/camss-sdm660-camfix.patch
(cd $B/orig && diff -u hi846.c $B/mod/hi846.c) > $P/patches/hi846-4lane-default.patch
wc -l $P/patches/*.patch
step sensor modules
printf 'ccflags-y += -DCONFIG_V4L2_CCI_MODULE=1 -DCONFIG_V4L2_CCI_I2C_MODULE=1\nobj-m += v4l2-cci.o imx576_a6l.o s5k3t1.o gt9769.o hi846.o i2c-qcom-cci.o\n' > $B/mod/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/mod -k modules 2>&1 | grep -E "error|warning|undefined" | head -40
ls $B/mod/*.ko | wc -l
step camss module
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/camss -k modules 2>&1 | grep -E "error|warning|undefined" | head -40
ls -la $B/camss/qcom-camss.ko && modinfo $B/camss/qcom-camss.ko | grep -E "vermagic|^parm"
for m in $B/mod/*.ko $B/camss/qcom-camss.ko; do llvm-strip --strip-debug $m; done
step overlay dtbo
DTC=$O/scripts/dtc/dtc; src=$W/device/hisense/a6l/kernel/a6l-camera-v75.dtso
cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts $src -o $B/a6l-camera-v75.pp
$DTC -@ -I dts -O dtb -o $B/a6l-camera-v75.dtbo $B/a6l-camera-v75.pp 2>&1 | grep -v "unit_address\|avoid_default_addr" | head -8
BASE=/home/a6l/kernel/kvoice-build/dt/v74.dtb
fdtoverlay -i $BASE -o $B/merged-v74.dtb $B/a6l-camera-v75.dtbo && echo A6L_CAM_OVL_MERGE_PASS || echo A6L_CAM_OVL_MERGE_FAIL
M=$B/merged-v74.dtb; C=/soc@0/camss@ca00020
echo "camss status=$(fdtget $M $C status) vdda=$(fdtget -t x $M $C vdda-supply) vdd_sec=$(fdtget -t x $M $C vdd_sec-supply)"
echo "l1a phandle=$(fdtget -t x $M /remoteproc/glink-edge/rpm-requests/regulators-1/l1 phandle) l1b phandle=$(fdtget -t x $M /remoteproc/glink-edge/rpm-requests/regulators-0/l1 phandle)"
for n in /soc@0/cci@ca0c000/i2c-bus@0/camera@1a /soc@0/cci@ca0c000/i2c-bus@0/vcm@c /soc@0/cci@ca0c000/i2c-bus@1/camera@20 /soc@0/cci@ca0c000/i2c-bus@1/camera@2d; do
  echo "$n: $(fdtget $M $n compatible 2>&1)"; done
step runtime overlay module
cp $P/ovl/Kbuild $P/ovl/a6l_cam_ovl.c $B/ovl/
{ echo "/* generated from a6l-camera-v75.dtbo (camfix) */"; echo "static const unsigned char a6l_cam_dtbo[] __aligned(8) = {"; xxd -i < $B/a6l-camera-v75.dtbo; echo "};"; } > $B/ovl/a6l_cam_dtbo.h
make -s -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ovl modules 2>&1 | tail -5
llvm-strip --strip-debug $B/ovl/a6l_cam_ovl.ko; modinfo $B/ovl/a6l_cam_ovl.ko | grep vermagic
cmp <(tail -c +1 $B/a6l-camera-v75.dtbo) <(tail -c +1 $B/a6l-camera-v75.dtbo) && grep -c "0x" $B/ovl/a6l_cam_dtbo.h
step camcap
$NDK/aarch64-linux-android34-clang -static -O2 -Wall -Wextra -o $B/a6l_camcap $W/device/hisense/a6l/camera/tools/a6l_camcap.c 2>&1 | head && $NDK/llvm-strip $B/a6l_camcap && file $B/a6l_camcap | cut -c1-90
step assemble $A
cp $B/mod/v4l2-cci.ko $B/mod/imx576_a6l.ko $B/mod/s5k3t1.ko $B/mod/gt9769.ko $B/mod/hi846.ko $B/mod/i2c-qcom-cci.ko $B/camss/qcom-camss.ko $B/a6l-camera-v75.dtbo $B/a6l_camcap $A/
cp $B/ovl/a6l_cam_ovl.ko $A/extra/; cp $B/ovl/a6l_cam_ovl.ko $P/ovl/a6l_cam_ovl.ko
for f in mc.ko videodev.ko v4l2-async.ko v4l2-fwnode.ko videobuf2-common.ko videobuf2-memops.ko videobuf2-dma-sg.ko videobuf2-v4l2.ko led-class-flash.ko leds-qcom-flash.ko load-order.txt raw10_to_png.py; do cp $OLD/$f $A/; done
cp $W/device/hisense/a6l/camera/run-camera.sh $A/
(cd $A && sha256sum *.ko extra/a6l_cam_ovl.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS; cat SHA256SUMS; ls | wc -l)
step laptop stage
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
cd $W/.relay
./lap.sh 30 'mkdir -p ~/A6L-usb-20260915/v75/camera2/extra && echo LAPTOP_MKDIR_OK'
timeout 120 $SCP 'C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260924b/*' a6l-laptop:A6L-usb-20260915/v75/camera2/ 2>&1 | tail -3
timeout 60 $SCP 'C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260924b/extra/a6l_cam_ovl.ko' a6l-laptop:A6L-usb-20260915/v75/camera2/extra/ 2>&1 | tail -3
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera2 && sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$"; sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$"'
echo CAMFIX_BUILD_DONE
