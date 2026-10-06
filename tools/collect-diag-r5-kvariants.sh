#!/bin/bash
# diag-r5 (30 Sep 2026): collect a kernel bisect variant (tools/build-diag-r5-kvariants.sh) into
# firmware/extracted/diag-r5-20260930/kernel-<v>/: Image, Image.gz, config, Module.symvers, System.map, merge.log,
# sdhci-msm.ko + (r5m) the kit module set (display + base lists, strip-debug like the ROM stage), SHA256SUMS.
# k1 must keep Module.symvers byte-identical to r5p (then the r5 kit modules are used unchanged).
# usage: collect-diag-r5-kvariants.sh <k1|r5m>
set -euo pipefail
V=$1; R=/mnt/c/Users/Pierre/Desktop/A6L; X=$R/firmware/extracted; O=/home/a6l/kernel/out-a6l-diag-$V; D=$X/diag-r5-20260930/kernel-$V
ST=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin/llvm-strip
[ "$(cat $O/DONE)" = OK ] || { echo "build $V not OK: $(cat $O/DONE 2>/dev/null)"; exit 1; }
rm -rf $D; mkdir -p $D
cp $O/arch/arm64/boot/Image $O/arch/arm64/boot/Image.gz $O/Module.symvers $O/System.map $O/merge.log $D/; cp $O/.config $D/config
grep -q '^CONFIG_MODVERSIONS=y' $D/config
if [ $V = k1 ]; then # KFENCE=n drops its 2 exports and every other CRC stays - but the SLAB_* flag bits shift (see build script):
  diff <(grep -vE '\s(kfence_sample_interval|__kfence_pool)\s' $X/kernel-r5p-20260930/Module.symvers) $D/Module.symvers > /dev/null \
    && echo "k1 Module.symvers == r5p minus the 2 KFENCE exports (CRCs identical, yet NOT module-compatible: own modules)"; fi
$ST --strip-debug -o /tmp/diag-r5-$V-sdhci.ko $O/drivers/mmc/host/sdhci-msm.ko && cp /tmp/diag-r5-$V-sdhci.ko $D/sdhci-msm.ko
mkdir -p $D/modules; if true; then
  for m in $(cat $R/device/hisense/a6l/rom/modules/display.txt $R/device/hisense/a6l/rom/modules/base.txt | tr -d '\r' | grep -v '^[[:space:]]*#' | awk 'NF{print $1}'); do
    f=$(find $O -path $O/oot -prune -o -name $m -print | grep -v '/oot/' | head -n 1 || true); [ -n "$f" ] || f=$(find $O/oot -name $m | head -n 1 || true)
    if [ -n "$f" ]; then $ST --strip-debug -o /tmp/diag-r5-$V-$m $f && cp /tmp/diag-r5-$V-$m $D/modules/$m && rm -f /tmp/diag-r5-$V-$m; else echo "$V: no build of $m (kit step skips it)"; fi
  done
  for m in $D/modules/*.ko; do grep -q 'vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64' $m || { echo "vermagic $m"; exit 2; }; done
fi
(cd $D && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS)
echo "COLLECT_$V OK $(grep ' Image$' $D/SHA256SUMS)"; ls $D $D/modules 2>/dev/null | tr '\n' ' '
