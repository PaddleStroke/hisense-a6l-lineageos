#!/usr/bin/env bash
# Stage the rom-v2 vendor prebuilts into the Lineage tree (agent merge, 25 Sep 2026; derived from stage-rom-v1-prebuilts.sh).
# WSL only. No build, no phone. rom-v1 sources (payloads proven from RAM, /home/a6l/rom-v1/src) + the 24-25 Sep module sets:
#   audio4 (kvoice q6voice stack + audfix per-direction q6routing), audio3 speaker (patched LPI pinctrl, snd-soc-tfa98xx,
#   tfa98xx.cnt), camera3 (camfix+camfix2 qcom-camss, CCI, sensors, v4l2 deps), rest2 haptics (misc fix), flash LED,
#   dualux frontlight (leds-qcom-lpg, leds-pwm), ipa2b (ipa2_lite, rmnet), v67 fuel gauge + charger, eink3 blobs,
#   stk3338_a6l (front light/prox, merge2). The adb key goes to rom/debug/ only (debug variant).
# Output: $L/device/hisense/a6l/rom/{prebuilt/vendor/..., rom-files.mk}, /home/a6l/rom-v2/prebuilt-manifest.txt
# merge r4 (28 Sep 2026): DRY RUN: A6L_STAGE_DRYRUN=<scratch dir> stages into <dir>/device/hisense/a6l/rom and writes the
# manifest to <dir>/prebuilt-manifest.txt - the Lineage tree and /home/a6l/rom-v2 are not touched (same checks run).
set -euo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; L=/home/a6l/android/a6l-lineage24; S=/home/a6l/rom-v1/src; X=$R/firmware/extracted
O67=/home/a6l/kernel/out-a6l-phone-v67
T=$L/device/hisense/a6l/rom; P=$T/prebuilt/vendor; MAN=/home/a6l/rom-v2/prebuilt-manifest.txt
if [ -n "${A6L_STAGE_DRYRUN:-}" ]; then
  case "$A6L_STAGE_DRYRUN" in /tmp/*|/home/a6l/scratch/*) ;; *) echo "A6L_STAGE_DRYRUN must be under /tmp or /home/a6l/scratch"; exit 2;; esac
  T=$A6L_STAGE_DRYRUN/device/hisense/a6l/rom; P=$T/prebuilt/vendor; MAN=$A6L_STAGE_DRYRUN/prebuilt-manifest.txt; echo "DRY RUN into $T"
fi
P72=/home/a6l/flash-scratch/p72/root/vendor
SYNC_STAGE=()
[ -z "${A6L_STAGE_DRYRUN:-}" ] || SYNC_STAGE=(--staging)
python3 "$R/tools/sync-rom-inputs.py" "$R/device/hisense/a6l/rom" "$T" --preserve prebuilt --preserve variant --preserve rom-files.mk --preserve conflicts.txt "${SYNC_STAGE[@]}" || exit 1
# Assemble the priority/override set separately, then compare final bytes.
# This keeps unchanged build inputs intact, including their timestamps.
FINAL_P=$P
STAGE_TMP=$(mktemp -d /tmp/a6l-stage-prebuilt.XXXXXX)
cleanup_stage() { case "$STAGE_TMP" in /tmp/a6l-stage-prebuilt.*) rm -rf -- "$STAGE_TMP";; esac; }
trap cleanup_stage EXIT
P=$STAGE_TMP/vendor
mkdir -p $P/lib/modules $P/firmware $P/etc/a6l/modules $P/a6l/radio/bin $P/a6l/epd $P/a6l/tools $P/lib64/egl
# sync-rom-inputs normalises source text before comparing; no blanket sed rewrite.
: > $T/conflicts.txt
addko() { # file (first copy wins; identical duplicates fine; different bytes recorded)
  local f=$1 n; n=$(basename $f)
  if [ -e $P/lib/modules/$n ]; then cmp -s $f $P/lib/modules/$n || echo "CONFLICT $n kept=$(sha256sum < $P/lib/modules/$n | cut -c1-16) ignored=$(sha256sum < $f | cut -c1-16) from $f" >> $T/conflicts.txt
  else cp $f $P/lib/modules/$n; fi; }
setko() { # file [name]: explicit v2 override (newer build of the same module)
  local f=$1 n=${2:-$(basename $1)}
  [ -e $P/lib/modules/$n ] && ! cmp -s $f $P/lib/modules/$n && echo "OVERRIDE $n $(sha256sum < $P/lib/modules/$n | cut -c1-16) -> $(sha256sum < $f | cut -c1-16) from $f" >> $T/conflicts.txt
  cp $f $P/lib/modules/$n; }
# --- rom-v1 base (priority: display > touch > sensors-adsp > adsp > audio2 > als2 > radio2 > radio2 bt) ---
for f in $S/v74_bundle-v74/modules/*.ko $S/v71_bundle_touch/modules/*.ko $S/v71_bundle_sensors-adsp/modules/*.ko $S/v68_bundle_adsp/modules/*.ko \
         $S/v74_audio2/modules/*.ko $S/v74_als2/modules/*.ko $S/v74_radio2/modules/*.ko $S/v74_radio2/modules-bt/*.ko; do addko $f; done
cp $R/firmware/extracted/phone-kernel-v67-candidate-20260919/a6l_simplefb.ko $P/lib/modules/
# --- rom-v2 overrides / additions ---
cp $P/lib/modules/q6routing.ko $P/lib/modules/q6routing-upstream.ko          # fallback = the 24 Sep call-test q6routing
cmp -s $P/lib/modules/q6routing-upstream.ko $X/kvoice-20260924/v75/kvoice/modules/q6routing.ko || setko $X/kvoice-20260924/v75/kvoice/modules/q6routing.ko q6routing-upstream.ko
for f in $X/audio4-20260924/v75/audio4/modules/*.ko; do setko $f; done       # q6voice stack + per-direction q6routing + xlate q6asm-dai
setko $X/audio5-20260925/modules/q6adm.ko                                     # misc2: ADM open endpoint_id_2 = 0xFFFF (stock parity; capture suspect)
setko $X/audio3-20260924/v75/audio3/extra/pinctrl-sdm660-lpass-lpi.ko         # TERT MI2S pins for the TFA9894
setko $X/audio3-20260924/v75/audio3/extra/snd-soc-tfa98xx.ko
for f in $X/camera-20260927-camfix5/*.ko; do case $f in */mc.ko) addko $f;; *) setko $f;; esac; done  # merge r4: camera6 set = camfix5 cumulative qcom-camss f9ce7705 (camfix1-5: WM MAX regs + CGC, VFE clock >= 404 MHz a6l_vfe_min); all other .ko byte-identical to camera-20260925b; mc.ko = the proven audio one
# camera-rom (29 Sep 2026, docs/camera-rom-20260929.md): qcom-camss = rom1 (series camss-sdm660-rom1.patch: camfix5 functional
# fixes + stock CSIPHY digital clocks cphy_csidK 200 MHz / csiK 310 MHz = the t35 attended fix, no diagnostics, a6l_wm default 6).
# Sensors/CCI/VCM stay the camfix2 builds above (t35 ran camfix11 builds whose only additions are default-off/read-only knobs).
CR1=$X/camera-rom1-20260929; ( cd $CR1 && sha256sum -c --quiet SHA256SUMS ) || { echo "camera-rom1 SHA256SUMS mismatch"; exit 1; }
setko $CR1/v67/qcom-camss.ko
# hi846 merge (29 Sep 2026, docs/hi846-20260929.md round 3): hi846.ko = mainline + hi846-set-ctrl-fix.patch (s_ctrl returned 1 ->
# the sensor never streamed; attended t38 PASS with the fix + 2-lane DT). No 4-lane patch (ROM DT a6l-camera-v75 is now 2-lane).
H846=$X/hi846-rom-20260929; ( cd $H846 && sha256sum -c --quiet SHA256SUMS ) || { echo "hi846-rom SHA256SUMS mismatch"; exit 1; }
setko $H846/v67/hi846.ko
setko $X/stk-20260929/stk3338_a6l.ko                                           # front STK3338 light/prox (merge2); r5 review fix F3 (29 Sep): wake-capable proximity suspend, f2320b68 - NOT yet suspend-tested on the phone
setko $X/vib49-20260929/a6l_gpio_vib.ko                                       # misc2: vibrator = TLMM GPIO79 (stock timed-gpio), replaces a6l_pm660_haptics; r5 review fix F49 (29 Sep): + LED class 'vibrator' for VibratorOL (8d3003b3) - NOT yet phone-tested
setko $X/dualux2-20260925/leds-pwm.ko; setko $X/dualux2-20260925/leds-qcom-lpg.ko; setko $X/dualux2-20260925/led-class-multicolor.ko   # merge3 r3: the set proven by dualux2 (25 Sep evening, frontlight lit + steps)
setko $X/ipa-20260925b/ipa4/modules/ipa2_lite.ko; addko $X/ipa-20260924/ipa2b/modules/rmnet.ko   # merge r4: ipa4 build (data3: QMI filter-rule replies, route index 7; 11f20f0f, ping/DNS/HTTP proven 27 Sep); rmnet unchanged (= ipa4 bundle c40313f3)
# power (26 Sep): + qcom-spmi-rradc.ko, the IIO provider qcom_smbx needs (charger group)
# thermal follow-up (29 Sep 2026, docs/android-thermal-20260929.md): + PM660 VADC adc5 + its vadc-common (misc group; r5 builds
# added to kernel-r5-20260930/modules from out-a6l-rom-r5/modinst, strip-debug, CRCs checked)
for m in drivers/power/supply/pmi8998_fg.ko drivers/iio/adc/qcom-spmi-rradc.ko drivers/power/supply/qcom_smbx.ko drivers/leds/led-class-multicolor.ko drivers/iio/adc/qcom-vadc-common.ko drivers/iio/adc/qcom-spmi-adc5.ko; do f=$O67/modinst/$m; [ -e $f ] || f=$O67/$m; addko $f; done
# r5 bug hunt boot-init (29 Sep): base group (rom/modules/base.txt) = Android-required features the V67 Image has as modules:
# fuse (CONFIG_FUSE_FS=m: vold /dev/fuse for /storage/emulated + microSD), uhid (CONFIG_UHID=m: Bluetooth HID host)
# taken from the module set released WITH the ROM's V67 Image (candidate modules.tar.gz, sums in modules-SHA256SUMS)
K67=$R/firmware/extracted/phone-kernel-v67-candidate-20260919; KT=$(mktemp -d)
tar xzf $K67/modules.tar.gz -C $KT ./fs/fuse/fuse.ko ./drivers/hid/uhid.ko
( cd $KT && grep -E ' \./(fs/fuse/fuse|drivers/hid/uhid)\.ko$' $K67/modules-SHA256SUMS | sha256sum -c --quiet - )
for m in fs/fuse/fuse.ko drivers/hid/uhid.ko; do addko $KT/$m; done; rm -rf "$KT"
# pwr27 (27 Sep): A6L-patched qcom_smbx (fcc_max_ua / jeita_hard / float clamp 4.40 V), btqca (MSFT off)
# merge r4 (28 Sep): qcom_smbx rebuilt with the stock FCC upper bound 2.4 A (merge-20260928, tools/build-merge-r4.sh; ROM passes
# fcc_max_ua=2400000 in charger.txt). MBHC analog codec (fixes-20260927/mbhc) EXCLUDED: does not work (rework in progress) ->
# the ROM keeps the audio4 snd-soc-msm8916-analog.ko (0c5261a5, main mic proven 27 Sep). TODO in the ledger.
setko $X/merge-20260928/qcom_smbx.ko; setko $X/fixes-20260927/bluetooth/btqca.ko   # + btqca without the MSFT extension
# merge r5 (29 Sep): MBHC v2 analog codec (audfix/a6l-wcd-analog-mbhc-mic2-v75.patch, d2671b89) replaces the audio4 codec:
# attended 29 Sep PASS (A6L_MBHC_HEADSET, Mic Jack On, headset-mic capture OK, button press/release no false removal).
setko $X/mbhc-20260928/mbhc/snd-soc-msm8916-analog.ko
# r5 bug hunt round2 kernel-drivers (29 Sep 2026, ledger): rebuilds of three modules with defect fixes, v67 here and r5 below:
# panel-a6l-epd-dsi (sysfs attrs via dev_groups: no NULL deref after a failed DSI attach), ipa2_lite (INIT_COMPLETE success
# log only on success), snd-soc-msm8916-analog (= MBHC v2 + irqs/detection work stopped when the card is unbound). NOT yet phone-tested.
BH2=$X/bh2k-20260929; ( cd $BH2 && sha256sum -c --quiet SHA256SUMS ) || { echo "bh2k SHA256SUMS mismatch"; exit 1; }
for f in $BH2/v67/*.ko; do setko $f; done
# fastcharge-rom (29 Sep 2026, docs/fastcharge-rom-20260929.md): qcom_smbx = fcc-jeita + hvdcp (power29, attended QC PASS 9.06 V,
# + r5 bug hunt P4/P5) replaces merge-20260928 (v67 here, r5 below); charger.txt turns hvdcp_enable=1 on (stock 9 V / 2 A ICL).
FC=$X/fastcharge-rom-20260929; ( cd $FC && sha256sum -c --quiet SHA256SUMS ) || { echo "fastcharge-rom SHA256SUMS mismatch"; exit 1; }
setko $FC/v67/qcom_smbx.ko
rm -f $P/lib/modules/qcom-spmi-haptics.ko $P/lib/modules/a6l_pm660_haptics.ko   # PM660 haptics block is not the A6L motor (misc2)
# kernel r5 (30 Sep 2026, docs/kernel-android-config-20260930.md): A6L_KERNEL=r5 replaces EVERY staged .ko by its rebuild against
# the r5 Android-config kernel (firmware/extracted/kernel-r5-20260930, Image de4970b3 (thermal rebuild of 5f92e057, same modules); MODVERSIONS=y, so V67 modules are refused
# by the r5 kernel and r5 modules by the V67 kernel - never mix). Default (unset / v67) = the proven V67 set, unchanged.
A6L_KERNEL=${A6L_KERNEL:-v67}
case "$A6L_KERNEL" in
  v67) VERMAGIC="vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload aarch64";;
  r5) VERMAGIC="vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64"; K5=$X/kernel-r5-20260930
      ( cd $K5 && sha256sum -c --quiet SHA256SUMS ) || { echo "kernel-r5 SHA256SUMS mismatch"; exit 1; }
      for k in $P/lib/modules/*.ko; do n=$(basename $k); [ -e $K5/modules/$n ] || { echo "R5 MISSING $n"; exit 1; }; cp $K5/modules/$n $k; done
      [ "$(ls $K5/modules | wc -l)" = "$(ls $P/lib/modules | wc -l)" ] || { echo "R5 module set size differs from the staged set"; exit 1; }
      cp $CR1/r5/qcom-camss.ko $P/lib/modules/qcom-camss.ko   # camera-rom: rom1 camss built against out-a6l-rom-r5 (CRCs checked)
      cp $H846/r5/hi846.ko $P/lib/modules/hi846.ko             # hi846 merge: s_ctrl fix, built against out-a6l-rom-r5 (CRCs checked)
      for f in $BH2/r5/*.ko; do cp $f $P/lib/modules/$(basename $f); done   # bug hunt round2 kernel-drivers: r5 rebuilds (CRCs checked)
      cp $FC/r5/qcom_smbx.ko $P/lib/modules/qcom_smbx.ko       # fastcharge-rom: fcc-jeita + hvdcp built against out-a6l-rom-r5 (CRCs checked)
      echo "kernel r5: $(ls $P/lib/modules | wc -l) modules replaced by the r5 rebuilds";;
  *) echo "A6L_KERNEL must be v67 or r5"; exit 2;;
esac
# Optional reviewed MSM diagnostic module, rebuilt against the r5 kernel's
# unchanged export CRCs. Retained kernel payloads are never overwritten.
if [ -n "${A6L_MSM_PAYLOAD:-}" ]; then
  [ "$A6L_KERNEL" = r5 ] || { echo "MSM override requires the r5 kernel"; exit 1; }
  [[ ${A6L_MSM_SHA256:-} =~ ^[0-9a-f]{64}$ ]] || { echo "MSM override needs an explicit SHA256 pin"; exit 1; }
  printf '%s  %s\n' "$A6L_MSM_SHA256" "$A6L_MSM_PAYLOAD" | sha256sum -c --quiet || { echo "MSM override hash differs"; exit 1; }
  setko "$A6L_MSM_PAYLOAD" msm.ko
fi
# kernel-gaps (29 Sep 2026, docs/kernel-gaps-20260929.md): out-of-tree ports of ACK-only features, built for BOTH kernels after the
# r5 replacement (so the r5 set-size check above is unchanged): xt_quota2 (netd data limits/alerts) + uid_sys_stats (per-UID
# cputime/io for BatteryStats/storaged), loaded by base.txt. dm-default-key (r5 only) is NOT staged (FBE+metadata trial).
KG=$X/kernel-gaps-20260929; ( cd $KG && sha256sum -c --quiet SHA256SUMS ) || { echo "kernel-gaps SHA256SUMS mismatch"; exit 1; }
for m in xt_quota2 uid_sys_stats; do cp $KG/$A6L_KERNEL/$m.ko $P/lib/modules/$m.ko; done
# btcall (29 Sep 2026, docs/btcall-kernel-20260929.md): A6L_AUDIO_SET=series stages the q6voice SERIES audio set instead of audio4:
# every staged sound/soc/qcom module rebuilt together from rom-v2/series incl. kvoice/a6l-btcall-incall-v75.patch (AFE pseudo ports,
# q6cvs in-call record/music; tools/build-btcall-series-modules.sh, V67 + r5 builds, r5 import CRCs checked). Needs the matching DT
# (tools/build-rom-v2-dt.sh with A6L_AUDIO_SET=series). Default (unset = audio4): the proven call set, unchanged.
A6L_AUDIO_SET=${A6L_AUDIO_SET:-audio4}
case "$A6L_AUDIO_SET" in
  audio4) ;;
  series) BT=$X/btcall-20260929; ( cd $BT && sha256sum -c --quiet SHA256SUMS ) || { echo "btcall SHA256SUMS mismatch"; exit 1; }
    for f in $BT/$A6L_KERNEL/*.ko; do n=$(basename $f); [ -e $P/lib/modules/$n ] || { echo "SERIES $n is not in the staged set"; exit 1; }; setko $f; done
    echo "audio set: series ($(ls $BT/$A6L_KERNEL | wc -l) modules, $BT/$A6L_KERNEL)";;
  *) echo "A6L_AUDIO_SET must be audio4 or series"; exit 2;;
esac
# cpufreq-watchdog (29 Sep 2026, docs/cpufreq-watchdog-20260929.md, H64): APSS watchdog driver qcom-wdt (in-tree source, built
# out-of-tree W=1 against each kernel; r5 import CRCs checked), loaded by base.txt; binds the rom-v2 DT watchdog@17817000. Inert
# until watchdogd opens /dev/watchdog (init.a6l-watchdog.rc, opt-in persist.vendor.a6l.watchdog=1). QEMU-tested, not phone-tested.
WG=$X/wdt-20260929; ( cd $WG && sha256sum -c --quiet SHA256SUMS ) || { echo "wdt SHA256SUMS mismatch"; exit 1; }
cp $WG/$A6L_KERNEL/qcom-wdt.ko $P/lib/modules/qcom-wdt.ko
# venus-impl (6 Oct 2026, firmware/extracted/venus-impl-20261006): Venus video codec modules for the r5 kernel only
# (stagec: venus-core/enc/dec = hfi3 prod SDM660 fixes built against out-a6l-rom-r5; v4l2-mem2mem/videobuf2-dma-contig =
# out-a6l-rom-r5/modinst; every import CRC checked vs Module.symvers; Stage B passed 7 Oct 2026 with these sources),
# loaded by rom/modules/video.txt (+ video-dec.txt) from a6l-modules.sh misc. v67: both lists are skipped below.
VI=$X/venus-impl-20261006/stagec; ( cd $VI/modules && sha256sum -c --quiet SHA256SUMS ) || { echo "venus modules SHA256SUMS mismatch"; exit 1; }
if [ "$A6L_KERNEL" = r5 ]; then for m in v4l2-mem2mem videobuf2-dma-contig venus-core venus-enc venus-dec; do cp $VI/modules/$m.ko $P/lib/modules/$m.ko; done; fi
# audio-silent-20261008 (8 Oct 2026, firmware/extracted/audio-silent-20261008/fix): the LPASS LPI TLMM loses its pad
# configuration across system suspend (gpio4-7 ter_mi2s 0xd0 -> 0x2ca/0xca after one s2idle: silent speaker). r5 rebuilds
# of pinctrl-lpass-lpi + pinctrl-sdm660-lpass-lpi = in-tree sources (a6l ter_mi2s included; base build == kernel-r5 set in
# every allocated section) + a6l-lpi-resume-restore.patch (shadow of programmed registers, restored at resume_early/resume).
AS=$X/audio-silent-20261008/fix; ( cd $AS && sha256sum -c --quiet SHA256SUMS ) || { echo "audio-silent fix SHA256SUMS mismatch"; exit 1; }
if [ "$A6L_KERNEL" = r5 ]; then for m in pinctrl-lpass-lpi pinctrl-sdm660-lpass-lpi; do cp $AS/modules/$m.ko $P/lib/modules/$m.ko; done; fi
# --- module lists: every listed module must exist; dependency order across the boot sequence ---
for l in $T/modules/*.txt; do [ "$A6L_KERNEL" != r5 ] && case "$(basename $l)" in video.txt|video-dec.txt) true;; *) false;; esac && continue; cp $l $P/etc/a6l/modules/; grep -v '^#' $l | awk 'NF{print $1}' | while read -r k; do [ -e $P/lib/modules/$k ] || { echo "MISSING $k in $(basename $l)"; exit 1; }; done; done
[ -e $P/lib/modules/q6routing-upstream.ko ]
python3 - $P/lib/modules $T/modules <<'PY'
import sys, os
M, Ld = sys.argv[1], sys.argv[2]
def modname(ko): return os.path.basename(ko)[:-3].replace('-', '_')
def info(ko):
    b = open(os.path.join(M, ko), 'rb').read(); d = {}
    for k in (b'depends=', b'name='):
        i = b.find(k)
        while i >= 0:
            if b[i-1] == 0:
                d[k.decode()[:-1]] = b[i+len(k):b.index(b'\0', i)].decode(); break
            i = b.find(k, i+1)
    return d
loaded = set(); bad = 0
# boot sequence: ramdisk sdhci-msm, base + display, adsp, audio, misc, charger(opt), radio: qrtr + ipa(opt) + radio, bt
seq = ['base', 'display', 'adsp', 'audio', 'misc', 'camera', 'charger', 'ipa', 'radio', 'bt']
loaded.add('sdhci_msm'); loaded.add('a6l_simplefb')
for g in seq:
    if g == 'ipa': loaded |= {'qrtr', 'qrtr_smd'}
    for line in open(os.path.join(Ld, g + '.txt')):
        line = line.split('#')[0].split()
        if not line: continue
        ko = line[0]; i = info(ko)
        deps = [x for x in i.get('depends', '').split(',') if x]
        miss = [x for x in deps if x.replace('-', '_') not in loaded]
        if miss: print(f'DEPORDER {g}:{ko} needs {miss} not loaded before'); bad += 1
        loaded.add(i.get('name', modname(ko)).replace('-', '_'))
if g == 'bt' and 'tmd3702' not in loaded: pass
print('MODULE_ORDER', 'PASS' if not bad else 'FAIL', 'loaded', len(loaded))
sys.exit(1 if bad else 0)
PY
bad=0; for k in $P/lib/modules/*.ko; do v=$(strings $k | grep '^vermagic=' | sed -n 1p || true); [ "$v" = "$VERMAGIC" ] || { echo "VERMAGIC $k $v"; bad=1; }; done; [ $bad = 0 ]
# --- firmware ---
cp -r $S/v74_bundle-v74/firmware/. $P/firmware/
cp -r $S/v74_radio2/firmware/. $P/firmware/
mkdir -p $P/firmware/qcom/hisense/a6l $P/firmware/qcom/sensors
cp $S/v68_bundle_adsp/firmware/adsp.* $P/firmware/qcom/hisense/a6l/
cp $S/v71_bundle_sensors-adsp/firmware/qcom/sensors/sns.reg $P/firmware/qcom/sensors/
cp $X/audio3-20260924/v75/audio3/firmware/tfa98xx.cnt $P/firmware/tfa98xx.cnt      # TFA9894 container (stock)
# venus-impl: stock Venus firmware VIDEO.VE.4.4-00060 (Hisense-signed, PAS 9; segment hashes verified against the signed
# hash table by venus-impl-20261006/tools/fw-verify.py), lowercase as qcom_mdt_load expects (qcom/venus-4.4/venus.mdt)
( cd $VI/firmware && sha256sum -c --quiet SHA256SUMS ) || { echo "venus firmware SHA256SUMS mismatch"; exit 1; }
mkdir -p $P/firmware/qcom/venus-4.4; cp $VI/firmware/venus.mdt $VI/firmware/venus.b0[0-4] $P/firmware/qcom/venus-4.4/
# --- radio userspace, e-ink manual payload, tools, Mesa, adb key (as rom-v1) ---
cp $S/v74_radio2/bin/* $P/a6l/radio/bin/
cp -r $S/v74_bundle-v74/epd/. $P/a6l/epd/
cp $S/v74_eink-mirror/a6l_epdd_v3 $S/v74_eink-mirror/a6l_eink_mirror $S/v74_eink-mirror/eink-mirror-session.sh $P/a6l/epd/
cp $S/v74_eink3/a6l_lease_probe $S/v74_eink3/eink3-session.sh $P/a6l/epd/ 2>/dev/null || true
cp $S/v74_bundle-v74/bin/* $P/a6l/tools/; cp $S/v74_audio2/bin/* $P/a6l/tools/
cp -r $S/v74_audio2/mixer2 $S/v74_audio2/wav $P/a6l/tools/
cp $X/merge-20260928/a6l-qmi-static $P/a6l/tools/a6l-qmi-static   # merge r4: current CLI (pinsafe PIN guard + provision1); was ril-20260924b
cp $X/gnss3-20260925/a6l_gnss_test $P/a6l/tools/a6l_gnss_test-static
cp $X/camera-20260927-camfix5/a6l_camcap $P/a6l/tools/   # merge r4: camera6 capture tool (camfix5 bundle)
cp $P72/lib64/egl/libEGL_mesa.so $P72/lib64/egl/libGLESv1_CM_mesa.so $P72/lib64/egl/libGLESv2_mesa.so $P/lib64/egl/
cp $P72/lib64/libgallium_dri.so $P/lib64/
# r6d (30 Sep 2026, docs/rom-r6d-20260930.md): the 32-bit Mesa set (same Mesa 26.1.0-devel source copy, NDK r27c
# armv7a API 34, tools/build-mesa-freedreno.sh A6L_MESA_ARCH=arm). Without it zygote_secondary (app_process32) aborted in
# ZygoteInit.preload -> eglGetDisplay: Loader takes the FIRST set property (persist.graphics.egl=mesa) and aborts when
# /vendor/lib/egl/libEGL_mesa.so is missing (no fallback to ro.hardware.egl) -> zygote restart loop, r6c never booted.
M32=$X/mesa-arm32-r6d-20260930; ( cd $M32 && sha256sum -c --quiet SHA256SUMS ) || { echo "mesa arm32 SHA256SUMS mismatch"; exit 1; }
mkdir -p $P/lib/egl; cp $M32/lib/egl/libEGL_mesa.so $M32/lib/egl/libGLESv1_CM_mesa.so $M32/lib/egl/libGLESv2_mesa.so $P/lib/egl/
cp $M32/lib/libgallium_dri.so $P/lib/
# Optional reviewed Mesa backport payload, same Android names and both ABIs.
# Default retains the original libraries; r6g selects the pinned payload.
if [ -n "${A6L_MESA_PAYLOAD:-}" ]; then
  ( cd "$A6L_MESA_PAYLOAD" && sha256sum -c --quiet SHA256SUMS ) || { echo "Mesa backport hashes differ"; exit 1; }
  for arch in lib lib64; do
    cp "$A6L_MESA_PAYLOAD/$arch/libgallium_dri.so" "$P/$arch/"
    for name in libEGL_mesa.so libGLESv1_CM_mesa.so libGLESv2_mesa.so; do
      cp "$A6L_MESA_PAYLOAD/$arch/egl/$name" "$P/$arch/egl/"
    done
  done
  echo "Mesa reviewed backports staged: $A6L_MESA_PAYLOAD"
fi
for f in $P/lib/egl/libEGL_mesa.so $P/lib/libgallium_dri.so; do readelf -h $f | grep -q "Class:.*ELF32" || { echo "not ELF32: $f"; exit 1; }; done
# merge2 (Astra R14): the laptop adb key is NOT part of the vendor payload; only the explicit debug variant (rom/debug/adbkey.mk) ships it
mkdir -p $T/debug
cmp -s $R/firmware/extracted/laptop-adb-public-key.txt $T/debug/adb_keys || cp $R/firmware/extracted/laptop-adb-public-key.txt $T/debug/adb_keys
mkdir -p $P/bin; cp $X/hals-20260924/bin/a6l_macs $P/bin/a6l_macs
# --- rom-files.mk ---
( cd $P && find . -type f | sed 's#^\./##' | sort ) | awk -v D=device/hisense/a6l/rom/prebuilt/vendor '
  BEGIN{print "# generated by tools/stage-rom-v2-prebuilts.sh - do not edit"; print "PRODUCT_COPY_FILES += \\"}
  {printf "    %s/%s:$(TARGET_COPY_OUT_VENDOR)/%s \\\n", D, $0, $0} END{print ""}' > $STAGE_TMP/rom-files.mk
python3 - "$R/tools/sync-rom-inputs.py" "$P" "$FINAL_P" <<'PY'
import importlib.util, pathlib, sys
spec = importlib.util.spec_from_file_location('a6l_input_sync', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
source, target = map(pathlib.Path, sys.argv[2:])
if source.name != 'vendor' or not source.parent.name.startswith('a6l-stage-prebuilt.') or source.parent.parent != pathlib.Path('/tmp'):
    raise SystemExit('Unexpected generated prebuilt source')
if tuple(target.parts[-6:]) != ('device', 'hisense', 'a6l', 'rom', 'prebuilt', 'vendor'):
    raise SystemExit('Unexpected generated prebuilt destination')
print('Generated vendor input entries changed:', module.sync(source, target))
PY
cmp -s $STAGE_TMP/rom-files.mk $T/rom-files.mk || cp $STAGE_TMP/rom-files.mk $T/rom-files.mk
P=$FINAL_P
mkdir -p $(dirname $MAN); ( cd $T && find . -type f ! -name prebuilt-manifest.txt | sort | xargs sha256sum ) > $MAN
echo "modules: $(ls $P/lib/modules | wc -l)  files: $(wc -l < $MAN)  size: $(du -sh $T/prebuilt | cut -f1)"
cat $T/conflicts.txt | cut -c1-200
echo STAGE_ROM_V2_PREBUILTS_PASS
