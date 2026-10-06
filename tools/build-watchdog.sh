#!/usr/bin/env bash
# build-watchdog.sh (cpufreq-watchdog agent, 29 Sep 2026; H64; docs/cpufreq-watchdog-20260929.md). WSL only.
#   build : qcom-wdt.ko out-of-tree (W=1) for V67 (out-a6l-phone-v67) and r5 (out-a6l-rom-r5) from each tree's own
#           drivers/watchdog/qcom-wdt.c, CRCs of r5 imports checked against Module.symvers; softdog.ko (r5, QEMU stand-in);
#           a6l_wdtctl + wdt-qemu-init (NDK static aarch64) -> firmware/extracted/wdt-20260929 (+ SHA256SUMS)
#   qemu  : boots <Image> (default r5 Image; A6L_WDT_IMAGE overrides, e.g. the CPR r5 Image) in qemu-system-aarch64 virt with a
#           tiny initramfs: qcom-wdt.ko must load, a6l_wdtctl info / watchdogd contract (30 s) / pet run, then a bite
#           that must reset the VM within timeout+3 s. Prints A6L_WDT_QEMU PASS.
set -euo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; X=$R/firmware/extracted/wdt-20260929; W=/home/a6l/kernel/wdt-work
L=/home/a6l/android/a6l-lineage24; export PATH=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin; WD=$R/device/hisense/a6l/watchdog
K5=$R/firmware/extracted/kernel-r5-20260930
declare -A SRC=([v67]=/home/a6l/kernel/a6l-baseline-7.2 [r5]=/home/a6l/kernel/a6l-rom-r5-src)
declare -A OUT=([v67]=/home/a6l/kernel/out-a6l-phone-v67 [r5]=/home/a6l/kernel/out-a6l-rom-r5)
export LOCALVERSION=+
mkdir -p $W $X/v67 $X/r5
for ph in "$@"; do case $ph in
build)
  for k in v67 r5; do
    d=$W/$k/qcom-wdt; rm -rf $d; mkdir -p $d; cp ${SRC[$k]}/drivers/watchdog/qcom-wdt.c $d/; echo 'obj-m := qcom-wdt.o' > $d/Kbuild
    cmp -s ${SRC[r5]}/drivers/watchdog/qcom-wdt.c $d/qcom-wdt.c || echo "NOTE $k qcom-wdt.c differs from r5"
    make -C ${SRC[$k]} O=${OUT[$k]} ARCH=arm64 LLVM=1 W=1 M=$d -j8 modules > $d/build.log 2>&1 || { tail -20 $d/build.log; echo "A6L_WDT_BUILD_FAIL $k"; exit 3; }
    n=$(grep -c 'warning:' $d/build.log || true); echo "$k qcom-wdt.ko W=1 warnings: $n"; [ "$n" = 0 ]
    llvm-strip --strip-debug -o $d/qcom-wdt.stripped.ko $d/qcom-wdt.ko && cp $d/qcom-wdt.stripped.ko $X/$k/qcom-wdt.ko   # strip in WSL fs (drvfs refuses)
    echo "$k $(modinfo -F vermagic $X/$k/qcom-wdt.ko) alias=$(modinfo -F alias $X/$k/qcom-wdt.ko | tr '\n' ' ')"
  done
  # in-tree products of the two kernels must be the same code (sanity: same .text size)
  for k in v67 r5; do [ -f ${OUT[$k]}/drivers/watchdog/qcom-wdt.ko ] && echo "$k in-tree qcom-wdt.ko present ($(size -A ${OUT[$k]}/drivers/watchdog/qcom-wdt.ko | awk '$1==".text"{print $2}') vs oot $(size -A $W/$k/qcom-wdt/qcom-wdt.ko | awk '$1==".text"{print $2}') bytes .text)"; done
  # r5 = MODVERSIONS: every import CRC must equal Module.symvers of the r5 kernel
  python3 - $X/r5/qcom-wdt.ko $K5/Module.symvers <<'PY' || { echo "A6L_WDT_CRC_FAIL"; exit 4; }
import sys, struct
ko = open(sys.argv[1], 'rb').read()
sym = {l.split('\t')[1]: int(l.split('\t')[0], 16) for l in open(sys.argv[2])}
# parse ELF64 section headers to find __versions (struct modversion_info: unsigned long crc; char name[56] on arm64)
e_shoff, = struct.unpack_from('<Q', ko, 0x28); e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', ko, 0x3a)
secs = [struct.unpack_from('<IIQQQQIIQQ', ko, e_shoff + i * e_shentsize) for i in range(e_shnum)]
stroff = secs[e_shstrndx][4]
def name(s): o = stroff + s[0]; return ko[o:ko.index(b'\0', o)].decode()
v = [s for s in secs if name(s) == '__versions']
if not v: print('no __versions section (not a modversions module)'); sys.exit(1)
off, size = v[0][4], v[0][5]; bad = 0; n = 0
for i in range(0, size, 64):
    crc, = struct.unpack_from('<Q', ko, off + i); nm = ko[off + i + 8: off + i + 64].split(b'\0')[0].decode(); n += 1   # {unsigned long crc; char name[56]}
    if nm not in sym or sym[nm] != crc: print('CRC MISMATCH', nm, hex(crc), hex(sym.get(nm, 0))); bad += 1
