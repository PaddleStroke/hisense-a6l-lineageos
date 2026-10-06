#!/usr/bin/env bash
# rom-v2 pipeline (agent merge, 25 Sep 2026). Run in the background (nohup), WSL, as the relay user.
# usage: rom-v2-pipeline.sh <tag> <phases...>   phases: prep build boot qemu flash   (e.g. "r1 prep build boot")
#   prep : V75 DT, stage prebuilts, sync device/hisense/a6l subtrees into the Lineage tree, composer patches, vendor/gapps link
#   build: m systemimage vendorimage (the only m in the tree: agent merge owns it)
#   boot : boot.img/dtbo.img (V67 Image + V75 DTB), captured-ABL emulation, kit (rom-v1 EDL tools + rom-v2 images)
#   qemu : first boot of the built system/vendor with the phone kernel in QEMU (virt), boot_completed + stability
#   flash: Test-RomV1Flash (virtual eMMC install/reinstall-refusal/restore/failure injection) against the kit
set -uo pipefail
TAG=$1; shift; PH=" $* "
R=/mnt/c/Users/Pierre/Desktop/A6L; L=/home/a6l/android/a6l-lineage24; W=/home/a6l/rom-v2; B=$W/boot-$TAG; Q=/home/a6l/rom-v1/qemu-modules
mkdir -p $W; D=$R/device/hisense/a6l; T=$L/device/hisense/a6l
crlf() { python3 "$R/tools/sync-rom-inputs.py" --normalize "$@"; }
fail() { echo "PIPELINE_FAIL $*"; exit 1; }
for f in stage-rom-v2-prebuilts.sh build-rom-v2-dt.sh Prepare-RomV2Boot.py Test-RomV2Abl.py Prepare-RomV2Stage.py Test-RomV1Qemu.py Test-RomV1Flash.py; do tr -d '\r' < $R/tools/$f > $W/$f; done
if [[ $PH == *" prep "* ]]; then
  echo "== $(date) prep: V75 DT"
  rm -rf $W/dt-$TAG; bash $W/build-rom-v2-dt.sh $W/dt-$TAG > $W/dt-$TAG.log 2>&1 || { tail -20 $W/dt-$TAG.log; fail dt; }
  grep -E "A6L_VOICE_DT_CHECK|A6L_DT_CHECK|A6L_V75DT_BUILD_PASS|^ilim" $W/dt-$TAG.log
  echo "== $(date) prep: stage prebuilts"
  bash $W/stage-rom-v2-prebuilts.sh > $W/stage-$TAG.log 2>&1 || { tail -25 $W/stage-$TAG.log; fail stage; }
  grep -E "MODULE_ORDER|^modules:|OVERRIDE|CONFLICT|STAGE_ROM_V2" $W/stage-$TAG.log | cut -c1-220
  echo "== $(date) prep: sync device tree"
  # merge r4 (28 Sep): + power (a6l-chg-guard + init.a6l-power.rc, referenced by rom.mk) and wifi (wifi-regdb.mk)
  # android-usb (29 Sep): + usb (gadget HAL, overlay, sepolicy; inherited by rom.mk, sepolicy dir listed in BoardConfig-rom.mk)
  # lc2 (29 Sep 2026): + camera (libcamera prebuilts + Android.bp, provider fork, sepolicy; inherited by rom.mk)
  # selinux-release (29 Sep 2026): + watchdog (r6.mk inherits device/hisense/a6l/watchdog/watchdog.mk; the tree had no copy)
  # Preserve unchanged source timestamps instead of deleting/recopying every subtree.
  for d in audio radio gnss kvoice hals eink power wifi usb camera watchdog; do python3 "$R/tools/sync-rom-inputs.py" "$D/$d" "$T/$d" || fail "sync $d"; done
  rm -rf $T/radio/tests/build* 2>/dev/null
  # merge r4: never part of a ROM build - unfinished/abandoned volte4 EFS/NV writer (docs/volte4-20260927.md: not wired,
  # not to be continued) and the pre-pinsafe CLI backup. Not compiled by radio/Android.bp anyway; removed from the TREE copy only.
  for x in radio/volte4 radio/tools/a6l_volte_cfg.cc radio/tools/a6l_qmi_cli.cc.pre-pinsafe; do rm -rf $T/$x && echo "not synced (never in the ROM): $x"; done
  # merge3: files of agents still in progress (e.g. volte2 IMS sources, picked up by the qmi/src/*.cc glob) stay out of this
  # build: A6L_PREP_EXCLUDE="radio/qmi/src/ims.cc ..." (paths relative to device/hisense/a6l)
  for x in ${A6L_PREP_EXCLUDE:-}; do rm -f $T/$x && echo "excluded (in progress, next build): $x"; done
  # the hals USB gadget fork is the AOSP example stub (no configfs/FunctionFS handling) and its Android.bp references a
  # license module not visible here: not part of rom-v2 (MTP not integrated, see docs/merge-20260925.md)
  rm -rf $T/hals/usb-gadget
  python3 "$R/tools/sync-rom-inputs.py" "$D" "$T" --files BoardConfig.mk lineage_gsi_a6l.mk a6l-software-graphics.mk manifest.xml Android.bp AndroidProducts.mk || fail "sync root inputs"
  mkdir -p $T/audio/realinit; cp $D/audio/audio_policy_configuration.xml $T/audio/realinit/audio_policy_configuration.xml
  mkdir -p $T/rom/variant
  [ -f $T/rom/variant/BoardConfig-variant.mk ] && [ ! -s $T/rom/variant/BoardConfig-variant.mk ] || : > $T/rom/variant/BoardConfig-variant.mk
  # merge2 (Astra R14): release by default; A6L_DEBUG_ADBKEY=1 builds the explicit debug variant (laptop adb key pre-trusted)
  if [ "${A6L_DEBUG_ADBKEY:-0}" = 1 ]; then echo '$(call inherit-product, device/hisense/a6l/rom/debug/adbkey.mk)' > $W/variant-$TAG.mk; echo "variant: DEBUG (adb key pre-trusted)"; else : > $W/variant-$TAG.mk; echo "variant: release (no adb key)"; fi
  cmp -s $W/variant-$TAG.mk $T/rom/variant/variant.mk || cp $W/variant-$TAG.mk $T/rom/variant/variant.mk
  crlf $T/audio $T/radio $T/gnss $T/kvoice $T/hals $T/eink $T/rom $T/power $T/wifi $T/usb $T/camera $T/watchdog $T/*.mk $T/*.bp $T/manifest.xml
  # release-prep (27 Sep 2026; docs/release-prep-20260927.md): A6L_FSTAB=fbe -> FBE /data (needs a kernel with
  # CONFIG_FS_ENCRYPTION=y); A6L_RELEASE=1 -> release board/product (recovery, OTA, gatekeeper, updater URI, rom sepolicy)
  # + prebuilts for the build-made boot/recovery images. Both unset = r3 behaviour.
  # bug hunt round2 install-tools (29 Sep 2026): FBE needs CONFIG_FS_ENCRYPTION=y in the kernel that boots this ROM (V67: unset)
  if [ "${A6L_FSTAB:-}" = fbe ]; then KC=$R/firmware/extracted/phone-kernel-v67-candidate-20260919/config; [ "${A6L_KERNEL:-v67}" = r5 ] && KC=$R/firmware/extracted/kernel-r5-20260930/config
    grep -qx 'CONFIG_FS_ENCRYPTION=y' $KC || fail "A6L_FSTAB=fbe with a kernel without CONFIG_FS_ENCRYPTION ($KC); use A6L_KERNEL=r5"; fi
  if [ "${A6L_FSTAB:-}" = fbe ]; then cp $T/rom/vendor-etc/fstab.qcom.fbe $T/rom/vendor-etc/fstab.qcom; echo "fstab: FBE (fstab.qcom.fbe)"; fi
  if [ "${A6L_RELEASE:-0}" = 1 ]; then
    [ "${A6L_DEBUG_ADBKEY:-0}" = 1 ] && fail "A6L_RELEASE=1 with A6L_DEBUG_ADBKEY=1"
    bash $R/tools/release/stage-release-prebuilts.sh $W/dt-$TAG > $W/stage-release-$TAG.log 2>&1 || { tail -20 $W/stage-release-$TAG.log; fail stage-release; }
    grep -E "keep|drop|STAGE_RELEASE" $W/stage-release-$TAG.log; echo "variant: RELEASE (A6L_RELEASE=1)"
  fi
  # selinux-release (29 Sep 2026; docs/selinux-release-20260929.md): A6L_SELINUX_PREP=1 (implied by A6L_RELEASE=1) rewrites the
  # TREE copy for enforcing readiness (no vendor_modprobe seclabels, debug logcat service userdebug-only, /dev/dri/card* 0660)
  # and marks it; BoardConfig-selinux.mk refuses the flag without the mark. Does NOT make the boot enforcing (A6L_SELINUX).
  rm -f $T/rom/selinux/.prep-applied
  if [ "${A6L_SELINUX_PREP:-0}" = 1 ] || [ "${A6L_RELEASE:-0}" = 1 ]; then
    python3 $R/tools/release/a6l_selinux_prep.py apply $T || fail "selinux prep"
    python3 $R/tools/release/a6l_selinux_prep.py services $T --aosp $L/system/sepolicy --prep --lineage $L/device/lineage/sepolicy/common/vendor | tail -1 || fail "selinux services"
  fi
  [ -f $T/eink/proprietary/lib64/libtcon_eink.so ] && [ -f $T/eink/proprietary/etc/epd-nor.bin ] || fail "eink blobs missing"
  echo "== $(date) prep: composer patches (eink3 0001 lease, dualux 0002 lcd-blank hold)"
  bash $T/eink/patches/apply-patches.sh $L || fail "eink 0001"
  P2=$(ls $T/eink/switcher/patches/external/drm_hwcomposer/0002-*.patch)
  ( cd $L/external/drm_hwcomposer && if git -c safe.directory='*' apply --reverse --check $P2 2>/dev/null; then echo "already applied: 0002"; else git -c safe.directory='*' apply --check $P2 && git -c safe.directory='*' apply $P2 && echo "applied: 0002"; fi ) || fail "dualux 0002"
  ( cd $L/external/drm_hwcomposer && git -c safe.directory='*' diff --stat | tail -1 )
  # merge3 (r3): audio HAL (hardware/interfaces StreamPrimary) opens capture on pcm ro.vendor.a6l.audio.capture_pcm (MultiMedia2)
  for P3 in $D/audio/patches/*.patch; do [ -e "$P3" ] || continue
    ( cd $L/hardware/interfaces && tr -d '\r' < $P3 > /tmp/a6l-audio.patch && if git -c safe.directory='*' apply --reverse --check /tmp/a6l-audio.patch 2>/dev/null; then echo "already applied: $(basename $P3)"; else git -c safe.directory='*' apply --check /tmp/a6l-audio.patch && git -c safe.directory='*' apply /tmp/a6l-audio.patch && echo "applied: $(basename $P3)"; fi ) || fail "audio patch $P3"; done
  # overlays-carrier-updater (29 Sep 2026): Lineage tree patches of the android-side config, rom/android/patches/<project>/*.patch
  # (project = path in the tree, e.g. vendor/apn: Orange France APN IPv4v6). Idempotent like the audio patches.
  for P4 in $(cd $D/rom/android/patches 2>/dev/null && find . -name '*.patch' | sort); do PRJ=$(dirname ${P4#./})
    ( cd $L/$PRJ && tr -d '\r' < $D/rom/android/patches/$P4 > /tmp/a6l-android.patch && if git -c safe.directory='*' apply --reverse --check /tmp/a6l-android.patch 2>/dev/null; then echo "already applied: $PRJ/$(basename $P4)"; else git -c safe.directory='*' apply --check /tmp/a6l-android.patch && git -c safe.directory='*' apply /tmp/a6l-android.patch && echo "applied: $PRJ/$(basename $P4)"; fi ) || fail "android patch $P4"; done
  echo "== $(date) prep: vendor/gapps"
  [ -d $W/src/vendor_gapps/arm64 ] || [ -d $L/vendor/gapps/arm64 ] || fail "MindTheGapps clone missing"
  # soong's finder does not follow directory symlinks: the clone must live INSIDE the tree (moved once, same filesystem)
  [ -L $L/vendor/gapps ] && rm -f $L/vendor/gapps
  [ -d $L/vendor/gapps/arm64 ] || mv $W/src/vendor_gapps $L/vendor/gapps
  ls $L/vendor/gapps/arm64/arm64-vendor.mk; git -C $L/vendor/gapps log -1 --format="gapps %H %ci" 2>/dev/null
  echo "PREP_DONE"
fi
if [[ $PH == *" build "* ]]; then
  echo "== $(date) build"
  pgrep -f "soong_ui|siso|ninja -d" > /dev/null && fail "another build is running"
  # release-prep: A6L_RELEASE=1 builds the signed-release inputs (variant user unless A6L_VARIANT, target-files + otatools)
  VARIANT=userdebug; TARGETS=${A6L_BUILD_TARGETS:-"systemimage vendorimage"}
  case "$TARGETS" in "systemimage vendorimage"|systemimage|vendorimage) ;; *) fail "A6L_BUILD_TARGETS must be systemimage, vendorimage, or systemimage vendorimage";; esac
  if [ "${A6L_RELEASE:-0}" = 1 ]; then VARIANT=${A6L_VARIANT:-user}; TARGETS="systemimage vendorimage target-files-package otatools"; fi
  echo "lunch variant=$VARIANT targets=$TARGETS A6L_RELEASE=${A6L_RELEASE:-0} A6L_SEPOLICY_ROM=${A6L_SEPOLICY_ROM:-0} A6L_SELINUX_PREP=${A6L_SELINUX_PREP:-0}"
  ( cd $L && export A6L_SOONG_GOMEMLIMIT=36GiB NINJA_HIGHMEM_NUM_JOBS=${A6L_HIGHMEM:-2} SOONG_NINJA=${A6L_NINJA:-ninja} && source build/envsetup.sh > /dev/null && source vendor/lineage/vars/aosp_target_release && lunch lineage_gsi_a6l "$aosp_target_release" $VARIANT > /dev/null 2>&1 && m -j${A6L_J:-12} $TARGETS ) > $W/build-$TAG.log 2>&1 || { grep -E "error:|FAILED:|ninja: build stopped|neverallow|ERROR" $W/build-$TAG.log | head -40; fail build; }
  tail -2 $W/build-$TAG.log; ls -la $L/out/target/product/a6l/*.img
  O=$L/out/target/product/a6l/vendor
  for f in bin/hw/android.hardware.radio-service.a6l bin/a6l-qmi bin/a6l-q6voiced bin/a6l-audio-route etc/mixer_paths_a6l.xml bin/hw/a6l_epdd bin/a6l_eink_mirror bin/a6l_dualux \
           bin/hw/vendor.qti.hardware.vibrator.service bin/hw/android.hardware.sensors-service.multihal lib64/hw/sensors.a6l.so lib64/android.hardware.sensors@2.0-subhal-impl-1.0.so \
           bin/hw/android.hardware.gnss-service.a6l bin/hw/android.hardware.wifi-service bin/hw/wpa_supplicant bin/hw/android.hardware.bluetooth-service.default \
           bin/hw/android.hardware.light-service.lineage bin/a6l_macs lib/modules/q6voice.ko lib/modules/snd-soc-tfa98xx.ko lib/modules/qcom-camss.ko firmware/tfa98xx.cnt \
           etc/fstab.qcom etc/a6l/modules/misc.txt etc/selinux/vendor_sepolicy.cil \
           bin/a6l-imsdcm bin/a6l-chg-guard.sh etc/init/init.a6l-power.rc firmware/regulatory.db firmware/regulatory.db.p7s lib/modules/ipa2_lite.ko lib/modules/qcom_smbx.ko \
           bin/hw/android.hardware.camera.provider-service.a6l lib64/hw/camera.libcamera.so lib64/libcamera.so lib64/libcamera/ipa/ipa_soft_simple.so.sign etc/libcamera/camera_hal.yaml; do [ -e $O/$f ] && echo "ok   $f" || echo "MISS $f"; done
  S=$L/out/target/product/a6l/system
  for f in system/priv-app/GmsCore system/product/priv-app/GmsCore product/priv-app/GmsCore product/priv-app/Phonesky system/product/priv-app/Phonesky system_ext/priv-app/A6LDisplaySwitcher system/system_ext/priv-app/A6LDisplaySwitcher; do [ -e $S/$f ] && echo "ok   system:$f"; done
  grep -h "ro.vendor.a6l.rom\|persist.vendor.a6l" $O/build.prop $O/etc/build.prop 2>/dev/null | head
  echo BUILD_DONE
fi
if [[ $PH == *" boot "* ]]; then
  echo "== $(date) boot"
  rm -rf $B $B-qemu
  A6L_V75_DTB=$W/dt-$TAG/rom-v2.dtb python3 $W/Prepare-RomV2Boot.py $B 2>&1 | tail -1
  # kernel r5 (30 Sep 2026): A6L_KERNEL=r5 (inherited by the stage script and Prepare-RomV2Boot.py) -> r5 QEMU modules too
  if [ "${A6L_KERNEL:-v67}" = r5 ]; then K5=$R/firmware/extracted/kernel-r5-20260930; QM="$K5/qemu-modules/virtio_mmio.ko $K5/qemu-modules/virtio_blk.ko $K5/modules/a6l_simplefb.ko"
  else QM="$Q/virtio_mmio.ko $Q/virtio_blk.ko $R/firmware/extracted/phone-kernel-v67-candidate-20260919/a6l_simplefb.ko"; fi
  A6L_V75_DTB=$W/dt-$TAG/rom-v2.dtb python3 $W/Prepare-RomV2Boot.py $B-qemu --qemu $QM 2>&1 | tail -1
  cp $W/Test-RomV2Abl.py $R/tools/Test-RomV2Abl.py 2>/dev/null; timeout 900 /home/a6l/venv-abl/bin/python $R/tools/Test-RomV2Abl.py $B 2>&1 | tail -1
  echo "== $(date) kit"
  rm -rf $W/boot; ln -s $B $W/boot; rm -rf $W/kit-$TAG
  python3 $W/Prepare-RomV2Stage.py $W/kit-$TAG/rom-v2 2>&1 | tail -1
  cp -r $B $W/kit-$TAG/boot-build; cp $W/dt-$TAG/rom-v2.dtb $W/dt-$TAG/rom-v2.dts $W/kit-$TAG/
  ls -la $W/kit-$TAG/rom-v2/images; cat $W/kit-$TAG/rom-v2/images/SHA256SUMS
fi
if [[ $PH == *" flash "* ]]; then
  echo "== $(date) flash test (virtual eMMC)"
  rm -rf $W/flashtest-$TAG; mkdir -p $W/flashtest-$TAG
  ( cd $R/tools && timeout 3000 python3 $R/tools/Test-RomV1Flash.py $W/flashtest-$TAG $W/kit-$TAG/rom-v2/images ) > $W/flashtest-$TAG.log 2>&1; echo "flash rc=$?"
  tail -15 $W/flashtest-$TAG.log
fi
if [[ $PH == *" qemu "* ]]; then
  echo "== $(date) qemu"
  pkill -f qemu-system-aarch64; sleep 1; rm -rf $W/qemu-$TAG
  python3 $W/Test-RomV1Qemu.py $W/qemu-$TAG $B-qemu ${A6L_QEMU_MIN:-45} 2>&1 | tail -3
  head -40 $W/qemu-$TAG/report.json
  grep -a -E "A6L_ROM" $W/qemu-$TAG/console.log | tail -40
fi
echo "== $(date) PIPELINE_DONE $TAG"
