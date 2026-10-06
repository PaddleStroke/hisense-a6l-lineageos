#!/bin/bash
# hi846 ROM merge (29 Sep 2026, docs/hi846-20260929.md round 3): the ROM hi846.ko = mainline 7.2.3 hi846.c +
# kernel/camera/patches/hi846-set-ctrl-fix.patch (= docs/upstream/hi846-set-ctrl.patch; the 4-lane patch is dropped,
# the ROM DT is 2-lane). Builds M= copies only (never writes the kernel trees or their out dirs):
#   v67: make -C a6l-baseline-7.2 O=out-a6l-phone-v67;  r5: LOCALVERSION=+ make -C a6l-rom-r5-src O=out-a6l-rom-r5
# W=1 must give 0 warnings; every import is checked against the kernel's Module.symvers (r5: CRCs, MODVERSIONS=y);
# llvm-strip --strip-debug; output firmware/extracted/hi846-rom-20260929/{v67,r5}/hi846.ko + SHA256SUMS + build-info.txt
set -euo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; KD=$W/device/hisense/a6l/kernel
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
B=/home/a6l/scratch/hi846-rom-$(date +%H%M%S); OUT=$W/firmware/extracted/hi846-rom-20260929
mkdir -p $B/src/drivers/media/i2c $OUT/v67 $OUT/r5
cp /home/a6l/kernel/a6l-baseline-7.2/drivers/media/i2c/hi846.c $B/src/drivers/media/i2c/
tr -d '\r' < $KD/camera/patches/hi846-set-ctrl-fix.patch > $B/fix.patch
( cd $B/src && patch -s -p1 --no-backup-if-mismatch < $B/fix.patch )
grep -q $'^\tret = 0;$' $B/src/drivers/media/i2c/hi846.c && ! grep -q 'nr_lanes == 4' $B/src/drivers/media/i2c/hi846.c || { echo H846ROM_FAIL source; exit 1; }
bad=0
for k in v67 r5; do
  case $k in
    v67) K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67; LV=; VM="7.2.3-a6l-probe+ SMP preempt mod_unload aarch64";;
    r5)  K=/home/a6l/kernel/a6l-rom-r5-src;   O=/home/a6l/kernel/out-a6l-rom-r5;   LV=+; VM="7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64";;
  esac
  M=$B/$k; mkdir -p $M; cp $B/src/drivers/media/i2c/hi846.c $M/; echo 'obj-m += hi846.o' > $M/Kbuild
  LOCALVERSION=$LV make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$M modules > $M/build.log 2>&1 || { tail -20 $M/build.log; echo "H846ROM_FAIL $k build"; exit 1; }
  nw=$(grep -c 'warning:' $M/build.log || true); [ "$nw" = 0 ] || { grep 'warning:' $M/build.log | head; bad=1; }
  v=$(modinfo -F vermagic $M/hi846.ko); [ "$v" = "$VM" ] || { echo "VERMAGIC $k: $v"; bad=1; }
  for s in $(llvm-nm -u $M/hi846.ko | awk '{print $2}'); do grep -qP "\t$s\t" $O/Module.symvers || { echo "UNRESOLVED $k $s"; bad=1; }; done
  if [ $k = r5 ]; then python3 - $M/hi846.ko $O/Module.symvers <<'PY' || bad=1
import sys, subprocess
ko, sv = sys.argv[1], sys.argv[2]
crc = {l.split('\t')[1]: int(l.split('\t')[0], 16) for l in open(sv) if '\t' in l}
b = open(ko, 'rb').read()
# __versions: struct modversion_info { unsigned long crc (8 bytes on arm64); char name[56]; } (7.x: may be __version_ext)
out = subprocess.run(['llvm-readelf', '-S', '-W', ko], capture_output=True, text=True).stdout
sec = [l for l in out.splitlines() if '__versions' in l]
if not sec: print('NO __versions (check __version_ext)'); sys.exit(0)
f = sec[0].split(); i = f.index('__versions'); off = int(f[i+3], 16); size = int(f[i+4], 16)
bad = 0; n = 0
for p in range(off, off + size, 64):
    c = int.from_bytes(b[p:p+8], 'little') & 0xffffffff; name = b[p+8:p+64].split(b'\0')[0].decode()
    n += 1
    if name in crc and crc[name] != c: print('CRC MISMATCH', name); bad = 1
print(f'CRC_CHECK {n} imports', 'OK' if not bad else 'BAD'); sys.exit(bad)
PY
  fi
  cp $M/hi846.ko $B/hi846-$k.ko; llvm-strip --strip-debug $B/hi846-$k.ko; cp $B/hi846-$k.ko $OUT/$k/hi846.ko
  echo "$k hi846.ko warnings=$nw srcversion=$(modinfo -F srcversion $M/hi846.ko) vermagic='$v' sha=$(sha256sum < $OUT/$k/hi846.ko | cut -c1-16)"
done
[ $bad = 0 ] || { echo H846ROM_FAIL checks; exit 1; }
cp $B/fix.patch $OUT/hi846-set-ctrl-fix.patch
{ echo "hi846 ROM module built $(date -Iseconds) B=$B (device/hisense/a6l/camera/tools/build-hi846-rom.sh)"
  echo "source = /home/a6l/kernel/a6l-baseline-7.2 drivers/media/i2c/hi846.c (= upstream master) + hi846-set-ctrl-fix.patch; NO 4-lane patch"
  echo "v67: make -C a6l-baseline-7.2 O=out-a6l-phone-v67 ARCH=arm64 LLVM=1 W=1 M=<copy>; r5: LOCALVERSION=+ make -C a6l-rom-r5-src O=out-a6l-rom-r5 ... (CRCs checked)"
  echo "llvm-strip --strip-debug. Staged by tools/stage-rom-v2-prebuilts.sh (v67 via setko, r5 after the kernel-r5 replacement)"; } > $OUT/build-info.txt
( cd $OUT && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | sort | xargs sha256sum > SHA256SUMS; cat SHA256SUMS )
echo H846ROM_PASS