print(f'r5 qcom-wdt.ko: {n} imports, {bad} CRC mismatches vs kernel-r5 Module.symvers'); sys.exit(1 if bad else 0)
PY
  # softdog (QEMU stand-in only, never staged)
  d=$W/r5/softdog; rm -rf $d; mkdir -p $d; cp ${SRC[r5]}/drivers/watchdog/softdog.c $d/; echo 'obj-m := softdog.o' > $d/Kbuild
  make -C ${SRC[r5]} O=${OUT[r5]} ARCH=arm64 LLVM=1 W=1 M=$d -j8 modules > $d/build.log 2>&1 || { tail -20 $d/build.log; exit 3; }
  echo "softdog W=1 warnings: $(grep -c 'warning:' $d/build.log || true)"; cp $d/softdog.ko $W/softdog-r5.ko
  $NDK/aarch64-linux-android34-clang -O2 -Wall -Wextra -Werror -static $WD/tools/a6l_wdtctl.c -o $W/a6l_wdtctl && $NDK/llvm-strip $W/a6l_wdtctl && cp $W/a6l_wdtctl $X/a6l_wdtctl
  $NDK/aarch64-linux-android34-clang -O2 -Wall -Wextra -Werror -static $WD/tests/wdt-qemu-init.c -o $W/wdt-qemu-init && $NDK/llvm-strip $W/wdt-qemu-init
  file $X/a6l_wdtctl | cut -d, -f1-2
  (cd $X && sha256sum v67/qcom-wdt.ko r5/qcom-wdt.ko a6l_wdtctl > SHA256SUMS); cat $X/SHA256SUMS; echo A6L_WDT_BUILD_PASS ;;
qemu)
  IMG=${A6L_WDT_IMAGE:-$K5/Image}; tag=${A6L_WDT_TAG:-r5}; q=$W/qemu-$tag; rm -rf $q; mkdir -p $q/root/{proc,sys,dev}
  cp $W/wdt-qemu-init $q/root/init; cp $X/a6l_wdtctl $q/root/; cp $X/r5/qcom-wdt.ko $W/softdog-r5.ko $q/root/; mv $q/root/softdog-r5.ko $q/root/softdog.ko
  (cd $q/root && find . | cpio -o -H newc --quiet | gzip -n > $q/initrd.gz)
  t0=$(date +%s)
  timeout 240 qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a53 -smp 2 -m 1024 -nodefaults -display none -serial stdio -monitor none -no-reboot \
     -kernel $IMG -initrd $q/initrd.gz -append "console=ttyAMA0 panic=-1 loglevel=6" > $q/console.log 2>&1 || true
  dt=$(( $(date +%s) - t0 )); tr -d '\r' < $q/console.log | grep -E 'A6L_|softdog|watchdog|Linux version|Kernel panic|disagrees|Unknown symbol' | head -60 || { echo "no matching console lines; tail:"; tail -20 $q/console.log; }
  ok=1; c=$(tr -d '\r' < $q/console.log)
  for want in 'insmod /qcom-wdt.ko ok' 'qcom_wdt driver registered' 'insmod /softdog.ko ok' 'SETTIMEOUT 30 -> 30' 'info  rc=0' \
              'pet 30 rc=0' 'pet 4 rc=0' 'survived the pet run' 'BITE_ARMED timeout=3'; do
    echo "$c" | grep -qF "A6L_QEMU_WDT ${want#A6L_QEMU_WDT }" || echo "$c" | grep -qF "$want" || { echo "MISSING: $want"; ok=0; }; done
  echo "$c" | grep -q 'softdog: Initiating system reboot' && echo "softdog reset message seen" || echo "(softdog reset message not flushed before the reset: normal)"
  # -no-reboot: QEMU only exits on a reset/poweroff. The VM must end <= timeout+1 s after BITE_ARMED (no 'still alive 4 s').
  echo "$c" | grep -q 'still alive 4 s after arming' && { echo "FAIL: VM alive 4 s after arming a 3 s watchdog"; ok=0; }
  [ $dt -lt 200 ] || { echo "FAIL: QEMU did not reset (ran ${dt}s, killed by timeout)"; ok=0; }
  echo "VM reset after arming: last line '$(echo "$c" | grep 'still alive' | tail -1)', VM ran ${dt}s"
  echo "$c" | grep -qE 'disagrees about version|Unknown symbol|FAIL' && { echo "module/CRC or test failure"; ok=0; }
  [ $ok = 1 ] && echo "A6L_WDT_QEMU PASS ($tag)" || { echo "A6L_WDT_QEMU FAIL ($tag)"; exit 5; } ;;
*) echo "unknown phase $ph"; exit 2 ;;
esac; done
