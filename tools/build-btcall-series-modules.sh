#!/usr/bin/env bash
# btcall (29 Sep 2026): build the q6voice SERIES audio module set = every staged module that sound/soc/qcom produces, from
# the rom-v2 series tree (incl. kvoice/a6l-btcall-incall-v75.patch), for the V67 kernel (out-a6l-phone-v67) and the r5
# kernel (out-a6l-rom-r5, MODVERSIONS). The whole sound/soc/qcom set is rebuilt together because the pseudo ports change
# LPASS_MAX_PORT/AFE_PORT_MAX (array sizes in q6afe, q6afe-dai, q6adm, q6routing, snd-soc-sm8250, snd-soc-qcom-common) and
# struct q6dsp_audio_port_dai_driver_config (snd-q6dsp-common <-> q6afe-dai): never mix these with the audio4 set.
# NOT staged by default: tools/stage-rom-v2-prebuilts.sh uses it only with A6L_AUDIO_SET=series (+ the btcall DT overlay via
# tools/build-rom-v2-dt.sh A6L_AUDIO_SET=series). WSL only, no phone, no `m`. ~5 min.
# Output: firmware/extracted/btcall-20260929/{v67,r5}/*.ko (strip-debug) + SHA256SUMS + build-info.txt
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; W=${A6L_BTCALL_WORK:-/home/a6l/scratch/btcall-build}; OUT=$R/firmware/extracted/btcall-20260929
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
export LOCALVERSION=+ KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-30 00:00:00 UTC'
K67=/home/a6l/kernel/a6l-baseline-7.2; O67=/home/a6l/kernel/out-a6l-phone-v67
K5=/home/a6l/kernel/a6l-rom-r5-src; O5=/home/a6l/kernel/out-a6l-rom-r5; R5=$R/firmware/extracted/kernel-r5-20260930
fail() { echo "BTCALL_SERIES_BUILD_FAIL $*"; exit 1; }
case "$W" in /tmp/*|/home/a6l/scratch/*) ;; *) fail "A6L_BTCALL_WORK must be under /tmp or /home/a6l/scratch";; esac
rm -rf $W; mkdir -p $W
# 1. series tree (and the series check itself)
A6L_KSERIES_DIR=$W/kseries bash $R/tools/check-rom-v2-kernel-series.sh > $W/kseries.log 2>&1; grep -q A6L_KSERIES_PASS $W/kseries.log || { tail -20 $W/kseries.log; fail "series check"; }
T=$W/kseries/t; grep -q q6voice_set_incall_record $T/sound/soc/qcom/qdsp6/q6voice.c || fail "series tree has no btcall patch"
NAMES=$(ls $R5/modules)   # the staged ROM module names (same for V67 and r5)
build() { # tag srctree objtree
  local tag=$1 K=$2 O=$3 D=$W/$1
  mkdir -p $D/inc/dt-bindings/sound $D/set; cp -r $T/sound/soc/qcom $D/qcom; cp $T/include/dt-bindings/sound/qcom,q6*.h $D/inc/dt-bindings/sound/
  ( cd $D/qcom && find . \( -name '*.o' -o -name '*.ko' -o -name '*.cmd' -o -name '*.mod' -o -name '*.mod.c' -o -name Module.symvers -o -name modules.order \) -delete )
  make -C $K O=$O ARCH=arm64 LLVM=1 M=$D/qcom NOSTDINC_FLAGS="-nostdinc -I$D/inc" CONFIG_SND_SOC_QDSP6_Q6VOICE=m CONFIG_SND_SOC_QDSP6_Q6VOICE_DAI=m \
    W=1 modules -j8 > $D/build.log 2>&1 || { grep -a -B2 -A8 'error' $D/build.log | head -30; fail "$tag build"; }
  echo "$tag: W=1 warnings in the btcall-touched files: $(grep -a 'warning:' $D/build.log | grep -cE 'q6afe|q6dsp-lpass-ports|q6routing|q6cvs|q6mvm|q6voice|sm8250|common\.')" \
       "(all: $(grep -ac 'warning:' $D/build.log))"
  for f in $(find $D/qcom -name '*.ko'); do n=$(basename $f); echo "$NAMES" | grep -qx "$n" || continue
    llvm-strip --strip-debug -o $D/set/$n $f; done
  echo "$tag: $(ls $D/set | wc -l) staged-name modules: $(ls $D/set | tr '\n' ' ')"
  cat $(find $D/qcom -name Module.symvers) > $D/set.symvers
}
build v67 $K67 $O67
build r5 $K5 $O5
# 2. verification
python3 - "$W" "$R5" "$O67" <<'PY' || fail "verify"
import os, sys, subprocess, struct, glob
W, R5, O67 = sys.argv[1:4]
CL = '/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin'
def run(*a): return subprocess.run(a, capture_output=True, text=True).stdout
def modinfo(f, k): return run('modinfo', '-F', k, f).strip()
def symvers(p):
    d = {}
    for l in open(p):
        c, s, m = l.split('\t')[:3]; d.setdefault(s, int(c, 16))
    return d
def versions(f):
    t = W + '/cur.versions'; subprocess.run([CL+'/llvm-objcopy', '-O', 'binary', '--only-section=__versions', f, t], check=True)
    b = open(t, 'rb').read(); os.unlink(t)
    return {b[i+8:i+64].split(b'\0')[0].decode(): struct.unpack('<Q', b[i:i+8])[0] & 0xffffffff for i in range(0, len(b), 64)}
def undef(f): return {l.split()[-1] for l in run(CL+'/llvm-nm', '-u', f).splitlines() if l.strip()}
bad = 0
# V67: vermagic, every undefined symbol resolves (vmlinux System.map, in-tree Module.symvers, the set)
S67 = W + '/v67/set'; mine = symvers(W + '/v67/set.symvers'); intree = symvers(O67 + '/Module.symvers')
sysmap = {l.split()[2] for l in open(O67 + '/System.map') if len(l.split()) == 3}
for n in sorted(os.listdir(S67)):
    f = f'{S67}/{n}'
    if modinfo(f, 'vermagic') != '7.2.3-a6l-probe+ SMP preempt mod_unload aarch64': print('VERMAGIC v67', n); bad += 1
    for s in undef(f):
        if s not in mine and s not in intree and s not in sysmap: print('UNRESOLVED v67', n, s); bad += 1
# r5: vermagic + every import CRC of the set AND of every other staged r5 module that imports from the set
S5 = W + '/r5/set'; mine5 = symvers(W + '/r5/set.symvers'); r5sv = symvers(R5 + '/Module.symvers')
checked = 0
for d, n in [(S5, n) for n in sorted(os.listdir(S5))] + [(R5 + '/modules', n) for n in sorted(os.listdir(R5 + '/modules')) if n not in os.listdir(S5)]:
    f = f'{d}/{n}'; own = d == S5
    if own and modinfo(f, 'vermagic') != '7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64': print('VERMAGIC r5', n); bad += 1
    v = versions(f)
    if own:
        for s in undef(f):
            if s not in v and s != '__this_module': print('UNVERSIONED r5', n, s); bad += 1
    for s, c in v.items():
        if s in mine5: want = mine5[s]
        elif own and s in r5sv: want = r5sv[s]
        else: continue   # other staged module importing something outside the set: verified by kernel-r5 (unchanged)
        checked += 1
        if want != c: print(f'CRC_MISMATCH r5 {n} {s} import={c:08x} export={want:08x}'); bad += 1
print(f'v67 {len(os.listdir(S67))} modules, r5 {len(os.listdir(S5))} modules, r5 imports checked {checked}, problems {bad}')
print('BTCALL_SERIES_VERIFY', 'PASS' if bad == 0 else 'FAIL'); sys.exit(1 if bad else 0)
PY
[ "$(ls $W/v67/set)" = "$(ls $W/r5/set)" ] || fail "v67/r5 sets differ"
# 3. output (replaced as a whole)
rm -rf $OUT; mkdir -p $OUT/v67 $OUT/r5; cp $W/v67/set/*.ko $OUT/v67/; cp $W/r5/set/*.ko $OUT/r5/
{ echo "btcall series audio module set, built $(date -u +%FT%TZ) by tools/build-btcall-series-modules.sh"
  echo "series: $(sha256sum < $R/device/hisense/a6l/kernel/rom-v2/series | cut -c1-16)  btcall patch: $(sha256sum < $R/device/hisense/a6l/kernel/kvoice/a6l-btcall-incall-v75.patch | cut -c1-16)"
  echo "v67: $K67 + series, O=$O67 (vermagic 7.2.3-a6l-probe+ SMP preempt mod_unload aarch64)"
  echo "r5:  $K5 headers + series sound/soc/qcom, O=$O5 (modversions; import CRCs checked against out-a6l-rom-r5 + this set)"
  echo "modules: $(ls $OUT/v67 | tr '\n' ' ')"; } > $OUT/build-info.txt
( cd $OUT && sha256sum v67/*.ko r5/*.ko build-info.txt > SHA256SUMS )
echo "output $OUT: $(ls $OUT/v67 | wc -l) + $(ls $OUT/r5 | wc -l) modules"
echo BTCALL_SERIES_BUILD_PASS
