#!/usr/bin/env bash
# merge r4 (agent merge, 28 Sep 2026): OFFLINE builds + checks for the r4 merge. NO ROM build (no m), nothing touches the phone.
# WSL, as a6l, via nohup (relay mg28-*). Builds the two new r4 prebuilts into firmware/extracted/merge-20260928:
#   qcom_smbx.ko      (A6L patch with the stock FCC upper bound 2.4 A; out-of-tree against out-a6l-phone-v67)
#   a6l-qmi-static    (NDK static a6l-qmi from the CURRENT radio sources: pinsafe PIN guard + provision1)
# then runs: radio host tests (g++), V75 DT build into scratch + runtime-overlay check, kernel series composition,
# staging DRY RUN (A6L_STAGE_DRYRUN, Lineage tree untouched), power sim, shell syntax, offline sepolicy check.
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
L=/home/a6l/android/a6l-lineage24; CL=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
export PATH=$CL:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build PYTHONDONTWRITEBYTECODE=1
B=/tmp/mg28; A=$R/firmware/extracted/merge-20260928; D=$R/device/hisense/a6l
rm -rf $B; mkdir -p $B $A; FAIL=""
f() { FAIL="$FAIL $1"; echo "STEP_FAIL $1"; }
echo "== 1. qcom_smbx.ko (stock FCC bound)"
SM=$B/smbx; mkdir -p $SM/a; cp $K/drivers/power/supply/qcom_smbx.c $SM/a/; tr -d '\r' < $D/kernel/power/smbx/qcom_smbx-a6l-fcc-jeita.patch > $SM/p.patch
(cd $SM/a && patch -p4 --no-backup-if-mismatch < $SM/p.patch) || f smbx-patch
printf 'obj-m += qcom_smbx.o\n' > $SM/a/Kbuild
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 M=$SM/a modules > $SM/build.log 2>&1 || { grep -a -B2 -A8 error $SM/build.log | head -30; f smbx-build; }
echo "warnings: $(grep -a -c 'warning:' $SM/build.log)"
if [ -f $SM/a/qcom_smbx.ko ]; then
  llvm-strip --strip-debug -o $SM/qcom_smbx.ko $SM/a/qcom_smbx.ko && cp $SM/qcom_smbx.ko $A/qcom_smbx.ko   # strip locally (no chmod on /mnt/c)
  modinfo -F parm $A/qcom_smbx.ko | grep fcc_max_ua; modinfo -F vermagic $A/qcom_smbx.ko
  modinfo -F parm $A/qcom_smbx.ko | grep -q '2400000 = stock' || f smbx-parm
  case "$(modinfo -F vermagic $A/qcom_smbx.ko)" in "7.2.3-a6l-probe+ "*) ;; *) f smbx-vermagic;; esac
  for s in $(llvm-nm -u $A/qcom_smbx.ko | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || echo "  UNRESOLVED $s"; done
  # FCC register: FAST_CHARGE_CURRENT_CFG = uA / CURRENT_SCALE_FACTOR must fit the field
  grep -n "define CURRENT_SCALE_FACTOR\|define FAST_CHARGE_CURRENT_CFG\|FAST_CHARGE_CURRENT_SETTING_MASK" $SM/a/qcom_smbx.c | head
fi
echo "== 2. a6l-qmi-static (NDK r27c, current sources)"
$NDK/aarch64-linux-android34-clang++ -std=c++17 -O2 -static -Wall -Wextra -Werror -DA6L_QMI_NO_LIBLOG -I$D/radio/qmi/include \
  $D/radio/qmi/src/*.cc $D/radio/tools/a6l_qmi_cli.cc -o $B/a6l-qmi-static && $NDK/llvm-strip $B/a6l-qmi-static && cp $B/a6l-qmi-static $A/ || f qmi-static
strings $A/a6l-qmi-static | grep -c -i "provision1\|A6L_PIN_MIN_RETRIES" | sed 's/^/provision1|pin-guard strings: /'
echo "== 3. radio host tests (g++, ASan/UBSan)"
cp -r $D/radio $B/radio; find $B/radio -type f \( -name '*.cc' -o -name '*.cpp' -o -name '*.h' -o -name '*.sh' \) -exec sed -i 's/\r$//' {} +
( cd $B/radio && CXX=g++ OUT=$B/qmi-tests sh tests/run-host-tests.sh ) > $B/host-tests.txt 2>&1; rc=$?; echo "host tests rc=$rc"
grep -aE "PASS|FAIL|passed|failed" $B/host-tests.txt | tail -12; [ $rc = 0 ] || f host-tests
echo "== 4. V75 DT (scratch) + runtime-overlay check"
bash <(tr -d '\r' < $R/tools/build-rom-v2-dt.sh) $B/dt > $B/dt.log 2>&1 || f dt
grep -E "^merged|A6L_|status=|front_als|vibrator:" $B/dt.log
python3 <(tr -d '\r' < $D/power/tools/check-runtime-overlay.py) $R/firmware/extracted/recovery-v74-candidate-20260923/base.dtb $B/dt/dtbo/a6l-charger-v75.dtbo $B/dt/dtbo/a6l-audio-mics-v75.dtbo 2>&1 | tail -6
fdtget $B/dt/rom-v2.dtb /battery constant-charge-current-max-microamp | sed 's/^/ROM DT battery ccc (DCP ICL) = /'
for p in /soc@0/spmi@800f000/pmic@0/adc@4500 /soc@0/spmi@800f000/pmic@0/charger@1000 /soc@0/spmi@800f000/pmic@0/battery@4000; do echo "$p status=$(fdtget $B/dt/rom-v2.dtb $p status 2>/dev/null || echo '(none=okay)')"; done
fdtget $B/dt/rom-v2.dtb /sound audio-routing | tr ' ' '\n' | grep -A1 -x AMIC3 | tr '\n' ' '; echo
echo "== 5. kernel series"
A6L_KSERIES_DIR=$B/kseries bash <(tr -d '\r' < $R/tools/check-rom-v2-kernel-series.sh) > $B/kseries.log 2>&1 || f kseries
grep -E "applied|UNRESOLVED|warnings|A6L_KSERIES" $B/kseries.log
echo "== 6. staging dry run"
A6L_STAGE_DRYRUN=$B/stage bash <(tr -d '\r' < $R/tools/stage-rom-v2-prebuilts.sh) > $B/stage.log 2>&1 || f stage-dryrun
grep -E "DRY RUN|MODULE_ORDER|^modules:|OVERRIDE|CONFLICT|MISSING|VERMAGIC|STAGE_ROM_V2" $B/stage.log | cut -c1-220
P=$B/stage/device/hisense/a6l/rom/prebuilt/vendor
for m in ipa2_lite qcom-camss qcom_smbx btqca snd-soc-msm8916-analog q6mvm stk3338_a6l a6l_gpio_vib q6adm; do echo "  $m.ko $(sha256sum < $P/lib/modules/$m.ko | cut -c1-16)"; done
echo "  a6l-qmi-static $(sha256sum < $P/a6l/tools/a6l-qmi-static | cut -c1-16)"
grep -c "" $B/stage/device/hisense/a6l/rom/rom-files.mk | sed 's/^/  rom-files.mk lines: /'
echo "== 7. power sim + shell syntax"
T=$B/p; mkdir -p $T/bundle $T/rom; tr -d '\r' < $D/power/bundle/run-power.sh > $T/bundle/run-power.sh; tr -d '\r' < $D/power/rom/a6l-chg-guard.sh > $T/rom/a6l-chg-guard.sh
bash <(tr -d '\r' < $D/power/test/sim-power.sh) $T | tail -3; bash <(tr -d '\r' < $D/power/test/sim-power.sh) $T | tail -1 | grep -q PASS || f power-sim
for s in rom/bin/a6l-radio.sh rom/bin/a6l-modules.sh rom/bin/a6l-logcat.sh power/rom/a6l-chg-guard.sh; do tr -d '\r' < $D/$s > $B/x.sh; dash -n $B/x.sh && bash -n $B/x.sh && echo "syntax ok $s" || f syntax-$s; done
for s in tools/rom-v2-pipeline.sh tools/stage-rom-v2-prebuilts.sh tools/build-rom-v2-dt.sh tools/check-rom-v2-kernel-series.sh tools/build-merge-r4.sh; do tr -d '\r' < $R/$s > $B/x.sh; bash -n $B/x.sh && echo "syntax ok $s" || f syntax-$s; done
echo "== 8. sepolicy (offline, r3 intermediates)"
for v in userdebug user; do A6L_SEPCHECK_DIR=$B/sep-$v bash <(tr -d '\r' < $R/tools/release/check-a6l-sepolicy.sh) $v rom/sepolicy/vendor > $B/sep-$v.log 2>&1; grep -a "A6L_SEPOLICY_CHECK" $B/sep-$v.log | tail -1 | sed "s/^/$v: /"; grep -aq "A6L_SEPOLICY_CHECK PASS" $B/sep-$v.log || f sepolicy-$v; done
echo "== 9. assemble"
( cd $A && sha256sum qcom_smbx.ko a6l-qmi-static > SHA256SUMS; cat SHA256SUMS )
cp $B/host-tests.txt $B/kseries.log $B/stage.log $B/dt.log $A/ 2>/dev/null
[ -z "$FAIL" ] && echo A6L_MERGE_R4_CHECKS_PASS || echo "A6L_MERGE_R4_CHECKS_FAIL:$FAIL"
