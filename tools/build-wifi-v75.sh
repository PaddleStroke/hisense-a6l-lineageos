#!/bin/bash
# wifi agent (26 Sep 2026): OFFLINE build of the v75/wifi bundle (Wi-Fi association + BT discovery tests for the test recovery).
#  - wpa_supplicant/wpa_cli 2.11 (upstream w1.fi) + iw 6.17: STATIC aarch64 musl, NDK r27c clang + lld against an Alpine
#    latest-stable aarch64 sysroot (musl, openssl 3.5 static, libnl3 static, gcc crt/libgcc).
#  - busybox (Alpine busybox-static: udhcpc, ip, route, ping, nslookup), a6l-net (data3, ipa4), regulatory.db(+p7s)
#  - a6l-bt-scan (NDK static bionic, device/hisense/a6l/wifi/tools/a6l_bt_scan.c)
# Output: repo firmware/extracted/wifi-20260926/wifi (+ laptop ~/A6L-usb-20260915/v75/wifi). Run with nohup.
exec < /dev/null
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
W=/home/a6l/wifi-build; A=$W/apk; SR=$W/sysroot; B=$W/build
NB=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
S=$R/device/hisense/a6l/wifi
fail() { echo "WIFI_BUNDLE_FAIL $*"; exit 1; }
mkdir -p $A $B; cd $A
echo "=== 1. alpine sysroot $(date)"
pkgs="musl musl-dev linux-headers openssl-dev openssl-libs-static libnl3-dev libnl3-static gcc libgcc-static busybox-static wireless-regdb"
rm -rf $SR; mkdir -p $SR
for p in $pkgs; do
  f=$(awk -v P=$p 'BEGIN{RS="";FS="\n"}{n="";v="";for(i=1;i<=NF;i++){if($i~/^P:/)n=substr($i,3);if($i~/^V:/)v=substr($i,3)} if(n==P)print n"-"v".apk"}' idx-main/APKINDEX)
  [ -n "$f" ] || fail "no apk $p"
  [ -s $f ] || timeout 120 curl -sSf -o $f https://dl-cdn.alpinelinux.org/alpine/latest-stable/main/aarch64/$f || fail "download $f"
  tar xzf $f -C $SR 2>/dev/null; echo "  $f $(sha256sum $f | cut -c1-16)"
done
GV=$(ls $SR/usr/lib/gcc/aarch64-alpine-linux-musl/ 2>/dev/null | head -1); echo "gcc dir $GV"; ls $SR/usr/lib/gcc/aarch64-alpine-linux-musl/$GV | grep -E 'crt|libgcc' | tr '\n' ' '; echo
CC="$NB/clang --target=aarch64-alpine-linux-musl --sysroot=$SR -fuse-ld=lld"
printf '#include <stdio.h>\nint main(void){puts("musl-static-ok");return 0;}\n' > $B/t.c
$CC -O2 -static $B/t.c -o $B/t 2>&1 | head -20; file $B/t; qemu-aarch64 $B/t || fail "musl toolchain"
echo "=== 2. wpa_supplicant 2.11 $(date)"
cd $W; [ -s wpa_supplicant-2.11.tar.gz ] || timeout 120 curl -sSf -o wpa_supplicant-2.11.tar.gz https://w1.fi/releases/wpa_supplicant-2.11.tar.gz || fail "download hostap"
sha256sum wpa_supplicant-2.11.tar.gz
rm -rf wpa_supplicant-2.11; tar xzf wpa_supplicant-2.11.tar.gz
cd wpa_supplicant-2.11/wpa_supplicant; cp $S/wpa_supplicant-a6l.config .config
CFLAGS="-O2 -MMD -Wno-unused-command-line-argument" LDFLAGS="-static" make -j8 CC="$CC" LIBNL_INC=$SR/usr/include/libnl3 \
     EXTRALIBS="-lnl-genl-3 -lnl-3 -lssl -lcrypto -lpthread" wpa_supplicant wpa_cli > $B/wpa-build.log 2>&1 || { tail -40 $B/wpa-build.log; fail wpa; }
