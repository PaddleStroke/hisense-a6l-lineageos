#!/bin/bash
# camss rom1 (29 Sep 2026): the clean ROM qcom-camss = camfix5 cumulative (old ROM patch) stripped of every diagnostic and
# experiment knob + the stock sdm660 CSIPHY digital clocks (cphy_csidK 200 MHz, csiK 310 MHz) that fixed the camera in the
# attended t35 run (docs/camera-rom-20260929.md). Builds qcom-camss.ko for BOTH ROM kernels with M= (no kernel out dir is
# modified): v67 (out-a6l-phone-v67, baseline tree) and r5 (out-a6l-rom-r5, a6l-rom-r5-src, MODVERSIONS).
# Output: device/hisense/a6l/kernel/camera/patches/camss-sdm660-rom1.patch and
#         firmware/extracted/camera-rom1-20260929/{v67,r5}/qcom-camss.ko + SHA256SUMS + build-info.txt
# WSL only, ~3 min. Never deletes an earlier build dir.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
K5=/home/a6l/kernel/a6l-rom-r5-src; O5=/home/a6l/kernel/out-a6l-rom-r5
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
P=$W/device/hisense/a6l/kernel/camera/patches; A=$W/firmware/extracted/camera-rom1-20260929
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
B=/home/a6l/camss-rom1; [ -e $B ] && B=$B-$(date +%H%M%S)
mkdir -p $B/orig $B/camss $A/v67 $A/r5
step "sources: baseline camss + camfix5 cumulative + camss_rom1_patch.py"
cp -r $K/drivers/media/platform/qcom/camss $B/orig/camss
cp -r $K/drivers/media/platform/qcom/camss/. $B/camss/
# the r5 tree's camss must be the same baseline (it only differs by the series patch we replace)
diff -r -q $K/drivers/media/platform/qcom/camss $B/orig/camss >/dev/null || { echo ROM1_FAIL copy; exit 1; }
( cd $B && tr -d '\r' < $P/camss-sdm660-camfix5-cumulative.patch | patch -s -p0 ) || { echo ROM1_FAIL camfix5; exit 1; }
python3 $P/camss_rom1_patch.py $B/orig/camss $B/camss > $B/rom1.patchlog 2>&1; rc=$?; cat $B/rom1.patchlog; [ $rc = 0 ] || { echo ROM1_FAIL strip; exit 1; }
( cd $B && diff -ru orig/camss camss ) > $P/camss-sdm660-rom1.patch
wc -l $P/camss-sdm660-rom1.patch; grep -c '^+++' $P/camss-sdm660-rom1.patch
# the r5 source tree = baseline + camfix5 series patch: prove the baseline part is identical before building against it
mkdir -p $B/r5check; cp -r $K5/drivers/media/platform/qcom/camss $B/r5check/camss
( cd $B/r5check && tr -d '\r' < $P/camss-sdm660-camfix5-cumulative.patch | patch -s -R -p0 ) && diff -r -q $B/r5check/camss $B/orig/camss && echo "r5 tree camss = baseline + camfix5 (R5_SRC_MATCH)"
cp -r $B/camss $B/camss-r5
step "build v67 (W=1)"
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/camss -j8 modules > $B/v67-build.log 2>&1 || { tail -30 $B/v67-build.log; echo ROM1_FAIL v67 build; exit 1; }
echo "v67 warnings: $(grep -c 'warning:' $B/v67-build.log)"; grep 'warning:' $B/v67-build.log | head -6
step "build r5 (W=1, LOCALVERSION=+)"
LOCALVERSION=+ make -C $K5 O=$O5 ARCH=arm64 LLVM=1 W=1 M=$B/camss-r5 -j8 modules > $B/r5-build.log 2>&1 || { tail -30 $B/r5-build.log; echo ROM1_FAIL r5 build; exit 1; }
echo "r5 warnings: $(grep -c 'warning:' $B/r5-build.log)"; grep 'warning:' $B/r5-build.log | head -6
llvm-strip --strip-debug -o $B/v67-qcom-camss.ko $B/camss/qcom-camss.ko && cp $B/v67-qcom-camss.ko $A/v67/qcom-camss.ko
llvm-strip --strip-debug -o $B/r5-qcom-camss.ko $B/camss-r5/qcom-camss.ko && cp $B/r5-qcom-camss.ko $A/r5/qcom-camss.ko
step "checks"
bad=0
v=$(modinfo -F vermagic $A/v67/qcom-camss.ko); [ "$v" = "7.2.3-a6l-probe+ SMP preempt mod_unload aarch64" ] && echo "v67 vermagic OK: $v" || { echo "v67 VERMAGIC BAD: $v"; bad=1; }
v=$(modinfo -F vermagic $A/r5/qcom-camss.ko); [ "$v" = "7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64" ] && echo "r5 vermagic OK: $v" || { echo "r5 VERMAGIC BAD: $v"; bad=1; }
for f in $A/v67/qcom-camss.ko $A/r5/qcom-camss.ko; do echo "$(basename $(dirname $f)) parm: $(modinfo -F parm $f | tr '\n' ' ')"; echo "$(basename $(dirname $f)) depends: $(modinfo -F depends $f)"; done
# v67: every undefined symbol exported by vmlinux or a module of out-a6l-phone-v67
for s in $(llvm-nm -u $A/v67/qcom-camss.ko | awk '{print $2}'); do grep -qP "\t$s\t" $O/Module.symvers || { echo "v67 UNRESOLVED $s"; bad=1; }; done
echo "v67 undefined symbols: $(llvm-nm -u $A/v67/qcom-camss.ko | wc -l) all in Module.symvers (unless listed above)"
# r5: every __versions CRC == the export CRC in out-a6l-rom-r5/Module.symvers, and no unversioned import
python3 - $A/r5/qcom-camss.ko $O5/Module.symvers $CL <<'PY' || bad=1
import sys, struct, subprocess
ko, sv, cl = sys.argv[1:4]
t='/tmp/rom1-versions.bin'; subprocess.run([cl+'/llvm-objcopy','-O','binary','--only-section=__versions',ko,t],check=True); b=open(t,'rb').read()
v={b[i+8:i+64].split(b'\0')[0].decode(): struct.unpack('<Q',b[i:i+8])[0]&0xffffffff for i in range(0,len(b),64)}
exp={}
for l in open(sv):
    c,s,m=l.split('\t')[:3]; exp.setdefault(s,(int(c,16),m))
