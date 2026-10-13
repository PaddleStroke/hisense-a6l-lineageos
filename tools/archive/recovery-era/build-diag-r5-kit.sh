#!/bin/bash
# diag-r5 (30 Sep 2026, docs/diag-r5-20260930.md): assemble the diag-r5 kit in WSL. No phone, no laptop.
#   images/  3 diagnostic boot images (tools/Prepare-DiagR5Boot.py) + reports + captured-ABL validations
#   phone/   d.sh + lists (ROM display.txt/base.txt) + mods-r5 (the ROM r5 stage = kernel-r5-20260930 builds)
#            + mods-v67 (the ROM V67 stage) + fw (GPU firmware of the proven bundles) + SHA256SUMS (checked on the phone)
#   host/    laptop scripts (stream, push, run, pull, bootwrite) + tests/
# usage: build-diag-r5-kit.sh   (after Prepare-DiagR5Boot.py img-<variant> for the 3 variants in /home/a6l/diag-r5)
set -euo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; W=/home/a6l/diag-r5; K=$W/kit; SC=/home/a6l/scratch/diag-r5
SRC=$R/device/hisense/a6l/diagnostic/diag-r5; X=$R/firmware/extracted
rm -rf $K; mkdir -p $K/images $K/phone/lists $K/phone/mods-r5 $K/phone/mods-v67 $K/phone/mods-r5m $K/phone/mods-k1 $K/phone/fw $K/host $K/tests
# stage dry runs (the exact module files an r6b ROM would load with A6L_KERNEL=r5, and with the V67 default)
tr -d '\r' < $R/tools/stage-rom-v2-prebuilts.sh > $W/stage.sh
for s in v67 r5; do rm -rf $SC/stage-$s; if [ $s = r5 ]; then A6L_KERNEL=r5 A6L_STAGE_DRYRUN=$SC/stage-$s bash $W/stage.sh > $W/stage-$s.log 2>&1; else A6L_STAGE_DRYRUN=$SC/stage-$s bash $W/stage.sh > $W/stage-$s.log 2>&1; fi
  tail -n 1 $W/stage-$s.log | grep -q STAGE_ROM_V2_PREBUILTS_PASS || { echo "stage $s failed"; exit 2; }; done
for l in display.txt base.txt; do tr -d '\r' < $R/device/hisense/a6l/rom/modules/$l > $K/phone/lists/$l; done
mods=$(cat $K/phone/lists/display.txt $K/phone/lists/base.txt | grep -v '^[[:space:]]*#' | awk 'NF{print $1}')
for s in r5 v67; do for m in $mods; do cp $SC/stage-$s/device/hisense/a6l/rom/prebuilt/vendor/lib/modules/$m $K/phone/mods-$s/; done; done
for m in $mods; do grep -q 'vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64' $K/phone/mods-r5/$m || { echo "r5 vermagic $m"; exit 3; }
  grep -qa 'vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload aarch64' $K/phone/mods-v67/$m || { echo "v67 vermagic $m"; exit 3; }; done
# provenance: every kit module = the ROM stage file; record which firmware/extracted file it is (sha match, indexed dirs)
find $X/kernel-r5-20260930/modules $X/bh2k-20260929 $X/wdt-20260929 $X/kernel-gaps-20260929 $X/epd-v73 $X/v69-attended-bundle-20260921 \
     $X/v71-attended-bundle-20260922 $X/phone-kernel-v67-candidate-20260919 -name '*.ko' 2>/dev/null | xargs sha256sum > $W/ko-index.txt
for s in r5 v67; do (cd $K/phone/mods-$s && for m in *.ko; do h=$(sha256sum $m | cut -c1-64); src=$(grep "^$h " $W/ko-index.txt | head -n 1 | cut -c67- || true)
  echo "${h:0:16} $m ${src#$X/}"; done > $K/provenance-$s.txt); done
sed -i 's/ $/ ROM stage only (V67: out-a6l-phone-v67 modinst \/ modules.tar.gz)/' $K/provenance-v67.txt $K/provenance-r5.txt
# alternatives for the panel bisect: r5 = the pre-bh2k r5 build (epd-v73 source); v67 = the proven bundle-v74 V73 panel (47dade5f)
mkdir -p $K/phone/mods-r5/alt $K/phone/mods-v67/alt
cp $X/kernel-r5-20260930/modules/panel-a6l-epd-dsi.ko $K/phone/mods-r5/alt/
cp $X/epd-v73/panel-a6l-epd-dsi.ko $K/phone/mods-v67/alt/
echo "47dade5fa355a98e094bf91a7d0faac9e27b0f124c0a3c49baae9541df7b9523  $K/phone/mods-v67/alt/panel-a6l-epd-dsi.ko" | sha256sum -c --quiet - || { echo "v67 alt panel is not 47dade5f"; exit 4; }
# r5m kernel variant: its own builds (tools/collect-diag-r5-kvariants.sh r5m; no xt_quota2/uid_sys_stats -> base step skips them)
cp $X/diag-r5-20260930/kernel-r5m/modules/*.ko $K/phone/mods-r5m/
cp $X/diag-r5-20260930/kernel-k1/modules/*.ko $K/phone/mods-k1/     # k1: own builds too (SLAB flag bits shift without KFENCE)
for m in $K/phone/mods-r5m/*.ko $K/phone/mods-k1/*.ko; do grep -q 'vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64' $m || { echo "r5m vermagic $m"; exit 3; }; done
cp -r $X/v71-attended-bundle-20260922/gpu/firmware/. $K/phone/fw/
tr -d '\r' < $SRC/d.sh > $K/phone/d.sh; chmod 755 $K/phone/d.sh; sh -n $K/phone/d.sh
(cd $K/phone && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS)
for f in $SRC/host/*.sh; do tr -d '\r' < $f > $K/host/$(basename $f); chmod 755 $K/host/$(basename $f); bash -n $K/host/$(basename $f); done
tr -d '\r' < $SRC/tests/test-bootwrite-mock.sh > $K/tests/test-bootwrite-mock.sh; chmod 755 $K/tests/*.sh
for v in r5p-dt6b r5p-dtv74 v67-dt6b k1-dt6b r5m-dt6b; do
  I=$W/img-$v; python3 -c "import json,sys,hashlib; r=json.load(open('$I/report.json')); a=json.load(open('$I/captured-abl-validation.json')); \
assert a['passed'] and hashlib.sha256(open('$I/boot.img','rb').read()).hexdigest()==r['boot_sha256']" || { echo "img $v not validated"; exit 5; }
  cp $I/boot.img $K/images/diag-$v.img; cp $I/report.json $K/images/diag-$v.report.json; cp $I/captured-abl-validation.json $K/images/diag-$v.abl.json
done
(cd $K && find . -type f ! -name KIT-SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > KIT-SHA256SUMS)
echo "DIAG_R5_KIT_DONE $(wc -l < $K/KIT-SHA256SUMS) files, phone $(wc -l < $K/phone/SHA256SUMS), $(du -sh $K | cut -f1)"