echo "wpa warnings: $(grep -c 'warning:' $B/wpa-build.log)"
$NB/llvm-strip wpa_supplicant wpa_cli; cp wpa_supplicant wpa_cli $B/
file $B/wpa_supplicant $B/wpa_cli; ls -la $B/wpa_supplicant $B/wpa_cli
qemu-aarch64 $B/wpa_supplicant -v | head -2; qemu-aarch64 $B/wpa_cli -v | head -1
echo "=== 3. iw 6.17 $(date)"
cd $W; [ -s iw-6.17.tar.xz ] || timeout 120 curl -sSf -o iw-6.17.tar.xz https://www.kernel.org/pub/software/network/iw/iw-6.17.tar.xz || echo "iw download failed (bundle keeps radio2 iw)"
if [ -s iw-6.17.tar.xz ]; then sha256sum iw-6.17.tar.xz; rm -rf iw-6.17; tar xJf iw-6.17.tar.xz; cd iw-6.17
  CFLAGS="-O2 -I$SR/usr/include/libnl3 -DCONFIG_LIBNL30 -Wno-unused-command-line-argument" LDFLAGS="-static" make -j8 CC="$CC" LIBS="-lnl-genl-3 -lnl-3" NLLIBNAME=libnl-3.0 PKG_CONFIG=true > $B/iw-build.log 2>&1 \
    && $NB/llvm-strip iw && cp iw $B/iw && qemu-aarch64 $B/iw --version || { tail -20 $B/iw-build.log; echo "iw build failed (bundle keeps radio2 iw)"; }
fi
echo "=== 4. a6l-bt-scan + a6l-net $(date)"
$NB/aarch64-linux-android34-clang -O2 -static -Wall -Wextra -Werror $S/tools/a6l_bt_scan.c -o $B/a6l-bt-scan || fail bt-ndk
$NB/llvm-strip $B/a6l-bt-scan; file $B/a6l-bt-scan
gcc -O2 -Wall -Wextra -Werror -DA6L_BT_SELFTEST $S/tools/a6l_bt_scan.c -o $B/bt-self && $B/bt-self || fail bt-selftest
qemu-aarch64 $B/a6l-bt-scan -t 1; echo "(qemu: socket family error expected) rc=$?"
echo "=== 5. bundle $(date)"
BU=$B/bundle/wifi; rm -rf $B/bundle; mkdir -p $BU/bin $BU/firmware
cp $B/wpa_supplicant $B/wpa_cli $B/a6l-bt-scan $BU/bin/; [ -f $B/iw ] && cp $B/iw $BU/bin/iw
cp $SR/bin/busybox.static $BU/bin/busybox; cp $R/firmware/extracted/ipa-20260925b/ipa4/a6l-net $BU/bin/
cp $SR/lib/firmware/regulatory.db $SR/lib/firmware/regulatory.db.p7s $BU/firmware/ 2>/dev/null || cp $SR/usr/lib/firmware/regulatory.db* $BU/firmware/
qemu-aarch64 $BU/bin/busybox --list | grep -x -E 'udhcpc|ip|route|ping|nslookup|wget|ifconfig|od' | tr '\n' ' '; echo
cp $S/bundle/wifi-test.sh $S/bundle/bt-scan.sh $S/bundle/udhcpc.script $BU/
sh -n $BU/wifi-test.sh && sh -n $BU/bt-scan.sh && sh -n $BU/udhcpc.script && echo scripts-syntax-ok
( cd $BU && find . -type f ! -name SHA256SUMS | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo bundle-hash-ok )
OUT=$R/firmware/extracted/wifi-20260926; rm -rf $OUT/wifi; mkdir -p $OUT/build; cp -r $BU $OUT/wifi; cp $B/*.log $OUT/build/ 2>/dev/null
cat $BU/SHA256SUMS; du -sh $BU
echo "=== 6. laptop $(date)"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
if [ "${NOSTAGE:-0}" != 1 ] && timeout 20 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75/logs && rm -rf A6L-usb-20260915/v75/wifi' < /dev/null; then
  timeout 90 $SCP -r 'C:\Users\Pierre\Desktop\A6L\firmware\extracted\wifi-20260926\wifi' a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
  timeout 20 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/wifi && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_STAGED_OK' < /dev/null
else echo "laptop not staged"; fi
echo WIFI_BUNDLE_DONE $(date)
