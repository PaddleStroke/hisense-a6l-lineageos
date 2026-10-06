#!/bin/bash
# data2 agent (25 Sep 2026): OFFLINE. ipa2_lite.ko (runtime-PM-safe a6l_diag) + static a6l-qmi (QMI DPM before WDA)
# -> bundle v75/ipa3 = repo firmware/extracted/ipa-20260925/ipa3 (+ laptop ~/A6L-usb-20260915/v75/ipa3). Run with nohup.
exec < /dev/null
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
NDK=/home/a6l/ndk/android-ndk-r27c; NB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
S=$R/device/hisense/a6l/kernel/ipa; RS=$R/device/hisense/a6l/radio
B=/home/a6l/kernel/data2-build; rm -rf $B; mkdir -p $B/ipa2-lite $B/radio
OUTR=$R/firmware/extracted/ipa-20260925; OLD=$R/firmware/extracted/ipa-20260924/ipa2b
fail() { echo "DATA2_BUNDLE_FAIL $*"; exit 1; }
echo "=== 1. ipa2_lite.ko $(date)"
cp $S/ipa2-lite/src/* $B/ipa2-lite/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ipa2-lite W=1 modules -j8 > $B/ipa2-lite-build.log 2>&1 || { tail -30 $B/ipa2-lite-build.log; fail ipa2-lite; }
echo "ipa2-lite warnings: $(grep -c 'warning:' $B/ipa2-lite-build.log)"; grep -A3 'warning:' $B/ipa2-lite-build.log | head -20
m=$B/ipa2-lite/ipa2_lite.ko
echo "vermagic=$(modinfo -F vermagic $m) depends=$(modinfo -F depends $m)"
for s in $($CL/llvm-nm -u $m | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || echo "  UNRESOLVED $s"; done
modinfo -F parm $m | grep -E "diag_pipes|stop_at" || fail "no diag_pipes param"
$CL/llvm-nm $m | grep -E " (pm_runtime_|__pm_runtime)" | head
echo "=== 2. a6l-qmi (static NDK) + host tests $(date)"
cp -r $RS/qmi $RS/tools $RS/tests $B/radio/
( cd $B/radio && CXX=g++ OUT=$B/qmi-tests sh tests/run-host-tests.sh 2>&1 | tail -2 )
$NB/aarch64-linux-android34-clang++ -std=c++17 -O2 -static -Wall -Wextra -Werror -DA6L_QMI_NO_LIBLOG -I$B/radio/qmi/include \
    $B/radio/qmi/src/*.cc $B/radio/tools/a6l_qmi_cli.cc -o $B/a6l-qmi || fail ndk
$NB/llvm-strip $B/a6l-qmi; file $B/a6l-qmi
strings $B/a6l-qmi | grep -c "dataformat" 
echo "=== 3. bundle $(date)"
BU=$B/bundle/ipa3; mkdir -p $BU/modules $BU/extra $BU/dt
cp $OLD/modules/order.txt $BU/modules/
for k in $(cat $OLD/modules/order.txt); do [ "$k" = ipa2_lite.ko ] || cp $OLD/modules/$k $BU/modules/; done
cp $m $BU/modules/; cp $OLD/extra/a6l_ipa2_ovl.ko $BU/extra/; cp $OLD/dt/* $BU/dt/
cp $S/bundle/run.sh $S/bundle/data-test.sh $BU/; cp $B/a6l-qmi $BU/; cp $RS/tools/ril-test.sh $BU/
sh -n $BU/run.sh && sh -n $BU/data-test.sh && echo scripts-syntax-ok
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )
rm -rf $OUTR/ipa3; mkdir -p $OUTR; cp -r $BU $OUTR/ipa3; mkdir -p $OUTR/build; cp $B/ipa2-lite-build.log $OUTR/build/
git -C $R show HEAD:device/hisense/a6l/kernel/ipa/ipa2-lite/src/ipa.c > $B/ipa.c.head 2>/dev/null && diff -u $B/ipa.c.head $S/ipa2-lite/src/ipa.c > $S/ipa2-lite/a6l-ipa2-lite-data2.diff; echo "ipa.c diff lines: $(wc -l < $S/ipa2-lite/a6l-ipa2-lite-data2.diff)"
cat $BU/SHA256SUMS
echo "=== 4. laptop $(date)"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
if timeout 20 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75/logs' < /dev/null; then
  timeout 90 $SCP -r 'C:\Users\Pierre\Desktop\A6L\firmware\extracted\ipa-20260925\ipa3' a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
  timeout 20 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/ipa3 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_STAGED_OK' < /dev/null
else echo "laptop unreachable (not staged)"; fi
echo DATA2_BUNDLE_DONE $(date)
