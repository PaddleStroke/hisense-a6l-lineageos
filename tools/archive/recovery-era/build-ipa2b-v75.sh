#!/bin/bash
# ipa2fix agent (24 Sep 2026): OFFLINE. Step-logged ipa2_lite.ko (W=1) + a6l_ipa2_ovl.ko (3 overlay variants),
# bundle v75/ipa2b -> repo firmware/extracted/ipa-20260924/ipa2b (+ laptop ~/A6L-usb-20260915/v75/ipa2b).
exec < /dev/null
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
S=$R/device/hisense/a6l/kernel/ipa
B=/home/a6l/kernel/ipa2b-build; rm -rf $B; mkdir -p $B/ipa2-lite $B/ovl $B/dt
OUTR=$R/firmware/extracted/ipa-20260924
fail() { echo "IPA2B_BUNDLE_FAIL $*"; exit 1; }
echo "=== 1. ipa2_lite.ko (step-logged)"
cp $S/ipa2-lite/src/* $B/ipa2-lite/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ipa2-lite W=1 modules -j8 > $B/ipa2-lite-build.log 2>&1 || { tail -30 $B/ipa2-lite-build.log; fail ipa2-lite; }
echo "ipa2-lite warnings: $(grep -c 'warning:' $B/ipa2-lite-build.log)"; grep -A3 'warning:' $B/ipa2-lite-build.log | head -30
echo "=== 2. overlays"
cp $S/dt/*.dtso $B/dt/
for v in a6l-ipa-v75 a6l-ipa-identity-v75 a6l-ipa-noiommu-v75; do
  dtc -@ -q -I dts -O dtb -o $B/dt/$v.dtbo $B/dt/$v.dtso || fail dtc $v
  fdtoverlay -i $R/firmware/extracted/recovery-v74-candidate-20260923/merged-captured-abl.dtb -o $B/dt/m-$v-on-v74.dtb $B/dt/$v.dtbo || fail fdtoverlay $v
  echo "$v: compat=$(fdtget $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 compatible) reg=$(fdtget -t x $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 reg) clocks=$(fdtget -t x $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 clocks) iommus=$(fdtget -t x $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 iommus 2>&1 | head -1) chosen=$(fdtget $B/dt/m-$v-on-v74.dtb /chosen hisense,a6l-ipa)"
done
echo "rpmcc phandle in v74: $(fdtget -t x $R/firmware/extracted/recovery-v74-candidate-20260923/merged-captured-abl.dtb /soc@0/remoteproc@4080000 clocks 2>/dev/null | head -c 40)"
cp $S/ovl/a6l_ipa2_ovl.c $S/ovl/Kbuild $B/ovl/
python3 - "$B/dt" "$B/ovl/a6l_ipa2_dtbo.h" <<'PY'
import sys
def arr(name, path):
    b=open(path,'rb').read(); l=[', '.join('0x%02x'%x for x in b[i:i+12]) for i in range(0,len(b),12)]
    return '/* generated from %s (%d bytes) */\nstatic const unsigned char %s[] __aligned(8) = {\n\t%s\n};\n'%(path.split('/')[-1],len(b),name,',\n\t'.join(l))
d=sys.argv[1]; open(sys.argv[2],'w').write(arr('a6l_ipa_dtbo',d+'/a6l-ipa-v75.dtbo')+arr('a6l_ipa_identity_dtbo',d+'/a6l-ipa-identity-v75.dtbo')+arr('a6l_ipa_noiommu_dtbo',d+'/a6l-ipa-noiommu-v75.dtbo'))
PY
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ovl W=1 modules > $B/ovl-build.log 2>&1 || { tail -30 $B/ovl-build.log; fail ovl; }
echo "ovl warnings: $(grep -c 'warning:' $B/ovl-build.log)"; grep -A3 'warning:' $B/ovl-build.log | head
echo "=== 3. symbols + params"
for m in $B/ipa2-lite/ipa2_lite.ko $B/ovl/a6l_ipa2_ovl.ko; do
  echo "$(basename $m): depends=$(modinfo -F depends $m) vermagic=$(modinfo -F vermagic $m)"
  for s in $($CL/llvm-nm -u $m | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || echo "  UNRESOLVED $(basename $m): $s"; done
  modinfo -F parm $m | tr '\n' ' '; echo
done
strings $B/ipa2-lite/ipa2_lite.ko | grep -c "A6L_IPA_STEP"
echo "=== 4. bundle"
BU=$B/bundle/ipa2b; mkdir -p $BU/modules $BU/extra $BU/dt
OLD=$OUTR/ipa2
cp $OLD/modules/order.txt $BU/modules/
for k in $(cat $OLD/modules/order.txt); do [ "$k" = ipa2_lite.ko ] || cp $OLD/modules/$k $BU/modules/; done
cp $B/ipa2-lite/ipa2_lite.ko $BU/modules/
cp $B/ovl/a6l_ipa2_ovl.ko $BU/extra/
cp $B/dt/*.dtbo $B/dt/*.dtso $BU/dt/
cp $S/bundle/run.sh $BU/run.sh
sh -n $BU/run.sh && echo run.sh-syntax-ok
echo "order: $(tr '\n' ' ' < $BU/modules/order.txt)"
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )
rm -rf $OUTR/ipa2b; cp -r $BU $OUTR/ipa2b; mkdir -p $OUTR/ipa2b-build; cp $B/dt/m-*.dtb $B/ipa2-lite-build.log $B/ovl-build.log $OUTR/ipa2b-build/
cat $BU/SHA256SUMS
echo "=== 5. laptop"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
if timeout 20 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75/logs' < /dev/null; then
  timeout 60 $SCP -r 'C:\Users\Pierre\Desktop\A6L\firmware\extracted\ipa-20260924\ipa2b' a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
  timeout 20 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/ipa2b && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_STAGED_OK' < /dev/null
else echo "laptop unreachable (not staged)"; fi
echo IPA2B_BUNDLE_DONE
