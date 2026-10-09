#!/usr/bin/env bash
# voice-speaker-20261009: rebuild the DEPLOYED snd-soc-tfa98xx.ko and q6cvp/q6voice/q6voice-dai.ko from their exact
# source dirs, prove the rebuild is byte-identical to the ROM prebuilts, then apply 0002 (TFA) and 0003 (q6voice volume).
# Run in WSL as root. /home/a6l/kernel and /home/a6l/scratch are mounted under overlays inside a private mount namespace
# (same paths, so the objects are path-identical); the canonical trees and out dirs never change (bms-20261009 recipe).
#   TFA:     source = /home/a6l/kernel/out-a6l-rom-r5/oot/tfa (= /home/a6l/audio3/tfa, build-r5-oot.sh), O=out-a6l-rom-r5
#   q6voice: source = /home/a6l/scratch/voice-controls-20261003-063728/candidate (r5 qdsp6 + tx-mute + rx-volume),
#            O = its private out dir (r5 config + Module.symvers of the deployed set)
set -euo pipefail
X=/mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/voice-speaker-20261009
W=/home/a6l/voice-speaker-work-20261009
S=/home/a6l/kernel/a6l-rom-r5-src
O=/home/a6l/kernel/out-a6l-rom-r5
VC=/home/a6l/scratch/voice-controls-20261003-063728
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
P=/home/a6l/android/a6l-lineage24/device/hisense/a6l/rom/prebuilt/vendor/lib/modules
declare -A DEP=(
  [snd-soc-tfa98xx]=b855076b82e22475c947c31c86ee5ad8d0c7f5aee2164af3afe4e97a81a4b52b
  [q6cvp]=6214916403c68cf1c2e6c270418011ab42169e4d18525ac8fc2f1a134b56a082
  [q6voice]=4527a4ec7df48abd761f107019498ea9c538f7b8eeaa61067806bb82e40f5ad5
  [q6voice-dai]=adad28e27135cb1a9df38a320d91e8bb6d0676e92a8f93670b849e717a2679a1
)
for m in "${!DEP[@]}"; do [ "$(sha256sum $P/$m.ko | cut -d' ' -f1)" = "${DEP[$m]}" ] || { echo "ROM prebuilt $m changed"; exit 1; }; done
PROT="$O/.config $O/Module.symvers $O/arch/arm64/boot/Image $O/include/generated/utsrelease.h $VC/out/Module.symvers $VC/out/.config"
sha256sum $PROT > /tmp/vs-prot.before
rm -rf $W; mkdir -p $W/k/upper $W/k/work $W/k/merged $W/s/upper $W/s/work $W/s/merged $W/out
unshare -m --propagation private bash -c "
set -euo pipefail
mount -t overlay overlay -o lowerdir=/home/a6l/kernel,upperdir=$W/k/upper,workdir=$W/k/work $W/k/merged
mount --bind $W/k/merged /home/a6l/kernel
mount -t overlay overlay -o lowerdir=/home/a6l/scratch,upperdir=$W/s/upper,workdir=$W/s/work $W/s/merged
mount --bind $W/s/merged /home/a6l/scratch
export PATH=$CL:\$PATH LOCALVERSION=+ KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-30 00:00:00 UTC'
clean() { find \"\$1\" \\( -name '*.o' -o -name '*.ko' -o -name '*.mod' -o -name '*.mod.c' -o -name '.*.cmd' -o -name '*.o.d' -o -name Module.symvers -o -name modules.order \\) -delete; }

# ---- TFA98xx
T=$O/oot/tfa
clean \$T
make -C $S O=$O ARCH=arm64 LLVM=1 -j8 M=\$T modules > $W/tfa-baseline.log 2>&1 || { tail -30 $W/tfa-baseline.log; exit 1; }
llvm-strip --strip-debug -o $W/out/baseline-snd-soc-tfa98xx.ko \$T/snd-soc-tfa98xx.ko
patch -d \$T -p1 --no-backup-if-mismatch < $X/kernel/0002-tfa98xx-no-rate-constraint-on-dpcm-back-end.patch
make -C $S O=$O ARCH=arm64 LLVM=1 -j8 M=\$T modules > $W/tfa-build.log 2>&1 || { tail -30 $W/tfa-build.log; exit 1; }
llvm-strip --strip-debug -o $W/out/snd-soc-tfa98xx.ko \$T/snd-soc-tfa98xx.ko

# ---- q6cvp / q6voice / q6voice-dai
Q=$VC/candidate/sound/soc/qcom/qdsp6
clean \$Q
QF=\"NOSTDINC_FLAGS=-nostdinc -I$VC/candidate/include\"
make -C $S O=$VC/out ARCH=arm64 LLVM=1 -j8 M=\$Q \"\$QF\" modules > $W/q6voice-baseline.log 2>&1 || { tail -30 $W/q6voice-baseline.log; exit 1; }
for m in q6cvp q6voice q6voice-dai; do llvm-strip --strip-debug -o $W/out/baseline-\$m.ko \$Q/\$m.ko; done
cp \$Q/Module.symvers $W/out/baseline-q6voice.symvers
patch -d $VC/candidate -p1 --no-backup-if-mismatch < $X/kernel/0003-q6voice-rx-volume-fallbacks.patch
make -C $S O=$VC/out ARCH=arm64 LLVM=1 -j8 M=\$Q \"\$QF\" modules > $W/q6voice-build.log 2>&1 || { tail -30 $W/q6voice-build.log; exit 1; }
for m in q6cvp q6voice q6voice-dai; do llvm-strip --strip-debug -o $W/out/\$m.ko \$Q/\$m.ko; done
cp \$Q/Module.symvers $W/out/q6voice.symvers
"
sha256sum $PROT > /tmp/vs-prot.after
cmp /tmp/vs-prot.before /tmp/vs-prot.after && echo "PROTECTED_UNCHANGED (canonical out dirs)"
echo "== baseline rebuilds vs ROM prebuilts"
for m in snd-soc-tfa98xx q6cvp q6voice q6voice-dai; do
  h=$(sha256sum $W/out/baseline-$m.ko | cut -d' ' -f1)
  [ "$h" = "${DEP[$m]}" ] && echo "BASELINE_IDENTICAL $m" || echo "BASELINE_DIFFERS $m $h"
done
mkdir -p $X/kernel/modules
cp $W/out/snd-soc-tfa98xx.ko $W/out/q6cvp.ko $W/out/q6voice.ko $X/kernel/modules/
cmp -s $W/out/q6voice-dai.ko $P/q6voice-dai.ko && echo "q6voice-dai: rebuilt against the new q6voice = identical to the ROM one (not shipped)" \
  || { echo "q6voice-dai: differs, shipped"; cp $W/out/q6voice-dai.ko $X/kernel/modules/; }
cp $W/tfa-build.log $W/q6voice-build.log $X/kernel/
cp $W/out/baseline-q6voice.symvers $W/out/q6voice.symvers $X/kernel/
echo "== compile lines / warnings (patched builds)"
grep -hE '^\s+(CC|LD|MODPOST)|warning:|error:' $W/tfa-build.log $W/q6voice-build.log | grep -v "pr_fmt\|no previous prototype\|declare 'static'\|^\s*|\|static $" || true
echo "== overlay upper (source files written)"; (cd $W/k/upper && find . -type f \( -name '*.c' -o -name '*.h' \) | sort); (cd $W/s/upper && find . -type f \( -name '*.c' -o -name '*.h' \) | sort)
(cd $X/kernel/modules && sha256sum *.ko)
