#!/usr/bin/env bash
# rom-v1 pipeline (agent flash): stage prebuilts -> m (vendorimage [systemimage]) -> boot images -> ABL test -> QEMU boot.
# usage: rom-v1-pipeline.sh <run-tag> [vendor|system] [base|full]   (run in the background)
set -uo pipefail
TAG=$1; WHAT=${2:-vendor}; VARIANT=${3:-base}
R=/mnt/c/Users/Pierre/Desktop/A6L; L=/home/a6l/android/a6l-lineage24; B=/home/a6l/rom-v1/boot; Q=/home/a6l/rom-v1/qemu-modules
for f in $R/tools/stage-rom-v1-prebuilts.sh $R/tools/Prepare-RomV1Boot.py $R/tools/Test-RomV1Abl.py $R/tools/Test-RomV1Qemu.py; do sed -i 's/\r$//' $f; done
echo "== $(date) stage"; bash $R/tools/stage-rom-v1-prebuilts.sh > /home/a6l/rom-v1/stage.log 2>&1 || { tail -20 /home/a6l/rom-v1/stage.log; echo PIPELINE_FAIL stage; exit 1; }
tail -3 /home/a6l/rom-v1/stage.log
W=$R/device/hisense/a6l
# variant: base = empty include files; full = audio3 + hals subset (see device/hisense/a6l/rom/full/full.mk)
mkdir -p $L/device/hisense/a6l/audio/realinit && cp $W/audio/realinit/*.xml $L/device/hisense/a6l/audio/realinit/
V=$L/device/hisense/a6l/rom/variant; rm -rf $V; mkdir -p $V; : > $V/variant.mk; : > $V/BoardConfig-variant.mk
if [ "$VARIANT" = full ]; then
  cp $W/rom/full/full.mk $V/variant.mk; cp $W/rom/full/BoardConfig-full.mk $V/BoardConfig-variant.mk; cp $W/rom/full/init.a6l.wifibt.rc $V/
  cp $R/firmware/extracted/hals-20260924/bin/a6l_macs $V/a6l_macs
  printf 'PRODUCT_COPY_FILES += device/hisense/a6l/rom/variant/a6l_macs:$(TARGET_COPY_OUT_VENDOR)/bin/a6l_macs\n' >> $V/variant.mk
  # audio3: routing daemon sources + mixer paths + policy (replaces the realinit VM placeholder at the same path) + overlay
  mkdir -p $L/device/hisense/a6l/audio; cp -r $W/audio/Android.bp $W/audio/route $W/audio/overlay $W/audio/mixer_paths_a6l.xml $L/device/hisense/a6l/audio/
  cp $W/audio/audio_policy_configuration.xml $L/device/hisense/a6l/audio/realinit/audio_policy_configuration.xml
  # patched q6asm-dai.ko (audio3: DAI lookup by reg, fixes the swapped MultiMedia FEs on the V74 DT)
  cp $R/firmware/extracted/audio3-20260924/modules/q6asm-dai.ko $L/device/hisense/a6l/rom/prebuilt/vendor/lib/modules/q6asm-dai.ko
  mkdir -p $L/device/hisense/a6l/hals; cp -r $W/hals/wifi $L/device/hisense/a6l/hals/
  find $V $L/device/hisense/a6l/audio -type f \( -name '*.mk' -o -name '*.rc' -o -name '*.bp' -o -name '*.xml' -o -name '*.c' \) -exec sed -i 's/\r$//' {} +
fi
# GNSS HAL (agent gnss 24 Sep): QMI LOC AIDL HAL + a6l_gnss_test, inherited by rom.mk (inherit-product-if-exists)
rm -rf $L/device/hisense/a6l/gnss; cp -r $W/gnss $L/device/hisense/a6l/gnss
find $L/device/hisense/a6l/gnss -type f \( -name '*.mk' -o -name '*.rc' -o -name '*.bp' -o -name '*.xml' -o -name '*.cpp' -o -name '*.h' -o -name '*.te' -o -name 'file_contexts' \) -exec sed -i 's/\r$//' {} +
echo "variant=$VARIANT: $(ls $V | tr '\n' ' ')"
cp $W/BoardConfig.mk $W/lineage_gsi_a6l.mk $W/a6l-software-graphics.mk $W/manifest.xml $L/device/hisense/a6l/; sed -i 's/\r$//' $L/device/hisense/a6l/*.mk
echo "== $(date) build $WHAT"
TGT=vendorimage; [ "$WHAT" = system ] && TGT="systemimage vendorimage"
( cd $L && export A6L_SOONG_GOMEMLIMIT=36GiB && source build/envsetup.sh > /dev/null && source vendor/lineage/vars/aosp_target_release && lunch lineage_gsi_a6l "$aosp_target_release" userdebug > /dev/null 2>&1 && m -j12 $TGT ) > /home/a6l/rom-v1/build-$TAG.log 2>&1 || { grep -E "error:|FAILED:" /home/a6l/rom-v1/build-$TAG.log | head -20; echo PIPELINE_FAIL build; exit 1; }
tail -2 /home/a6l/rom-v1/build-$TAG.log; ls -la $L/out/target/product/a6l/*.img
O=$L/out/target/product/a6l/vendor
grep -h "dalvik.vm.heapgrowthlimit\|dalvik.vm.heapsize" $O/build.prop $O/etc/build.prop 2>/dev/null | head -3
ls -la $O/bin/hw/android.hardware.gnss-service.a6l $O/bin/a6l_gnss_test $O/etc/init/android.hardware.gnss-service.a6l.rc $O/etc/vintf/manifest/android.hardware.gnss-service.a6l.xml $O/etc/permissions/android.hardware.location.gps.xml $O/bin/a6l-logcat.sh 2>&1
echo "== $(date) boot"
rm -rf $B $B-qemu
python3 $R/tools/Prepare-RomV1Boot.py $B 2>&1 | tail -1
python3 $R/tools/Prepare-RomV1Boot.py $B-qemu --qemu $Q/virtio_mmio.ko $Q/virtio_blk.ko $R/firmware/extracted/phone-kernel-v67-candidate-20260919/a6l_simplefb.ko 2>&1 | tail -1
timeout 600 /home/a6l/venv-abl/bin/python $R/tools/Test-RomV1Abl.py $B 2>&1 | tail -1
echo "== $(date) kit"
sed -i 's/\r$//' $R/tools/Prepare-RomV1Stage.py; rm -rf /home/a6l/rom-v1/kit-$TAG
python3 $R/tools/Prepare-RomV1Stage.py /home/a6l/rom-v1/kit-$TAG/rom-v1 2>&1 | tail -1
cp -r $B /home/a6l/rom-v1/kit-$TAG/boot-build; echo "$VARIANT" > /home/a6l/rom-v1/kit-$TAG/VARIANT
echo "== $(date) qemu"
pkill -f qemu-system-aarch64; sleep 1; rm -rf /home/a6l/rom-v1/qemu-$TAG
python3 $R/tools/Test-RomV1Qemu.py /home/a6l/rom-v1/qemu-$TAG $B-qemu ${A6L_QEMU_MIN:-25} 2>&1 | tail -3
cat /home/a6l/rom-v1/qemu-$TAG/report.json | head -30
grep -a -E "A6L_ROM" /home/a6l/rom-v1/qemu-$TAG/console.log | tail -30
echo "== $(date) PIPELINE_DONE"
