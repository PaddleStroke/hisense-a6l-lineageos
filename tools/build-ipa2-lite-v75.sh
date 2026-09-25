#!/bin/bash
# ipa agent (24 Sep 2026): OFFLINE. Rebuild ipa2_lite.ko (W=1), build a6l_ipa2_ovl.ko, check both overlays on the V74 DTB,
# assemble bundle v75/ipa2 -> repo firmware/extracted/ipa-20260924/ (+ laptop v75/ipa2 if reachable, 20 s timeout).
exec < /dev/null
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
S=$R/device/hisense/a6l/kernel/ipa
B=/home/a6l/kernel/ipa-build; rm -rf $B/ipa2-lite $B/ovl $B/dt $B/bundle; mkdir -p $B/ipa2-lite $B/ovl $B/dt
OUTR=$R/firmware/extracted/ipa-20260924
fail() { echo "IPA_BUNDLE_FAIL $*"; exit 1; }
echo "=== 1. ipa2_lite.ko"
cp $S/ipa2-lite/src/* $B/ipa2-lite/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ipa2-lite W=1 modules -j8 > $B/ipa2-lite-build.log 2>&1 || { tail -20 $B/ipa2-lite-build.log; fail ipa2-lite; }
echo "ipa2-lite warnings: $(grep -c 'warning:' $B/ipa2-lite-build.log)"
echo "=== 2. overlays"
cp $S/dt/*.dtso $B/dt/
for v in a6l-ipa-v75 a6l-ipa-identity-v75; do
  dtc -@ -q -I dts -O dtb -o $B/dt/$v.dtbo $B/dt/$v.dtso || fail dtc $v
  fdtoverlay -i $R/firmware/extracted/recovery-v74-candidate-20260923/merged-captured-abl.dtb -o $B/dt/m-$v-on-v74.dtb $B/dt/$v.dtbo || fail fdtoverlay $v
  echo "$v: compat=$(fdtget $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 compatible) iommus=$(fdtget -t x $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 iommus) icc=$(fdtget -t x $B/dt/m-$v-on-v74.dtb /soc@0/ipa@14780000 interconnects)"
done
cp $S/ovl/a6l_ipa2_ovl.c $S/ovl/Kbuild $B/ovl/
python3 - "$B/dt" "$B/ovl/a6l_ipa2_dtbo.h" <<'PY'
import sys
def arr(name, path):
    b=open(path,'rb').read(); l=[', '.join('0x%02x'%x for x in b[i:i+12]) for i in range(0,len(b),12)]
    return '/* generated from %s (%d bytes) */\nstatic const unsigned char %s[] __aligned(8) = {\n\t%s\n};\n'%(path.split('/')[-1],len(b),name,',\n\t'.join(l))
d=sys.argv[1]; open(sys.argv[2],'w').write(arr('a6l_ipa_dtbo',d+'/a6l-ipa-v75.dtbo')+arr('a6l_ipa_identity_dtbo',d+'/a6l-ipa-identity-v75.dtbo'))
PY
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ovl W=1 modules > $B/ovl-build.log 2>&1 || { tail $B/ovl-build.log; fail ovl; }
echo "ovl warnings: $(grep -c 'warning:' $B/ovl-build.log)"
echo "=== 3. symbols"
for m in $B/ipa2-lite/ipa2_lite.ko $B/ovl/a6l_ipa2_ovl.ko; do
  echo "$(basename $m): depends=$(modinfo -F depends $m) vermagic=$(modinfo -F vermagic $m)"
  for s in $($CL/llvm-nm -u $m | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || echo "  UNRESOLVED $(basename $m): $s"; done
done
echo "=== 4. bundle"
BU=$B/bundle/ipa2; mkdir -p $BU/modules $BU/extra $BU/dt
: > $BU/modules/order.txt
add() { local n=$1 p d; [ -f $BU/modules/$n.ko ] && return; p=$(find $O -name "$n.ko" -o -name "$(echo $n | tr _ -).ko" | head -1); [ -n "$p" ] || { echo "  dep $n not found in $O (built-in?)"; return; }
  for d in $(modinfo -F depends $p | tr , ' '); do add $d; done; [ -f $BU/modules/$(basename $p) ] && return; cp $p $BU/modules/; basename $p >> $BU/modules/order.txt; }
add qrtr
for d in $(modinfo -F depends $B/ipa2-lite/ipa2_lite.ko | tr , ' '); do add $d; done
add rmnet
cp $B/ipa2-lite/ipa2_lite.ko $BU/modules/; echo ipa2_lite.ko >> $BU/modules/order.txt
cp $B/ovl/a6l_ipa2_ovl.ko $BU/extra/
cp $B/dt/*.dtbo $B/dt/*.dtso $BU/dt/
cp $S/bundle/run.sh $BU/run.sh
echo "order: $(tr '\n' ' ' < $BU/modules/order.txt)"
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )
rm -rf $OUTR; mkdir -p $OUTR; cp -r $BU $OUTR/; cp $B/dt/m-*.dtb $B/*.log $B/ipa2-lite/../ovl-build.log $OUTR/ 2>/dev/null; cp $B/ipa2-lite-build.log $OUTR/ 2>/dev/null
cat $BU/SHA256SUMS
echo "=== 5. laptop (optional)"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
if timeout 20 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75' < /dev/null; then
  timeout 60 $SCP -r 'C:\Users\Pierre\Desktop\A6L\firmware\extracted\ipa-20260924\ipa2' a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
  timeout 20 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/ipa2 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_STAGED_OK' < /dev/null
else echo "laptop unreachable (not staged)"; fi
echo IPA_BUNDLE_DONE
