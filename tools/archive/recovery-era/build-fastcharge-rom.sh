#!/usr/bin/env bash
# fastcharge-rom (29 Sep 2026, docs/fastcharge-rom-20260929.md): the ROM qcom_smbx.ko = mainline 7.2.3 qcom_smbx.c +
# kernel/power/smbx/qcom_smbx-a6l-fcc-jeita.patch + qcom_smbx-a6l-hvdcp.patch (power29 = attended QC PASS 30 Sep, + r5 bug
# hunt P4 abort-suspend release / P5 plug-in kick), for BOTH ROM kernels. WSL only, no phone. Builds M= copies only
# (never writes the kernel trees or their out dirs):
#   v67: make -C a6l-baseline-7.2 O=out-a6l-phone-v67;  r5: LOCALVERSION=+ make -C a6l-rom-r5-src O=out-a6l-rom-r5
# W=1 must give 0 warnings (the 'compiler differs' note aside); vermagic checked; every import resolved against the kernel's
# Module.symvers / System.map (r5: export CRCs, MODVERSIONS=y); parameters checked; host HVDCP register test (ASan/UBSan)
# + hvdcp_enable=0 trace; llvm-strip --strip-debug.
# Output: firmware/extracted/fastcharge-rom-20260929/{v67,r5}/qcom_smbx.ko + src/ + SHA256SUMS + build-info.txt
set -euo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; D=$W/device/hisense/a6l; SM=$D/kernel/power/smbx
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build PYTHONDONTWRITEBYTECODE=1
B=/home/a6l/scratch/fastcharge-rom-$(date +%H%M%S); OUT=$W/firmware/extracted/fastcharge-rom-20260929
mkdir -p $B/src $B/smbx $OUT/v67 $OUT/r5 $OUT/src
cp /home/a6l/kernel/a6l-baseline-7.2/drivers/power/supply/qcom_smbx.c $B/src/; cp $B/src/qcom_smbx.c $B/qcom_smbx.c.orig
for p in qcom_smbx-a6l-fcc-jeita.patch qcom_smbx-a6l-hvdcp.patch; do
  tr -d '\r' < $SM/$p > $B/smbx/$p
  ( cd $B/src && patch -s -p4 --no-backup-if-mismatch < $B/smbx/$p ) || { echo "FCROM_FAIL patch $p"; exit 1; }
  if [ $p = qcom_smbx-a6l-fcc-jeita.patch ]; then
    # the r5 tree carries the series up to fcc-jeita: baseline + fcc-jeita must equal it (same driver base on both kernels)
    cmp -s $B/src/qcom_smbx.c /home/a6l/kernel/a6l-rom-r5-src/drivers/power/supply/qcom_smbx.c && echo "baseline+fcc-jeita == r5 tree qcom_smbx.c" \
      || { echo "FCROM_FAIL r5 tree qcom_smbx.c differs from baseline+fcc-jeita"; exit 1; }
  fi
done
S=$B/src/qcom_smbx.c
# markers: power29 (OCP/FLOAT, rerun, regs), P4 (hv_susp released at unplug), P5 (plug-in IRQ kick), stock bounds
for m in hvdcp_rerun hvdcp_regs hv_susp a6l_hv_release_susp 'a6l_hv_wants_events(chip))' 'A6L_FCC_MAX_UA.*2400000' 'A6L_HV_ICL_MAX_UA'; do
  grep -q -- "$m" $S || { echo "FCROM_FAIL marker $m"; exit 1; }; done
