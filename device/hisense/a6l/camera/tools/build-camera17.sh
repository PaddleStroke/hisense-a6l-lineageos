#!/bin/bash
# camera17 build (29 Sep 2026, docs/hi846-20260929.md round 2): hi846 s_ctrl fix + runtime-only diagnostics, NO rmmod ever.
#  - hi846.ko   = mainline 7.2.3 + hi846-4lane-default.patch + camfix16_hi846_patch.py + camfix17_hi846_patch.py
#                 (a6l_fix default 1 = THE fix; a6l_rd/a6l_rdx/a6l_mclk/a6l_stock runtime knobs)
#  - qcom-camss = rom1 as in camera15/16 (a6l_wm 6; source /home/a6l/camss-rom1-132720, srcversion checked against the
#                 camera16 bundle) + camss_diag17_patch.py (a6l_c17*, all default off)
#  - also writes patches/hi846-a6l-ctrlfix.patch = the bare fix on mainline+4lane (for the ROM hi846.ko, not applied here)
# M= builds only against v67 (never touches out-a6l-rom-r5, the v67 .config or the Android out dir). Never deletes.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
P=$W/device/hisense/a6l/kernel/camera
SRC=$W/firmware/extracted/camera-20260929-hi846; A=$W/firmware/extracted/camera-20260929-hi846b
R1=/home/a6l/camss-rom1-132720/camss
B=/home/a6l/c17-$(date +%H%M%S); echo $B > /home/a6l/c17.last
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
bad=0
mkdir -p $B/h846base $B/h846fix $B/h846 $B/camss-rom1 $B/camss
step hi846 sources
cp $K/drivers/media/i2c/hi846.c $B/h846base/hi846.c
(cd $B/h846base && patch -s -p0 --no-backup-if-mismatch hi846.c < $P/patches/hi846-4lane-default.patch) || { echo C17_FAIL 4lane; exit 1; }
cp $B/h846base/hi846.c $B/h846/hi846.c
python3 $P/patches/camfix16_hi846_patch.py $B/h846/hi846.c || exit 1
python3 $P/patches/camfix17_hi846_patch.py $B/h846/hi846.c || exit 1
# bare ROM fix (no knobs): the one line
python3 - $B/h846base/hi846.c $B/h846fix/hi846.c <<'PY'
import sys
s=open(sys.argv[1]).read()
old="""	ret = pm_runtime_get_if_in_use(&client->dev);
	if (!ret || ret == -EAGAIN)
		return 0;
"""
assert s.count(old)==1
s=s.replace(old, old+"""	ret = 0; /* A6L: pm_runtime_get_if_in_use() returned 1; do not leak it into the s_ctrl result */
""")
open(sys.argv[2],'w').write(s); print("CTRLFIX_OK")
PY
(cd $B && diff -u h846base/hi846.c h846fix/hi846.c | sed '1,2s#\t.*##') > $P/patches/hi846-a6l-ctrlfix.patch
(cd $B && diff -u h846base/hi846.c h846/hi846.c | sed '1,2s#\t.*##') > $P/patches/hi846-camfix17-cumulative.patch
wc -l $P/patches/hi846-a6l-ctrlfix.patch $P/patches/hi846-camfix17-cumulative.patch
for d in h846 h846fix; do echo 'obj-m += hi846.o' > $B/$d/Kbuild; make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/$d modules > $B/$d-build.log 2>&1 || { tail -20 $B/$d-build.log; echo C17_FAIL $d build; exit 1; }
  echo "$d warnings: $(grep -c 'warning:' $B/$d-build.log)"; grep 'warning:' $B/$d-build.log | head -5; done
