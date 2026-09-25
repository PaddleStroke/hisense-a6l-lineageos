#!/bin/bash
# kvoice (24 Sep 2026): IPA v2.6L (ipa-legacy) for SDM660 on the 7.2.3 phone kernel (v67): build ipa_legacy.ko, the
# a6l_ipa_bam.ko bam_dma fork, the attended-test DT overlay module, compile-check the IPA overlay on the V74 DTB and
# assemble the v75/kipa bundle. Offline only.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
KV=$R/device/hisense/a6l/kernel/kvoice/ipa
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
B=/home/a6l/kernel/kvoice-build-ipa; rm -rf $B; mkdir -p $B/src $B/bam $B/ovl $B/dt
OUTR=$R/firmware/extracted/kvoice-20260924; mkdir -p $OUTR/ipa
fail() { echo "KIPA_BUILD_FAIL $*"; exit 1; }
st() { echo "=== $(date +%T) $*"; }
st "1. ipa-legacy (alikates 6.5 squash) + A6L 7.2/SDM660 fixes"
cd $B/src && git apply -p1 --include="drivers/net/ipa-legacy/*" $KV/0001-ipa-legacy-alikates-6.5.patch 2>/dev/null || fail apply
I=$B/src/drivers/net/ipa-legacy; cp $KV/a6l_ipa_bam/a6l_ipa_bam.h $I/
python3 $KV/a6l-ipa-legacy-fix.py $I || fail fix
make -C $K O=$O ARCH=arm64 LLVM=1 M=$I CONFIG_QCOM_IPA_LEGACY=m modules -j8 > $B/ipa-legacy-build.log 2>&1 || { grep error: $B/ipa-legacy-build.log | head; fail ipa-legacy; }
echo "warnings: $(grep -c warning: $B/ipa-legacy-build.log)"
( cd $B/src && diff -ruN /dev/null /dev/null; ) ; cd $B && mkdir -p orig && ( cd orig && git apply -p1 --include="drivers/net/ipa-legacy/*" $KV/0001-ipa-legacy-alikates-6.5.patch 2>/dev/null )
( cd $B && diff -ruN orig/drivers/net/ipa-legacy src/drivers/net/ipa-legacy -x '*.o' -x '*.ko' -x '*.cmd' -x '*.mod*' -x 'Module.symvers' -x 'modules.order' -x '.*' -x a6l_ipa_bam.h ) > $KV/a6l-ipa-legacy-7.2-sdm660.diff
echo "fix diff: $(grep -c '^[+-][^+-]' $KV/a6l-ipa-legacy-7.2-sdm660.diff) changed lines"
st "2. a6l_ipa_bam (bam_dma fork)"
cp $KV/a6l_ipa_bam/* $B/bam/; cp $K/drivers/dma/dmaengine.h $K/drivers/dma/virt-dma.h $B/bam/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/bam W=1 modules > $B/bam-build.log 2>&1 || { tail $B/bam-build.log; fail bam; }
grep -E "warning" $B/bam-build.log | head -5
st "3. DT overlay + runtime overlay module"
cp $KV/dt/a6l-ipa-v75.dtso $B/dt/; dtc -@ -q -I dts -O dtb -o $B/dt/a6l-ipa-v75.dtbo $B/dt/a6l-ipa-v75.dtso || fail dtc
fdtoverlay -i $R/firmware/extracted/recovery-v74-candidate-20260923/merged-captured-abl.dtb -o $B/dt/m-ipa-v74.dtb $B/dt/a6l-ipa-v75.dtbo || fail fdtoverlay
fdtget -t x $B/dt/m-ipa-v74.dtb /soc@0/ipa@147c0000 iommus clocks modem-remoteproc
cp $KV/ovl/* $B/ovl/
python3 - "$B/dt/a6l-ipa-v75.dtbo" "$B/ovl/a6l_ipa_dtbo.h" <<'PY'
import sys
b=open(sys.argv[1],'rb').read(); l=[', '.join('0x%02x'%x for x in b[i:i+12]) for i in range(0,len(b),12)]
open(sys.argv[2],'w').write('/* generated from a6l-ipa-v75.dtbo (%d bytes) */\nstatic const unsigned char a6l_ipa_dtbo[] __aligned(8) = {\n\t%s\n};\n'%(len(b),',\n\t'.join(l)))
PY
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ovl W=1 modules > $B/ovl-build.log 2>&1 || { tail $B/ovl-build.log; fail ovl; }
st "4. deps + symbol check"
for m in $I/ipa_legacy.ko $B/bam/a6l_ipa_bam.ko $B/ovl/a6l_ipa_ovl.ko; do
  echo "$(basename $m): depends=$(modinfo -F depends $m) vermagic=$(modinfo -F vermagic $m)"
  for s in $($CL/llvm-nm -u $m | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || echo "  UNRESOLVED $(basename $m): $s"; done
done
st "5. bundle v75/kipa"
BU=$B/bundle/kipa; mkdir -p $BU/modules $BU/extra
cp $B/ovl/a6l_ipa_ovl.ko $BU/extra/
: > $BU/modules/order.txt
add() { n=$1; [ -f $BU/modules/$n.ko ] && return; p=$(find $O -name "$n.ko" -o -name "$(echo $n | tr _ -).ko" | head -1); [ -n "$p" ] || { echo "  dep $n not found in $O"; return; }
  for d in $(modinfo -F depends $p | tr , ' '); do add $d; done; cp $p $BU/modules/; basename $p >> $BU/modules/order.txt; }
for d in $(modinfo -F depends $I/ipa_legacy.ko | tr , ' ') $(modinfo -F depends $B/bam/a6l_ipa_bam.ko | tr , ' '); do add $d; done
cp $B/bam/a6l_ipa_bam.ko $I/ipa_legacy.ko $BU/modules/; echo a6l_ipa_bam.ko >> $BU/modules/order.txt; echo ipa_legacy.ko >> $BU/modules/order.txt
add rmnet
cat $BU/modules/order.txt | tr '\n' ' '; echo
cp $KV/bundle/run.sh $BU/run.sh
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )
rm -rf $OUTR/ipa; mkdir -p $OUTR/ipa; cp -r $BU $OUTR/ipa/; cp $B/dt/*.dtbo $B/dt/m-ipa-v74.dtb $B/*.log $OUTR/ipa/
cat $BU/SHA256SUMS
echo KIPA_BUILD_DONE
