#!/usr/bin/env bash
# rom-v2 (agent merge, 25 Sep 2026): V75 DTB = V74 base.dtb + the merged V75 overlays, for the ROM boot image. WSL only, < 30 s.
# usage: build-rom-v2-dt.sh <outdir>   -> <outdir>/rom-v2.dtb + dtbos + merge log + link checks
set -euo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
DTC=$O/scripts/dtc/dtc; KD=$R/device/hisense/a6l/kernel; OUT=$1; mkdir -p $OUT/dtbo
BASE=$R/firmware/extracted/recovery-v74-candidate-20260923/base.dtb
[ "$(sha256sum < $BASE | cut -c1-64)" = aceadcb790ded3f4b6da770263000eb2f6031f19fb8ff85d521a70346b98637e ] || { echo "V74 base sha mismatch"; exit 1; }
# order matters: audio/voice first (it relies on fdtoverlay prepending children of &sound, see kvoice doc §1.3)
OVLS="kvoice/dt/a6l-voice-speaker-onbase-v75 a6l-charger-v75 a6l-flash-v75 a6l-hall-v75 a6l-vibrator-v75 a6l-camera-v75
      a6l-eink-frontlight-v75 ipa/dt/a6l-ipa-v75 a6l-microsd-v75 a6l-stk3338-v75
      a6l-audio-mics-v75 a6l-watchdog-v75 a6l-ramoops-v75"
# btcall (29 Sep 2026, docs/btcall-kernel-20260929.md): A6L_AUDIO_SET=series swaps the voice overlay for its btcall superset
# (+ MultiMedia3/MultiMedia4 front ends, in-call record DL / in-call music back ends on AFE pseudo ports 153/155). ONLY together with
# the series audio modules (tools/stage-rom-v2-prebuilts.sh A6L_AUDIO_SET=series): the audio4 modules reject cpu ids >= 153 (no card).
# Default (unset = audio4): the DTB is unchanged.
case "${A6L_AUDIO_SET:-audio4}" in
  audio4) ;;
  series) OVLS=${OVLS/kvoice\/dt\/a6l-voice-speaker-onbase-v75/kvoice\/dt\/a6l-voice-speaker-btcall-onbase-v75}; echo "audio set: series (btcall voice overlay)";;
  *) echo "A6L_AUDIO_SET must be audio4 or series"; exit 2;;
esac
# r6b boot fix (30 Sep 2026): a6l-ramoops-v75 = pstore/ramoops on the stock recorder_mem reservation (0xb0180000, 4 MiB).
# cpufreq-watchdog (29 Sep 2026): a6l-watchdog-v75 = APSS watchdog@17817000 (qcom-wdt, H64); inert until watchdogd opens it.
cp $BASE $OUT/cur.dtb
for o in $OVLS; do
  n=$(basename $o); src=$KD/$o.dtso; tr -d '\r' < $src > $OUT/dtbo/$n.dtso
  cpp -nostdinc -undef -D__DTS__ -x assembler-with-cpp -I $K/include -I $K/arch/arm64/boot/dts $OUT/dtbo/$n.dtso -o $OUT/dtbo/$n.pp
  $DTC -@ -q -I dts -O dtb -o $OUT/dtbo/$n.dtbo $OUT/dtbo/$n.pp
  fdtoverlay -i $OUT/cur.dtb -o $OUT/next.dtb $OUT/dtbo/$n.dtbo || { echo "A6L_V75DT_MERGE_FAIL $n"; exit 2; }
  mv $OUT/next.dtb $OUT/cur.dtb; echo "merged $n $(sha256sum < $OUT/dtbo/$n.dtbo | cut -c1-16)"