step camss rom1 source + srcversion check
( cd $R1 && cp Makefile Kconfig *.c *.h $B/camss-rom1/ )
cp -r $B/camss-rom1/. $B/camss/
grep -n "^int a6l_wm" $B/camss-rom1/camss-vfe-4-8.c
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/camss-rom1 -j8 modules > $B/camss-rom1-build.log 2>&1 || { tail -20 $B/camss-rom1-build.log; echo C17_FAIL rom1 rebuild; exit 1; }
a=$(modinfo -F srcversion $SRC/qcom-camss.ko); b=$(modinfo -F srcversion $B/camss-rom1/qcom-camss.ko)
[ "$a" = "$b" ] && echo "ROM1_SRC_MATCH srcversion $a (= camera16 bundle qcom-camss.ko)" || { echo "ROM1_SRC_MISMATCH bundle $a rebuilt $b"; bad=1; }
python3 $P/patches/camss_diag17_patch.py $B/camss || exit 1
(cd $B && diff -ru camss-rom1 camss -x '*.o' -x '*.ko' -x '*.mod*' -x 'modules.order' -x 'Module.symvers' -x '.*' ) > $P/patches/camss-sdm660-diag17-on-rom1.patch
wc -l $P/patches/camss-sdm660-diag17-on-rom1.patch
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/camss -j8 modules > $B/camss-build.log 2>&1 || { tail -30 $B/camss-build.log; echo C17_FAIL camss build; exit 1; }
echo "camss warnings: $(grep -c 'warning:' $B/camss-build.log) (rom1 rebuild: $(grep -c 'warning:' $B/camss-rom1-build.log))"; grep 'warning:' $B/camss-build.log | head -8
step checks
for m in $B/h846/hi846.ko $B/camss/qcom-camss.ko; do
  v=$(modinfo -F vermagic $m); [ "$v" = "7.2.3-a6l-probe+ SMP preempt mod_unload aarch64" ] || { echo "VERMAGIC BAD $m: $v"; bad=1; }
  echo "$(basename $m) srcversion $(modinfo -F srcversion $m) depends $(modinfo -F depends $m)"; modinfo -F parm $m | sed 's/^/  parm /' | cut -c1-110
  for s in $(llvm-nm -u $m | awk '{print $2}'); do grep -qP "\t$s\t" $O/Module.symvers || { echo "UNRESOLVED $s in $m"; bad=1; }; done
done
[ "$(modinfo -F depends $B/camss/qcom-camss.ko)" = "$(modinfo -F depends $SRC/qcom-camss.ko)" ] && echo "camss depends identical to rom1" || { echo "CAMSS DEPENDS CHANGED"; bad=1; }
llvm-strip --strip-debug $B/h846/hi846.ko $B/camss/qcom-camss.ko $B/h846fix/hi846.ko
step assemble $A
mkdir -p $A/extra
for f in $(cd $SRC && ls); do [ -f $SRC/$f ] && cp $SRC/$f $A/; done
for f in a6l_cam_ovl.ko a6l_cam_ovl-camera15.ko hi846-rom.ko; do cp $SRC/extra/$f $A/extra/; done
cp $SRC/qcom-camss.ko $A/extra/qcom-camss-rom1.ko
cp $SRC/hi846.ko $A/extra/hi846-camfix16.ko
cp $B/h846fix/hi846.ko $A/extra/hi846-ctrlfix.ko
cp $B/h846/hi846.ko $A/hi846.ko
cp $B/camss/qcom-camss.ko $A/qcom-camss.ko
tr -d '\r' < $W/device/hisense/a6l/camera/run-camera17.sh > $A/run-camera.sh
cp $P/patches/camfix17_hi846_patch.py $P/patches/camss_diag17_patch.py $P/patches/hi846-a6l-ctrlfix.patch $A/extra/
(cd $A && sha256sum *.ko extra/*.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS
 echo "sums $(wc -l < SHA256SUMS) ok $(sha256sum -c SHA256SUMS | grep -c ': OK$')"; sha256sum qcom-camss.ko hi846.ko extra/hi846-ctrlfix.ko run-camera.sh SHA256SUMS)
echo "B=$B"; [ $bad = 0 ] && echo CAMERA17_BUILD_PASS || echo CAMERA17_BUILD_FAIL
