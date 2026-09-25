#!/bin/bash
# kvoice (24 Sep 2026): build q6voice modules (7.2.3 phone kernel v67), the attended-test DT-overlay module, a6l-q6voiced (NDK),
# compile-check the voice DT overlays against the V74 DTB, and assemble the v75/kvoice bundle. Offline only.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2          # read-only use (M= build against out-v67)
O=/home/a6l/kernel/out-a6l-phone-v67
W=/home/a6l/kernel/a6l-7.2-voice             # kvoice worktree (branch kvoice-7.2)
S=/home/a6l/kernel/kvoice-src
B=/home/a6l/kernel/kvoice-build; rm -rf $B; mkdir -p $B
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
KV=$R/device/hisense/a6l/kernel/kvoice
OUTR=$R/firmware/extracted/kvoice-20260924; mkdir -p $OUTR
export PATH=$CL:$PATH
st() { echo "=== $(date +%T) $*"; }
fail() { echo "KVOICE_BUILD_FAIL $*"; exit 1; }

st "1. worktree state (upstream q6voice series applied by kvoice-07), apply A6L + audio3 q6asm patches"
cd $W || fail worktree
mkdir -p $KV/q6voice-series; cp $S/export/sc7280/00{01..09}*.patch $S/export/sc7280/00{13..19}*.patch $S/export/sc7280/002[123]*.patch $KV/q6voice-series/ 2>/dev/null
if ! grep -q A6L_Q6VOICE sound/soc/qcom/qdsp6/q6voice.c; then
  patch -p1 --dry-run < $KV/a6l-q6voice-sdm660-v75.patch > /dev/null || fail "a6l patch dry-run"
  patch -p1 < $KV/a6l-q6voice-sdm660-v75.patch || fail "a6l patch"
fi
if ! grep -q q6asm_dai_of_xlate_dai_name sound/soc/qcom/qdsp6/q6asm-dai.c; then
  patch -p1 < $R/device/hisense/a6l/kernel/a6l-q6asm-dai-xlate-v75.patch || fail "q6asm xlate patch"
fi
git -C $W status --short | grep -v '\.o$\|\.ko$\|\.cmd$\|\.mod' | head -30
git -C $W diff e47d622cb --stat -- sound include | tail -3
# full series as one patch vs the 7.2 base (for the flash agent: git apply on a6l-baseline-7.2)
( cd $W && git add -N sound/soc/qcom/qdsp6/q6voice* sound/soc/qcom/qdsp6/q6mvm* sound/soc/qcom/qdsp6/q6cv* include/dt-bindings/sound/qcom,q6voice.h 2>/dev/null
  git diff e47d622cb -- sound/soc/qcom/Kconfig sound/soc/qcom/qdsp6 include/dt-bindings/sound/qcom,q6voice.h ) > $KV/a6l-q6voice-full-7.2.3.patch
echo "full patch: $(grep -c '^+++' $KV/a6l-q6voice-full-7.2.3.patch) files, $(wc -l < $KV/a6l-q6voice-full-7.2.3.patch) lines"
git -C $K apply --check $KV/a6l-q6voice-full-7.2.3.patch && echo "FULL_PATCH_APPLIES_ON_BASELINE (check only)" || echo "full patch does NOT apply on the baseline working copy"

st "2. q6voice modules: in-tree Makefile/Kconfig of the worktree, M= build against out-v67"
Q=$B/qdsp6; cp -r $W/sound/soc/qcom/qdsp6 $Q; cp $W/sound/soc/qcom/common.h $B/common.h; mkdir -p $Q/include/dt-bindings/sound; cp $W/include/dt-bindings/sound/qcom,q6voice.h $Q/include/dt-bindings/sound/
rm -f $Q/*.o $Q/*.ko $Q/.*.cmd $Q/*.mod*
echo 'ccflags-y += -I$(src)/include' >> $Q/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 M=$Q CONFIG_SND_SOC_QDSP6_Q6VOICE=m CONFIG_SND_SOC_QDSP6_Q6VOICE_DAI=m W=1 modules -j8 > $B/qdsp6-build.log 2>&1; rc=$?
grep -E "warning|error" $B/qdsp6-build.log | grep -E "q6voice|q6mvm|q6cv|q6asm-dai" | head -30
[ $rc = 0 ] || { tail -30 $B/qdsp6-build.log; fail "qdsp6 modules rc=$rc"; }
ls -la $Q/*.ko | awk '{print $5, $9}'
for m in q6voice-common q6mvm q6cvs q6cvp q6voice q6voice-dai q6asm-dai; do [ -f $Q/$m.ko ] || fail "missing $m.ko"; done
$CL/llvm-readelf -S $Q/q6voice.ko > /dev/null || fail readelf
for m in q6voice-common q6mvm q6cvs q6cvp q6voice q6voice-dai; do echo "$m: $(modinfo -F depends $Q/$m.ko) | vermagic $(modinfo -F vermagic $Q/$m.ko)"; done
modinfo -F parm $Q/q6voice.ko
# undefined symbols must exist in the v67 kernel/modules
for m in q6voice-common q6mvm q6cvs q6cvp q6voice q6voice-dai; do
  for s in $($CL/llvm-nm -u $Q/$m.ko | awk '{print $2}'); do
    grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || grep -qP "^0x[0-9a-f]+\t$s\t" $Q/Module.symvers || echo "  UNRESOLVED $m: $s"
  done
done
echo "symbol check done"

st "3. overlay module + DT overlays"
V=$R/firmware/extracted/recovery-v74-candidate-20260923
D=$B/dt; mkdir -p $D; cp $KV/dt/*.dtso $D/
for f in $D/*.dtso; do dtc -@ -q -I dts -O dtb -o ${f%.dtso}.dtbo $f || fail "dtc $f"; done
cp $V/merged-captured-abl.dtb $D/v74.dtb; cp $D/v74.dtb $D/nosound.dtb
for n in mm1-dai-link mm2-dai-link int0-mi2s-dai-link int3-mi2s-dai-link; do fdtput -r $D/nosound.dtb /sound/$n; done
AS=$(fdtget -t s $D/v74.dtb /__symbols__ q6asmdai); for c in $(fdtget -l $D/v74.dtb $AS); do fdtput -r $D/nosound.dtb $AS/$c; done
for p in "v74 a6l-voice-onbase-v75" "v74 a6l-voice-speaker-onbase-v75" "nosound a6l-audio-voice-internal-v75" "nosound a6l-audio-voice-speaker-v75"; do
  set -- $p; fdtoverlay -i $D/$1.dtb -o $D/m-$2.dtb $D/$2.dtbo || fail "fdtoverlay $2"
  python3 $R/tools/check-voice-dt-links.py $D/m-$2.dtb | tail -1; python3 $R/tools/check-audio-dt-links.py $D/m-$2.dtb --rule reg | tail -1
done
OV=$B/ovl; mkdir -p $OV; cp $KV/ovl/* $OV/
cmp $OV/a6l-voice-onbase-v75.dtbo $D/a6l-voice-onbase-v75.dtbo 2>/dev/null || echo "note: embedded dtbo differs from fresh dtc build (rebuild header)"
python3 - "$D/a6l-voice-onbase-v75.dtbo" "$OV/a6l_voice_dtbo.h" <<'PY'
import sys
b=open(sys.argv[1],'rb').read()
l=[', '.join('0x%02x'%x for x in b[i:i+12]) for i in range(0,len(b),12)]
open(sys.argv[2],'w').write('/* generated from a6l-voice-onbase-v75.dtbo (%d bytes) */\nstatic const unsigned char a6l_voice_dtbo[] __aligned(8) = {\n\t%s\n};\n'%(len(b),',\n\t'.join(l)))
PY
make -C $K O=$O ARCH=arm64 LLVM=1 M=$OV W=1 modules > $B/ovl-build.log 2>&1 || { tail -20 $B/ovl-build.log; fail ovl; }
grep -i warning $B/ovl-build.log | head; ls -la $OV/a6l_voice_ovl.ko