done
# audio6 (25 Sep): a6l-audio-mics-v75 rewrites /sound audio-routing = base + "AMIC3","MIC BIAS External1" (secondary mic bias, stock).
# ROM fix-ups (documented in docs/merge-20260925.md):
#  - vibrator (misc2, 25 Sep): a6l-vibrator-v75 replaces a6l-haptics-v75: the motor is on TLMM GPIO79 (stock timed-gpio),
#    the PM660 haptics block is left disabled (stock kernel has no qpnp-haptic driver). No ilim fix-up needed any more.
#  - image marker (controls marker stays v71 so the attended bundles keep accepting this kernel)
fdtput -t s $OUT/cur.dtb /chosen hisense,a6l-image rom-v2
fdtput -t s $OUT/cur.dtb /chosen hisense,a6l-dt v75
mv $OUT/cur.dtb $OUT/rom-v2.dtb
$DTC -q -I dtb -O dts -o $OUT/rom-v2.dts $OUT/rom-v2.dtb
echo "== checks"
python3 $R/tools/check-voice-dt-links.py $OUT/rom-v2.dtb 2>&1 | tail -4
python3 $R/tools/check-audio-dt-links.py $OUT/rom-v2.dtb --rule reg --expect-tfa 2>&1 | tail -4
echo "front_als: $(fdtget $OUT/rom-v2.dtb $(fdtget $OUT/rom-v2.dtb /__symbols__ front_als) compatible) reg=$(fdtget -tx $OUT/rom-v2.dtb $(fdtget $OUT/rom-v2.dtb /__symbols__ front_als) reg)"
for p in /soc@0/mmc@c084000 /soc@0/spmi@800f000/pmic@0/charger@1000 /soc@0/spmi@800f000/pmic@3/led-controller@d300 /soc@0/spmi@800f000/pmic@1/vibrator@c000 /a6l-vibrator /a6l-epd-frontlight /hall-sensor; do
  echo "$p status=$(fdtget $OUT/rom-v2.dtb $p status 2>/dev/null || echo '(none=okay)')"; done
echo "watchdog: $(fdtget $OUT/rom-v2.dtb /soc@0/watchdog@17817000 compatible | tr '\n' ' ')clk=$(fdtget -tx $OUT/rom-v2.dtb /soc@0/watchdog@17817000 clocks) irq=$(fdtget -tx $OUT/rom-v2.dtb /soc@0/watchdog@17817000 interrupts) timeout-sec=$(fdtget $OUT/rom-v2.dtb /soc@0/watchdog@17817000 timeout-sec)"
echo "ramoops: $(fdtget $OUT/rom-v2.dtb /reserved-memory/memory@b0180000 compatible) reg=$(fdtget -tx $OUT/rom-v2.dtb /reserved-memory/memory@b0180000 reg) console=$(fdtget -tx $OUT/rom-v2.dtb /reserved-memory/memory@b0180000 console-size)"
echo "vibrator: $(fdtget $OUT/rom-v2.dtb /a6l-vibrator compatible) gpios=$(fdtget -tx $OUT/rom-v2.dtb /a6l-vibrator enable-gpios) chosen: $(fdtget $OUT/rom-v2.dtb /chosen hisense,a6l-image) $(fdtget $OUT/rom-v2.dtb /chosen hisense,a6l-voice) $(fdtget $OUT/rom-v2.dtb /chosen hisense,a6l-ipa) controls=$(fdtget $OUT/rom-v2.dtb /chosen hisense,a6l-controls)"
# r6e (1 Oct 2026, docs/rom-r6e-20261001.md): the eMMC (sdhc_1) must NOT use the command queue engine: sdhci-msm enables
# CQE only with `supports-cqe` on the node (sdhci_msm_probe -> sdhci_msm_cqe_add_host). V74 base + overlays never set it.
EM=/soc@0/mmc@c0c4000
if fdtget -p $OUT/rom-v2.dtb $EM | grep -qx 'supports-cqe'; then echo "A6L_DT_CHECK_FAIL $EM has supports-cqe"; exit 2; fi
echo "emmc: $EM supports-cqe absent (CQE off), max-frequency=$(fdtget $OUT/rom-v2.dtb $EM max-frequency) $(fdtget -p $OUT/rom-v2.dtb $EM | grep -E 'mmc-hs|cap-|no-' | tr '\n' ' ')"
ls -la $OUT/rom-v2.dtb; sha256sum $OUT/rom-v2.dtb
echo A6L_V75DT_BUILD_PASS
