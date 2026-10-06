#!/bin/bash
# volte3 (26 Sep 2026): build + check VoLTE steps 3/4 (offline). WSL, run with nohup (~6 min):
#  radio: a6l-qmi / a6l-imsdcm NDK static, HAL+lib clang syntax vs the tree's AIDL V4 NDK headers, host tests (gcc, ASan/UBSan)
#  kernel: q6mvm.ko with a6l-q6mvm-volte-session-v75.patch (M= against out-a6l-phone-v67, W=1, symbols, modversions)
#  a6l-q6voiced (-s) NDK static + host build; rom-v2 series composition check; bundle v75/volte3 + laptop staging.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67; WV=/home/a6l/kernel/a6l-7.2-voice
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
W=/home/a6l/volte3-work; rm -rf $W; mkdir -p $W/out
B=$R/firmware/extracted/volte3-20260926; mkdir -p $B
st() { echo "=== $(date +%T) $*"; }
st "radio copy"; cp -r $R/device/hisense/a6l/radio $W/radio
st "NDK static"
for t in a6l_imsdcm:a6l-imsdcm a6l_qmi_cli:a6l-qmi; do src=${t%%:*}; bin=${t##*:}
  $NDK/aarch64-linux-android34-clang++ -std=c++17 -O2 -static -Wall -Wextra -Werror -DA6L_QMI_NO_LIBLOG -I$W/radio/qmi/include \
    $W/radio/qmi/src/*.cc $W/radio/tools/$src.cc -o $W/out/$bin && $NDK/llvm-strip $W/out/$bin || { echo "NDK_FAIL $bin"; exit 1; }
  file $W/out/$bin
done
st "HAL/lib syntax vs Lineage AIDL V4 NDK headers"
T=/home/a6l/android/a6l-lineage24; S=$W/radio
CC=$T/prebuilts/clang/host/linux-x86/clang-r596125/bin/clang++
G=out/soong/.intermediates/hardware/interfaces/radio/aidl
F="-nostdlibinc -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -D__ANDROID_VENDOR_API__=202604 -Werror=implicit-function-declaration -D__BIONIC_NO_PAGE_SIZE_MACRO -O2 -Wall -Wextra -target aarch64-linux-android37 -DANDROID_STRICT -fPIE -Wimplicit-fallthrough -D_LIBCPP_ENABLE_THREAD_SAFETY_ANNOTATIONS -Wno-gnu-include-next -fno-short-enums -Werror=non-virtual-dtor -Werror=address -I$S/hal -I$S -I$S/qmi/include -I$S/minradio/include -Iframeworks/native/libs/binder/ndk/include_cpp -Iframeworks/native/libs/binder/ndk/include_ndk -Iframeworks/native/libs/binder/ndk/include_platform"
for x in radio radio.config radio.data radio.messaging radio.modem radio.network radio.sim radio.voice; do F="$F -I$G/android.hardware.$x-V4-ndk-source/gen/include"; done
F="$F -Isystem/libbase/include -Iexternal/fmtlib/include -Isystem/core/libcutils/include_outside_system -Isystem/core/libprocessgroup/include -Isystem/core/libcutils/include -Isystem/logging/liblog/include_vndk -Isystem/core/libutils/include -Isystem/core/libsystem/include -Isystem/core/libutils/binder/include -Iprebuilts/clang/host/linux-x86/clang-r596125/android_libc++/platform/aarch64/include/c++/v1 -Iprebuilts/clang/host/linux-x86/clang-r596125/include/c++/v1 -isystem bionic/libc/include -isystem bionic/libc/kernel/uapi/asm-arm64 -isystem bionic/libc/kernel/uapi -isystem bionic/libc/kernel/android/scsi -isystem bionic/libc/kernel/android/uapi -Wall -Wextra -Werror -Wno-unused-parameter -D_LIBCPP_DISABLE_DEPRECATION_WARNINGS -DANDROID_UTILS_REF_BASE_DISABLE_IMPLICIT_CONSTRUCTION -std=gnu++20 -fno-rtti -Wno-deprecated-declarations"
( cd $T; for f in $S/hal/*.cpp $S/qmi/src/*.cc $S/tools/a6l_qmi_cli.cc $S/tools/a6l_imsdcm.cc; do
  if $CC $F -fsyntax-only $f 2> $W/out/syn.err; then echo "OK $(basename $f)"; else echo "FAIL $(basename $f)"; head -40 $W/out/syn.err; fi
done ) | tee $W/out/syntax.txt
grep -q FAIL $W/out/syntax.txt && echo "SYNTAX rc=1" | tee -a $W/out/syntax.txt || echo "SYNTAX rc=0" | tee -a $W/out/syntax.txt
st "host tests gcc"
( cd $W/radio && CXX=g++ OUT=$W/out/t-gcc sh tests/run-host-tests.sh ) > $W/out/host-tests.txt 2>&1; echo "gcc rc=$?" >> $W/out/host-tests.txt
grep -E 'tests:|rc=|FAIL' $W/out/host-tests.txt
st "kernel q6mvm (volte3 patch) against out-v67"
export PATH=$CL:$PATH
KB=/home/a6l/kernel/volte3-build; rm -rf $KB; mkdir -p $KB
Q=$KB/qdsp6; cp -r $WV/sound/soc/qcom/qdsp6 $Q; cp $WV/sound/soc/qcom/common.h $KB/common.h; mkdir -p $Q/include/dt-bindings/sound
cp $WV/include/dt-bindings/sound/qcom,q6voice.h $Q/include/dt-bindings/sound/
rm -f $Q/*.o $Q/*.ko $Q/.*.cmd $Q/*.mod*
tr -d '\r' < $R/device/hisense/a6l/kernel/kvoice/a6l-q6mvm-volte-session-v75.patch > $KB/p.patch
( cd $Q && patch --dry-run -p5 < $KB/p.patch && patch -p5 < $KB/p.patch ) || { echo "KPATCH_FAIL"; exit 1; }
echo 'ccflags-y += -I$(src)/include' >> $Q/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 M=$Q CONFIG_SND_SOC_QDSP6_Q6VOICE=m CONFIG_SND_SOC_QDSP6_Q6VOICE_DAI=m W=1 modules -j8 > $KB/build.log 2>&1; rc=$?
echo "kbuild rc=$rc warnings(q6mvm)=$(grep -c 'q6mvm.*warning' $KB/build.log)"; grep -E 'q6mvm.*(warning|error)' $KB/build.log | head
[ $rc = 0 ] || { tail -30 $KB/build.log; exit 1; }
echo "q6mvm: parm=$(modinfo -F parm $Q/q6mvm.ko | tr '\n' ' ') vermagic=$(modinfo -F vermagic $Q/q6mvm.ko)"
for s in $($CL/llvm-nm -u $Q/q6mvm.ko | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || grep -qP "^0x[0-9a-f]+\t$s\t" $Q/Module.symvers || echo "  UNRESOLVED q6mvm: $s"; done
grep -q '^CONFIG_MODVERSIONS=y' $O/.config && echo "MODVERSIONS=y" || echo "MODVERSIONS off (no CRC coupling with the audio6 q6voice-common.ko)"
A6=$R/firmware/extracted/audio6-20260925/v75/audio6/modules
for m in q6voice-common q6cvs q6cvp q6voice q6voice-dai; do
  echo "$m: rebuilt $(sha256sum < $Q/$m.ko | cut -c1-16) audio6 $(sha256sum < $A6/$m.ko | cut -c1-16)"; done
echo "q6mvm old(audio6) $(sha256sum < $A6/q6mvm.ko | cut -c1-16) new $(sha256sum < $Q/q6mvm.ko | cut -c1-16)"
$CL/llvm-strings $Q/q6mvm.ko | grep -E 'mmode1_session|default volte voice' | head -4
st "a6l-q6voiced"
QV=$KB/q6voiced; mkdir -p $QV; cp $R/device/hisense/a6l/kvoice/q6voiced/a6l_q6voiced.c $QV/
$NDK/aarch64-linux-android34-clang -static -O2 -Wall -Wextra -Werror -o $QV/a6l-q6voiced $QV/a6l_q6voiced.c && $NDK/llvm-strip $QV/a6l-q6voiced || { echo NDK_FAIL q6voiced; exit 1; }
gcc -Wall -Wextra -Werror -O2 -o $QV/host $QV/a6l_q6voiced.c && echo q6voiced-host-ok
# host test of -s: the parameter file is redirected with a fake sysfs path via LD_PRELOAD-free trick: run as a normal user
# -> it must fail cleanly (no /sys/module/q6mvm) and print the warning, then refuse to find a pcm (exit 3)
$QV/host -s volte hold 1; echo "q6voiced host rc=$? (3 = no VoiceMMode1 pcm, expected on WSL)"
st "rom-v2 series composition (incl. the volte3 q6mvm patch)"
bash $R/tools/check-rom-v2-kernel-series.sh 2>&1 | tail -14
st "bundle"
cp $W/out/a6l-imsdcm $W/out/a6l-qmi $Q/q6mvm.ko $QV/a6l-q6voiced $W/radio/tools/volte3-test.sh $R/firmware/extracted/volte-20260925/volte-probe $B/
grep -E 'tests:|rc=|FAIL' $W/out/host-tests.txt > $B/host-tests.txt; cp $W/out/syntax.txt $B/syntax.txt
(cd $B && sha256sum a6l-imsdcm a6l-qmi volte-probe q6mvm.ko a6l-q6voiced volte3-test.sh > SHA256SUMS && cat SHA256SUMS)
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
timeout 30 $SSH -o BatchMode=yes a6l-laptop 'mkdir -p A6L-usb-20260915/v75/volte3' < /dev/null
for f in a6l-imsdcm a6l-qmi volte-probe q6mvm.ko a6l-q6voiced volte3-test.sh SHA256SUMS; do timeout 120 $SCP "C:/Users/Pierre/Desktop/A6L/firmware/extracted/volte3-20260926/$f" a6l-laptop:A6L-usb-20260915/v75/volte3/$f < /dev/null || echo "SCP_FAIL $f"; done
timeout 60 $SSH -o BatchMode=yes a6l-laptop 'cd A6L-usb-20260915/v75/volte3 && sha256sum -c SHA256SUMS && ls -la && ls ../audio6/run.sh' < /dev/null && echo LAPTOP_STAGED_OK
echo "== DONE $(date)"
