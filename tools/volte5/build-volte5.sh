#!/bin/bash
# volte5 (28 Sep 2026): build + check + stage bundle v75/volte5 (offline). WSL, run with nohup (~6 min):
#  a6l-imsdcm / a6l-qmi NDK r27c static; HAL+lib clang syntax vs the tree's AIDL V4 NDK headers; host tests (gcc,
#  ASan/UBSan); pdc/ = volte4 bundle (qmicli + pinned Orange MBN) with the fixed volte4-test.sh; mocks (recovery mksh
#  under qemu); bundle firmware/extracted/volte5-20260928 + laptop ~/A6L-usb-20260915/v75/volte5 (new folder).
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
W=/home/a6l/vo28-work; rm -rf $W; mkdir -p $W/out
B=$R/firmware/extracted/volte5-20260928
[ -e $B/SHA256SUMS ] && { echo "REFUSE: $B already staged (new folder only)"; exit 1; }
mkdir -p $B/test-logs
st() { echo "=== $(date +%T) $*"; }
st "radio copy"; cp -r $R/device/hisense/a6l/radio $W/radio
st "NDK static"
for t in a6l_imsdcm:a6l-imsdcm a6l_qmi_cli:a6l-qmi; do src=${t%%:*}; bin=${t##*:}
  $NDK/aarch64-linux-android34-clang++ -std=c++17 -O2 -static -Wall -Wextra -Werror -DA6L_QMI_NO_LIBLOG -I$W/radio/qmi/include \
    $W/radio/qmi/src/*.cc $W/radio/tools/$src.cc -o $W/out/$bin && $NDK/llvm-strip $W/out/$bin || { echo "NDK_FAIL $bin"; exit 1; }
  file $W/out/$bin
done
strings $W/out/a6l-imsdcm | grep -c -E "A6L_IMSDCM_IMSS|--status" | sed 's/^/imsdcm volte5 strings: /'
st "HAL/lib syntax vs Lineage AIDL V4 NDK headers"
T=/home/a6l/android/a6l-lineage24; S=$W/radio
CC=$T/prebuilts/clang/host/linux-x86/clang-r596125/bin/clang++
G=out/soong/.intermediates/hardware/interfaces/radio/aidl
F="-nostdlibinc -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -D__ANDROID_VENDOR_API__=202604 -Werror=implicit-function-declaration -D__BIONIC_NO_PAGE_SIZE_MACRO -O2 -Wall -Wextra -target aarch64-linux-android37 -DANDROID_STRICT -fPIE -Wimplicit-fallthrough -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS -Wno-gnu-include-next -fno-short-enums -Werror=non-virtual-dtor -Werror=address -I$S/hal -I$S -I$S/qmi/include -I$S/minradio/include -Iframeworks/native/libs/binder/ndk/include_cpp -Iframeworks/native/libs/binder/ndk/include_ndk -Iframeworks/native/libs/binder/ndk/include_platform"
for x in radio radio.config radio.data radio.messaging radio.modem radio.network radio.sim radio.voice; do F="$F -I$G/android.hardware.$x-V4-ndk-source/gen/include"; done
F="$F -Isystem/libbase/include -Iexternal/fmtlib/include -Isystem/core/libcutils/include_outside_system -Isystem/core/libprocessgroup/include -Isystem/core/libcutils/include -Isystem/logging/liblog/include_vndk -Isystem/core/libutils/include -Isystem/core/libsystem/include -Isystem/core/libutils/binder/include -Iprebuilts/clang/host/linux-x86/clang-r596125/android_libc++/platform/aarch64/include/c++/v1 -Iprebuilts/clang/host/linux-x86/clang-r596125/include/c++/v1 -isystem bionic/libc/include -isystem bionic/libc/kernel/uapi/asm-arm64 -isystem bionic/libc/kernel/uapi -isystem bionic/libc/kernel/android/scsi -isystem bionic/libc/kernel/android/uapi -Wall -Wextra -Werror -Wno-unused-parameter -D_LIBCPP_DISABLE_DEPRECATION_WARNINGS -DANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION -std=gnu++20 -fno-rtti -Wno-deprecated-declarations"
( cd $T; for f in $S/hal/*.cpp $S/qmi/src/*.cc $S/tools/a6l_qmi_cli.cc $S/tools/a6l_imsdcm.cc; do
  if $CC $F -fsyntax-only $f 2> $W/out/syn.err; then echo "OK $(basename $f)"; else echo "FAIL $(basename $f)"; head -40 $W/out/syn.err; fi
done ) > $W/out/syntax.txt 2>&1
grep -q FAIL $W/out/syntax.txt && echo "SYNTAX rc=1" | tee -a $W/out/syntax.txt || echo "SYNTAX rc=0" | tee -a $W/out/syntax.txt
st "host tests gcc"
( cd $W/radio && CXX=g++ OUT=$W/out/t-gcc sh tests/run-host-tests.sh ) > $W/out/host-tests.txt 2>&1; echo "gcc rc=$?" >> $W/out/host-tests.txt
grep -E 'tests|PASS|rc=|FAIL' $W/out/host-tests.txt
st "pdc/ (volte4 bundle + fixed volte4-test.sh)"
P=$W/bundle/pdc; mkdir -p $P
( cd $R/firmware/extracted/volte4-20260927 && cp -r mbn q qmicli $P/ ); cp $R/tools/volte4-pdc/volte4-test.sh $P/
tr -d '\r' < $P/volte4-test.sh > $P/v && mv $P/v $P/volte4-test.sh
( cd $P && sha256sum mbn/France-Commercial-Orange.mbn mbn/MBN-INFO.txt q/SOURCES.txt q/ld-musl-aarch64.so.1 q/lib/* q/qmicli.bin q/libgmffix.so qmicli volte4-test.sh > SHA256SUMS )
grep -q 336ac0f81cf6daa6e1424331a8d2f8d4e0030daeb24b89d3b15de54e6e003c4c $P/SHA256SUMS && echo "MBN pinned hash ok"
st "mocks (recovery mksh + toybox under qemu)"
rm -rf ~/vo5/mock-out; bash $R/tools/volte4-pdc/mock-test.sh $P ~/vo5/rd > $W/out/mock-volte4.txt 2>&1; echo "volte4 mock rc=$?"; grep -v linker $W/out/mock-volte4.txt | grep -E "MOCK_(FAIL|SUMMARY)"
tr -d '\r' < $W/radio/tools/volte5-test.sh > $W/out/volte5-test.sh
bash $R/tools/volte5/mock-test-volte5.sh $W/out/volte5-test.sh ~/vo5/rd > $W/out/mock-volte5.txt 2>&1; echo "volte5 mock rc=$?"; grep -v linker $W/out/mock-volte5.txt | grep -E "MOCK_(FAIL|SUMMARY)"
st "bundle"
cp $W/out/a6l-imsdcm $W/out/a6l-qmi $W/out/volte5-test.sh $R/firmware/extracted/volte-20260925/volte-probe $W/bundle/
chmod 755 $W/bundle/a6l-imsdcm $W/bundle/a6l-qmi $W/bundle/volte-probe $W/bundle/volte5-test.sh
( cd $W/bundle && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS && wc -l SHA256SUMS && grep -E ' (a6l-imsdcm|a6l-qmi|volte-probe|volte5-test.sh|pdc/volte4-test.sh)$' SHA256SUMS )
cp -r $W/bundle/. $B/
grep -v linker $W/out/mock-volte4.txt > $B/test-logs/mock-volte4.txt; grep -v linker $W/out/mock-volte5.txt > $B/test-logs/mock-volte5.txt
cp $W/out/host-tests.txt $W/out/syntax.txt $B/test-logs/
( cd $B && sha256sum -c SHA256SUMS | grep -vc ': OK$' | sed 's/^/repo non-OK lines: /' )
st "laptop v75/volte5 (new folder)"
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
timeout 30 $SSH a6l-laptop '[ -e A6L-usb-20260915/v75/volte5 ] && echo EXISTS || mkdir -p A6L-usb-20260915/v75/volte5' < /dev/null | grep -q EXISTS && { echo "REFUSE: laptop v75/volte5 exists"; exit 1; }
cd $W/bundle && timeout 300 $SCP -r $(ls) a6l-laptop:A6L-usb-20260915/v75/volte5/ < /dev/null 2>&1 | tail -2
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/volte5 && chmod 755 a6l-imsdcm a6l-qmi volte-probe volte5-test.sh pdc/qmicli pdc/volte4-test.sh pdc/q/ld-musl-aarch64.so.1 pdc/q/qmicli.bin && sha256sum -c SHA256SUMS > /tmp/v5sum.txt && (cd pdc && sha256sum -c SHA256SUMS > /tmp/v5pdc.txt) && echo LAPTOP_STAGED_OK; grep -vc ": OK$" /tmp/v5sum.txt; find . -type f | wc -l' < /dev/null
echo "== DONE $(date)"