und={l.split()[-1] for l in subprocess.run([cl+'/llvm-nm','-u',ko],capture_output=True,text=True).stdout.splitlines()}
bad=0
for s in und:
    if s not in v and s!='__this_module': print('UNVERSIONED',s); bad+=1
prov={}
for s,c in v.items():
    if s not in exp: print('NOEXPORT',s); bad+=1; continue
    if exp[s][0]!=c: print('CRC_MISMATCH',s,hex(c),hex(exp[s][0]),exp[s][1]); bad+=1
    prov[exp[s][1]]=prov.get(exp[s][1],0)+1
print('r5 __versions', len(v), 'imports; CRC match against out-a6l-rom-r5/Module.symvers; providers', prov)
print('R5_CRC', 'PASS' if not bad else 'FAIL')
sys.exit(1 if bad else 0)
PY
# the staged r5 providers (videodev, mc, vb2, v4l2-*) must be the ones whose CRCs are in out-a6l-rom-r5/Module.symvers
X5=$W/firmware/extracted/kernel-r5-20260930/modules
for m in videodev mc videobuf2-common videobuf2-v4l2 videobuf2-dma-sg v4l2-fwnode v4l2-async; do
  a=$(modinfo -F srcversion $X5/$m.ko); b=$(find $O5/modinst -name $m.ko | head -1); b=$(modinfo -F srcversion $b 2>/dev/null)
  [ "$a" = "$b" ] && echo "provider $m srcversion $a = staged r5" || { echo "PROVIDER MISMATCH $m $a vs $b"; bad=1; }
done
cp $P/camss-sdm660-rom1.patch $P/camss_rom1_patch.py $A/
{ echo "camss rom1 built $(date -Iseconds) B=$B"; echo "v67: make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=<camss>"; echo "r5:  LOCALVERSION=+ make -C $K5 O=$O5 ARCH=arm64 LLVM=1 W=1 M=<camss>";
  echo "source = baseline camss + camss-sdm660-rom1.patch (= camfix5 cumulative + camss_rom1_patch.py)"; } > $A/build-info.txt
( cd $A && sha256sum v67/qcom-camss.ko r5/qcom-camss.ko camss-sdm660-rom1.patch camss_rom1_patch.py build-info.txt > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && cat SHA256SUMS )
echo "B=$B"
[ $bad = 0 ] && echo ROM1_BUILD_PASS || echo ROM1_BUILD_FAIL
