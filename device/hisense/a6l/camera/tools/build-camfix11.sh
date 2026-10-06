#!/bin/bash
# camfix11 build (29 Sep 2026): camfix + camfix2..camfix10 + camfix11 (CSID packet diagnostics, CORE_CTRL_1 / CID LUT /
# IRQ mask / settle / COMMON_CTRL7 / lane_assign knobs: qcom_camss.a6l_v11*) -> qcom-camss.ko, plus imx576_a6l.ko and
# s5k3t1.ko (a6l_regs / a6l_rd). Output: firmware/extracted/camera-20260929-camfix11 (all other files byte-identical to
# camera-20260930-camfix10 = laptop camera12). Laptop staging (camera13): tools/stage-camfix11-laptop.sh.
# Never touches out-a6l-rom-r5 or the v67 .config: M= module builds in /home/a6l/camfix11*.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
B=/home/a6l/camfix11; A=$W/firmware/extracted/camera-20260929-camfix11; OLD=$W/firmware/extracted/camera-20260930-camfix10
P=$W/device/hisense/a6l/kernel/camera
export PATH="$CL:$PATH" KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
step(){ echo; echo "=== $(date +%T) $*"; }
[ -e $B ] && B=$B-$(date +%H%M%S)  # never delete an earlier build dir
mkdir -p $B/camss $B/orig $B/src-snapshot $B/mod $B/modpre $A/extra
step sources+patch
cp -r $K/drivers/media/platform/qcom/camss/. $B/camss/
cp -r $K/drivers/media/platform/qcom/camss $B/orig/camss; cp $K/drivers/media/i2c/hi846.c $B/hi846.c
for f in camfix camfix2 camfix3 camfix4 camfix5 camfix6 camfix7 camfix8 camfix9 camfix9b camfix10 camfix11; do
  if [ $f = camfix ]; then python3 $P/patches/${f}_patch.py $B/camss $B/hi846.c; else python3 $P/patches/${f}_patch.py $B/camss; fi \
    > $B/$f.patchlog 2>&1 || { cat $B/$f.patchlog; echo ${f^^}_PATCH_FAIL; exit 1; }
done
cat $B/camfix11.patchlog
(cd $B && diff -ru orig/camss camss) > $P/patches/camss-sdm660-camfix11-cumulative.patch
wc -l $P/patches/camss-sdm660-camfix11-cumulative.patch
step camss module W=1
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/camss -k modules > $B/camss-build.log 2>&1
grep -E "error|undefined" $B/camss-build.log | head -20
echo "warnings total $(grep -c 'warning:' $B/camss-build.log), in a6l code: $(grep 'warning:' $B/camss-build.log | grep -c -i 'a6l')"
grep 'warning:' $B/camss-build.log | head -10
ls -la $B/camss/qcom-camss.ko || { echo CAMFIX11_BUILD_FAIL; exit 1; }
modinfo $B/camss/qcom-camss.ko | grep -E "^vermagic|^parm: *a6l_(v10|v10_tpg|v11[a-z0-9_]*):" | cut -c1-110
step sensor modules W=1 '(imx576_a6l + s5k3t1, same v4l2-cci build recipe as camfix7)'
cp $P/imx576_a6l.c $P/s5k3t1.c $B/mod/; cp $K/drivers/media/v4l2-core/v4l2-cci.c $B/mod/
printf 'ccflags-y += -DCONFIG_V4L2_CCI_MODULE=1 -DCONFIG_V4L2_CCI_I2C_MODULE=1\nobj-m += v4l2-cci.o imx576_a6l.o s5k3t1.o\n' > $B/mod/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$B/mod -k modules > $B/mod-build.log 2>&1
grep -E "error|warning|undefined" $B/mod-build.log | head -20
ls -la $B/mod/imx576_a6l.ko $B/mod/s5k3t1.ko || { echo SENSOR_BUILD_FAIL; exit 1; }
# provenance: the pre-camfix11 sources (camfix11 block stripped) must reproduce the bundled modules' srcversion
python3 - $B <<'PY'
import sys, re
b = sys.argv[1]
for fn in ['imx576_a6l.c', 's5k3t1.c']:
    s = open(f'{b}/mod/{fn}').read()
    i = s.index('/*\n * A6L camfix11: sensor-side knobs'); j = s.index('static int sns_enable_streams(struct v4l2_subdev *sd,\n')
    s = s[:i] + s[j:]
    s = s.replace("\tret = a6l_sns_extra(s); /* camfix11 */\n\tif (ret)\n\t\tgoto error;\n\n", "")
    s = s.replace('#include <linux/slab.h>\n#include <linux/string.h>\n', '')
    open(f'{b}/modpre/{fn}', 'w').write(s)
PY
cp $B/mod/v4l2-cci.c $B/mod/Makefile $B/modpre/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/modpre -k modules > $B/modpre-build.log 2>&1
for m in imx576_a6l s5k3t1; do
  echo "srcversion $m pre-camfix11 $(modinfo -F srcversion $B/modpre/$m.ko) bundled $(modinfo -F srcversion $OLD/$m.ko) new $(modinfo -F srcversion $B/mod/$m.ko)"
done
for m in $B/mod/imx576_a6l.ko $B/mod/s5k3t1.ko; do modinfo $m | grep -E "^vermagic|^parm: *a6l_(hts|regs|rd):" | cut -c1-110; done
llvm-nm -u $B/mod/s5k3t1.ko | grep -E "kstrdup|kfree|kstrtouint|strsep|cci_" | tr '\n' ' '; echo
for s in $(llvm-nm -u $B/mod/s5k3t1.ko $B/mod/imx576_a6l.ko | awk '{print $2}' | grep -E "^(kstrdup|kfree|kstrtouint|strsep|strchr)$" | sort -u); do grep -q -w "$s" $O/Module.symvers && echo "sym $s OK" || echo "sym $s MISSING(builtin?)"; done
llvm-strip --strip-debug $B/camss/qcom-camss.ko $B/mod/imx576_a6l.ko $B/mod/s5k3t1.ko
step assemble $A
cp $B/camss/*.c $B/camss/*.h $B/src-snapshot/ 2>/dev/null; cp $B/mod/imx576_a6l.c $B/mod/s5k3t1.c $B/src-snapshot/
for f in $(cd $OLD && ls); do [ -f $OLD/$f ] && cp $OLD/$f $A/; done
cp $OLD/extra/a6l_cam_ovl.ko $A/extra/
cp $B/camss/qcom-camss.ko $B/mod/imx576_a6l.ko $B/mod/s5k3t1.ko $A/
cp $W/device/hisense/a6l/camera/run-camera.sh $A/
(cd $A && sha256sum *.ko extra/a6l_cam_ovl.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS; sha256sum qcom-camss.ko imx576_a6l.ko s5k3t1.ko run-camera.sh SHA256SUMS; echo "entries $(ls | wc -l) sums $(wc -l < SHA256SUMS)"; sha256sum -c SHA256SUMS 2>&1 | grep -c ': OK$')
echo "B=$B"
echo CAMFIX11_BUILD_DONE