bad=0
for k in v67 r5; do
  case $k in
    v67) K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67; LV=; VM="7.2.3-a6l-probe+ SMP preempt mod_unload aarch64";;
    r5)  K=/home/a6l/kernel/a6l-rom-r5-src;   O=/home/a6l/kernel/out-a6l-rom-r5;   LV=+; VM="7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64";;
  esac
  M=$B/$k; mkdir -p $M; cp $S $M/; echo 'obj-m += qcom_smbx.o' > $M/Kbuild
  LOCALVERSION=$LV make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$M modules > $M/build.log 2>&1 || { tail -20 $M/build.log; echo "FCROM_FAIL $k build"; exit 1; }
  nw=$(grep 'warning:' $M/build.log | grep -vc 'compiler differs' || true); [ "$nw" = 0 ] || { grep 'warning:' $M/build.log | head; bad=1; }
  v=$(modinfo -F vermagic $M/qcom_smbx.ko); [ "$v" = "$VM" ] || { echo "VERMAGIC $k: $v"; bad=1; }
  for prm in fcc_max_ua jeita_hard hvdcp_enable hvdcp_max_uv hvdcp_icl_ua hvdcp_status hvdcp_rerun hvdcp_regs; do
    modinfo -F parm $M/qcom_smbx.ko | grep -q "^$prm:" || { echo "PARM $k missing $prm"; bad=1; }; done
  for s in $(llvm-nm -u $M/qcom_smbx.ko | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || { echo "UNRESOLVED $k $s"; bad=1; }; done
  if [ $k = r5 ]; then python3 - $M/qcom_smbx.ko $O/Module.symvers <<'PY' || bad=1
import sys, subprocess
ko, sv = sys.argv[1], sys.argv[2]
crc = {l.split('\t')[1]: int(l.split('\t')[0], 16) for l in open(sv) if '\t' in l}
b = open(ko, 'rb').read()
out = subprocess.run(['llvm-readelf', '-S', '-W', ko], capture_output=True, text=True).stdout
sec = [l for l in out.splitlines() if '__versions' in l]
if not sec: print('NO __versions'); sys.exit(1)
f = sec[0].split(); i = f.index('__versions'); off = int(f[i+3], 16); size = int(f[i+4], 16)
bad = 0; n = 0
for p in range(off, off + size, 64):
    c = int.from_bytes(b[p:p+8], 'little') & 0xffffffff; name = b[p+8:p+64].split(b'\0')[0].decode()
    n += 1
    if name in crc and crc[name] != c: print('CRC MISMATCH', name); bad = 1
print(f'CRC_CHECK {n} imports', 'OK' if not bad else 'BAD'); sys.exit(bad)
PY
  fi
  llvm-strip --strip-debug -o $B/qcom_smbx-$k.ko $M/qcom_smbx.ko; cp $B/qcom_smbx-$k.ko $OUT/$k/qcom_smbx.ko   # (-o onto /mnt/c: chmod EPERM)
  echo "$k qcom_smbx.ko warnings=$nw srcversion=$(modinfo -F srcversion $M/qcom_smbx.ko) vermagic='$v' sha=$(sha256sum < $OUT/$k/qcom_smbx.ko | cut -c1-16)"
done
[ $bad = 0 ] || { echo "FCROM_FAIL checks"; exit 1; }
# host register-level HVDCP test (power/test/hvdcp, 85 checks incl. P4/P5) + hvdcp_enable=0 trace == r4
mkdir -p $B/t/linux/iio
for x in shim.h sim-smb2.c run-hvdcp-sim.sh; do tr -d '\r' < $D/power/test/hvdcp/$x > $B/t/$x; done
( cd $D/power/test/hvdcp/linux && for h in *.h; do tr -d '\r' < $h > $B/t/linux/$h; done; tr -d '\r' < iio/consumer.h > $B/t/linux/iio/consumer.h )
bash $B/t/run-hvdcp-sim.sh $B/qcom_smbx.c.orig $B/smbx > $B/hvdcp-sim.log 2>&1 || { tail -30 $B/hvdcp-sim.log; echo "FCROM_FAIL hvdcp sim"; exit 1; }
grep -E '^(FAIL|A6L_)' $B/hvdcp-sim.log
grep -q 'A6L_HVDCP_SIM PASS' $B/hvdcp-sim.log && grep -q 'A6L_HVDCP_OFF_TRACE SAME' $B/hvdcp-sim.log || { echo "FCROM_FAIL hvdcp sim result"; exit 1; }
cp $B/smbx/*.patch $OUT/src/; cp $B/hvdcp-sim.log $OUT/
{ echo "fastcharge-rom qcom_smbx built $(date -Iseconds) B=$B (tools/build-fastcharge-rom.sh)"
  echo "source = /home/a6l/kernel/a6l-baseline-7.2 drivers/power/supply/qcom_smbx.c + src/qcom_smbx-a6l-fcc-jeita.patch + src/qcom_smbx-a6l-hvdcp.patch (power29 + P4/P5)"
  echo "v67: make -C a6l-baseline-7.2 O=out-a6l-phone-v67 ARCH=arm64 LLVM=1 W=1 M=<copy>; r5: LOCALVERSION=+ make -C a6l-rom-r5-src O=out-a6l-rom-r5 ... (CRCs checked)"
  echo "srcversion v67=$(modinfo -F srcversion $B/v67/qcom_smbx.ko) r5=$(modinfo -F srcversion $B/r5/qcom_smbx.ko); hvdcp sim: $(grep -E '^A6L_' $B/hvdcp-sim.log | tr '\n' ' ')"
  echo "llvm-strip --strip-debug. Staged by tools/stage-rom-v2-prebuilts.sh (v67 via setko, r5 after the kernel-r5 replacement)"; } > $OUT/build-info.txt
( cd $OUT && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | sort | xargs sha256sum > SHA256SUMS; cat SHA256SUMS )
echo FCROM_PASS
