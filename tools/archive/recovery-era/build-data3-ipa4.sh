#!/bin/bash
# data3 agent (25 Sep 2026): OFFLINE. ipa2_lite.ko (QMI filter-rule replies, route index 7, A6L_IPA_ST status decode)
# + static a6l-net (NDK) -> bundle v75/ipa4 = repo firmware/extracted/ipa-20260925b/ipa4 (+ laptop ~/A6L-usb-20260915/v75/ipa4).
# Other files (a6l-qmi, ril-test.sh, overlay, dt, rmnet/qrtr .ko) are taken unchanged from ipa3. Run with nohup.
exec < /dev/null
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
NDK=/home/a6l/ndk/android-ndk-r27c; NB=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
S=$R/device/hisense/a6l/kernel/ipa
B=/home/a6l/kernel/data3-build; rm -rf $B; mkdir -p $B/ipa2-lite
OUTR=$R/firmware/extracted/ipa-20260925b; OLD=$R/firmware/extracted/ipa-20260925/ipa3
fail() { echo "DATA3_BUNDLE_FAIL $*"; exit 1; }
echo "=== 1. ipa2_lite.ko $(date)"
cp $S/ipa2-lite/src/* $B/ipa2-lite/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$B/ipa2-lite W=1 modules -j8 > $B/ipa2-lite-build.log 2>&1 || { tail -40 $B/ipa2-lite-build.log; fail ipa2-lite; }
echo "ipa2-lite warnings: $(grep -c 'warning:' $B/ipa2-lite-build.log)"; grep -A3 'warning:' $B/ipa2-lite-build.log | head -30
m=$B/ipa2-lite/ipa2_lite.ko
echo "vermagic=$(modinfo -F vermagic $m) depends=$(modinfo -F depends $m)"
for s in $($CL/llvm-nm -u $m | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || echo "  UNRESOLVED $s"; done
modinfo -F parm $m | grep -E "status_log|rt_apps_entry|diag_pipes" || fail "params missing"
strings $m | grep -E "A6L_IPA qmi|A6L_IPA_ST op" | head -4
echo "=== 2. a6l-net (static NDK) $(date)"
$NB/aarch64-linux-android34-clang -O2 -static -Wall -Wextra -Werror $S/tools/a6l_net.c -o $B/a6l-net || fail ndk
$NB/llvm-strip $B/a6l-net; file $B/a6l-net; ls -la $B/a6l-net
gcc -O2 -Wall -Wextra -Werror $S/tools/a6l_net.c -o $B/a6l-net-host && $B/a6l-net-host stats lo
echo "=== 3. bundle $(date)"
BU=$B/bundle/ipa4; mkdir -p $BU/modules $BU/extra $BU/dt
cp $OLD/modules/* $BU/modules/; cp $m $BU/modules/ipa2_lite.ko
cp $OLD/extra/* $BU/extra/; cp $OLD/dt/* $BU/dt/
cp $S/bundle/run.sh $S/bundle/data-test.sh $BU/; cp $OLD/a6l-qmi $OLD/ril-test.sh $BU/; cp $B/a6l-net $BU/
sh -n $BU/run.sh && sh -n $BU/data-test.sh && echo scripts-syntax-ok
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )
rm -rf $OUTR/ipa4; mkdir -p $OUTR/build; cp -r $BU $OUTR/ipa4; cp $B/ipa2-lite-build.log $OUTR/build/
: diffs are made by the agent: a6l-ipa2-lite-data3-ipa.diff and -qmi.diff


cat $BU/SHA256SUMS
echo "=== 4. laptop $(date)"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
if timeout 20 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75/logs' < /dev/null; then
  timeout 90 $SCP -r 'C:\Users\Pierre\Desktop\A6L\firmware\extracted\ipa-20260925b\ipa4' a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
  timeout 20 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/ipa4 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_STAGED_OK' < /dev/null
else echo "laptop unreachable (not staged)"; fi
echo DATA3_BUNDLE_DONE $(date)
