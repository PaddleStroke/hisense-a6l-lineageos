#!/bin/bash
# diag-r5 (30 Sep 2026): build the kernel bisect variants from the r5 tree (/home/a6l/kernel/a6l-rom-r5-src, unchanged).
#   k1  = r5p .config + configs/a6l-diag-k1.config  (KFENCE=n, BUG_ON_DATA_CORRUPTION=n). NOT module-compatible with r5
#         although Module.symvers only loses the 2 KFENCE exports: KFENCE=n removes _SLAB_SKIP_KFENCE from enum _slab_flag_bits,
#         so every later SLAB_* flag bit shifts (QEMU: r5 fuse.ko on k1 -> "__kmem_cache_create_args(fuse_inode) failed -22").
#         -> own module set like r5m
#   r5m = V67 .config + rom-v2 fragment + configs/a6l-diag-r5m.config (MODVERSIONS, thermal delay, CFI permissive) = r5
#         WITHOUT the android fragment; own modules (in-tree kit set + OOT tps65185, panel-ft8719, panel-a6l-epd-dsi(v73 src))
# Detached; each variant writes $O/DONE. usage: build-diag-r5-kvariants.sh <k1|r5m> [oot]  (oot = only the out-of-tree modules)
set -uo pipefail
V=$1; R=/mnt/c/Users/Pierre/Desktop/A6L; X=$R/firmware/extracted; T=/home/a6l/kernel/a6l-rom-r5-src
O=/home/a6l/kernel/out-a6l-diag-$V; C=$R/device/hisense/a6l/kernel/configs
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
export LOCALVERSION=+ KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-30 00:00:00 UTC'
mkdir -p $O; rm -f $O/DONE
[ "${2:-}" = oot ] || {
if [ $V = k1 ]; then cp $X/kernel-r5p-20260930/config $O/.config.base; tr -d '\r' < $C/a6l-diag-k1.config > $O/frag1
  frags="$O/frag1"
else cp $X/phone-kernel-v67-candidate-20260919/config $O/.config.base; tr -d '\r' < $X/kernel-r5-20260930/frag-rom-v2.config > $O/frag1
  tr -d '\r' < $C/a6l-diag-r5m.config > $O/frag2; frags="$O/frag1 $O/frag2"; fi
cd $T; ARCH=arm64 LLVM=1 scripts/kconfig/merge_config.sh -m -O $O $O/.config.base $frags
make O=$O ARCH=arm64 LLVM=1 olddefconfig
echo "== diffconfig base -> $V"; scripts/diffconfig $O/.config.base $O/.config
echo "== diffconfig r5p -> $V"; scripts/diffconfig $X/kernel-r5p-20260930/config $O/.config
} > $O/merge.log 2>&1
cd $T
if [ "${2:-}" != oot ]; then
make O=$O ARCH=arm64 LLVM=1 -j8 Image.gz > $O/image-build.log 2>&1 || { echo IMAGE_FAIL > $O/DONE; exit 1; }
make O=$O ARCH=arm64 LLVM=1 -j8 modules > $O/modules-build.log 2>&1 || { echo MOD_FAIL > $O/DONE; exit 1; }
fi
if [ ! -e $O/oot/tps.log ] || [ "${2:-}" = oot ]; then W=$O/oot; rm -rf $W; mkdir -p $W
  bld() { local n=$1 s=$2 objs=$3; mkdir -p $W/$n; cp -a $s/. $W/$n/; ( cd $W/$n && rm -f *.ko *.o *.mod *.mod.c *.cmd .*.cmd Module.symvers modules.order )
    if [ "$objs" != - ]; then rm -f $W/$n/Makefile; { grep -h ccflags $s/Kbuild $s/Makefile 2>/dev/null || true; echo "obj-m += $objs"; } > $W/$n/Kbuild; fi
    make -C $T O=$O ARCH=arm64 LLVM=1 -j8 M=$W/$n modules > $W/$n.log 2>&1 || { echo OOT_FAIL_$n > $O/DONE; exit 1; }; }
  bld epd /home/a6l/kernel/a6l-epd-v73 -
  bld ft8719 /home/a6l/kernel/a6l-display-modules-v67 panel-ft8719-tianma-1080x2340.o
  bld tps /home/a6l/kernel/a6l-extra-modules-v67 tps65185.o
  bld xt_quota2 $R/device/hisense/a6l/kernel/android-gaps/xt_quota2 -          # base group (kernel-gaps sources)
  bld uid_sys_stats $R/device/hisense/a6l/kernel/android-gaps/uid_sys_stats -
fi
echo OK > $O/DONE