st "4. a6l-q6voiced (NDK static, API 34)"
QV=$B/q6voiced; mkdir -p $QV; cp $R/device/hisense/a6l/kvoice/q6voiced/a6l_q6voiced.c $QV/
$NDK/aarch64-linux-android34-clang -static -O2 -Wall -Wextra -Werror -o $QV/a6l-q6voiced $QV/a6l_q6voiced.c || fail ndk
$NDK/llvm-strip $QV/a6l-q6voiced; file $QV/a6l-q6voiced
gcc -Wall -Wextra -Werror -O2 -o $QV/a6l-q6voiced-host $QV/a6l_q6voiced.c && echo host-build-ok

st "5. bundle v75/kvoice"
A3=$R/firmware/extracted/audio3-20260924/v75/audio3
BU=$B/bundle/kvoice; mkdir -p $BU/modules $BU/extra $BU/bin $BU/mixer3
cp $A3/modules/*.ko $BU/modules/ || fail "audio3 modules"
cp $Q/q6asm-dai.ko $BU/modules/q6asm-dai.ko      # same patch as audio3 (xlate by reg), rebuilt here
for m in q6voice-common q6mvm q6cvs q6cvp q6voice q6voice-dai; do cp $Q/$m.ko $BU/modules/; done
awk '/^snd-soc-sm8250.ko$/{print "q6voice-common.ko\nq6mvm.ko\nq6cvs.ko\nq6cvp.ko\nq6voice.ko\nq6voice-dai.ko"} {print}' $A3/modules/order.txt > $BU/modules/order.txt
grep -q q6voice-dai $BU/modules/order.txt || fail order
while read -r ko; do [ -f $BU/modules/$ko ] || echo "  order.txt lists missing $ko"; done < $BU/modules/order.txt
cp $OV/a6l_voice_ovl.ko $BU/extra/
cp $A3/bin/tinymix $A3/bin/pcmprobe $BU/bin/; cp $QV/a6l-q6voiced $BU/bin/
cp $A3/mixer3/common-off.txt $A3/mixer3/headset.txt $A3/mixer3/headset-mic.txt $A3/mixer3/main-mic.txt $BU/mixer3/
cp $KV/bundle/run.sh $BU/run.sh
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )

st "6. copy to repo + laptop"
rm -rf $OUTR/v75; mkdir -p $OUTR/v75 $OUTR/dt $OUTR/logs
cp -r $BU $OUTR/v75/; cp $D/*.dtbo $D/m-*.dtb $OUTR/dt/; cp $B/*.log $OUTR/logs/
( cd $OUTR && find . -type f ! -name SHA256SUMS-all | sort | xargs sha256sum > SHA256SUMS-all )
SCP=/mnt/c/Windows/System32/OpenSSH/scp.exe
WIN=$(wslpath -w $OUTR/v75/kvoice)
timeout 120 $SCP -r -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf "$WIN" a6l-laptop:A6L-usb-20260915/v75/ && echo LAPTOP_STAGED || echo "LAPTOP_STAGE_FAILED (copy by hand)"
timeout 60 $R/.relay/lap.sh 60 'cd ~/A6L-usb-20260915/v75/kvoice && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_HASH_OK' 2>&1 | tail -3
cat $BU/SHA256SUMS | grep -E "q6voice|q6mvm|q6cv|ovl|q6voiced|run.sh|q6asm"
echo KVOICE_BUILD_DONE
