#!/usr/bin/env bash
# audio3 agent, 24 Sep 2026 (night). OFFLINE build, nothing touches the phone. Run from WSL (relay nohup):
#  - q6asm-dai.ko with a6l-q6asm-dai-xlate-v75.patch (own M= dir; kernel tree untouched)       -> fixes the swapped MultiMedia FEs
#  - snd-soc-tfa98xx.ko = tfa98xx v6.7.14 + apply-a6l-patches.sh + apply-a6l-optional-v75.sh (stub DAI on amp failure)
#  - a6l-audio-route (static NDK test build of device/hisense/a6l/audio/route/a6l_audio_route.c + libaudioroute + tinyalsa + expat)
#  - V75 audio DT overlays compiled + merged + link-order/FE-mapping check (tools/check-audio-dt-links.py)
#  - bundle v75/audio3 (V71/V74 recovery header + audio3-run.body.sh) -> repo firmware/extracted/audio3-20260924, laptop v75/audio3
set -uo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/audio3; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
L=/home/a6l/android/a6l-lineage24; SRC=$W/device/hisense/a6l; A=$W/firmware/extracted/audio3-20260924; A2=$W/firmware/extracted/audio2-20260923
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin; CC=$NDK/aarch64-linux-android34-clang
export PATH=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
mkdir -p $B; rm -rf $B/out $B/bundle; mkdir -p $B/out/modules $B/out/bin $B/out/dt
# ---- 1. q6asm-dai
M=$B/q6asm; rm -rf $M; mkdir -p $M/a/sound/soc/qcom/qdsp6; cp $K/sound/soc/qcom/qdsp6/*.h $M/
cp $K/sound/soc/qcom/qdsp6/q6asm-dai.c $M/a/sound/soc/qcom/qdsp6/
( cd $M/a && patch -p1 < $SRC/kernel/a6l-q6asm-dai-xlate-v75.patch ) || { echo Q6ASM_PATCH_FAIL; exit 1; }
cp $M/a/sound/soc/qcom/qdsp6/q6asm-dai.c $M/; rm -rf $M/a; printf 'obj-m += q6asm-dai.o\n' > $M/Makefile
( cd $K && patch -p1 --dry-run -s < $SRC/kernel/a6l-q6asm-dai-xlate-v75.patch && echo Q6ASM_PATCH_APPLIES_TO_TREE )
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$M modules > $M/build.log 2>&1 || { grep -a -B2 -A6 "error" $M/build.log | head -40; exit 1; }
llvm-strip --strip-debug -o $B/out/modules/q6asm-dai.ko $M/q6asm-dai.ko
echo "Q6ASM_PASS warnings=$(grep -a -c 'warning:' $M/build.log) xlate_sym=$(strings $B/out/modules/q6asm-dai.ko | grep -c q6asm_dai_of_xlate_dai_name)"
modinfo -F vermagic $B/out/modules/q6asm-dai.ko; modinfo -F vermagic $A2/v74/audio2/modules/q6asm-dai.ko; modinfo -F depends $B/out/modules/q6asm-dai.ko
# ---- 2. tfa98xx optional
T=$B/tfa; rm -rf $T; cp -r /home/a6l/audio-v74/tfa98xx $T; rm -rf $T/.git
bash $SRC/kernel/tfa98xx-a6l/apply-a6l-patches.sh $T && bash $SRC/kernel/tfa98xx-a6l/apply-a6l-optional-v75.sh $T || { echo TFA_PATCH_FAIL; exit 1; }
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$T modules > $T/build.log 2>&1 || { grep -a "error" $T/build.log | head -30; exit 1; }
llvm-strip --strip-debug -o $B/out/modules/snd-soc-tfa98xx.ko $T/snd-soc-tfa98xx.ko
echo "TFA_PASS warnings=$(grep -a -c 'warning:' $T/build.log) stub=$(strings $B/out/modules/snd-soc-tfa98xx.ko | grep -c A6L_TFA_STUB)"
grep -a "tfa98xx.c.*warning" $T/build.log | grep -i "a6l\|stub\|probe" | head
# ---- 3. a6l-audio-route (static test build; the ROM builds it with soong from device/hisense/a6l/audio/Android.bp)
TA=$L/external/tinyalsa; AR=$L/system/media/audio_route; EX=$L/external/expat; S=$B/shim; mkdir -p $S/log
cat > $S/log/log.h <<'H'
#pragma once
#include <stdio.h>
#define ALOGE(...) (fprintf(stderr, "E " LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define ALOGW(...) (fprintf(stderr, "W " LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define ALOGI(...) (fprintf(stderr, "I " LOG_TAG ": " __VA_ARGS__), fputc('\n', stderr))
#define ALOGD(...) ((void)0)
#define ALOGV(...) ((void)0)
H
ls $AR $EX/lib 2>&1 | head -30; grep -n "cflags\|-D" $EX/Android.bp 2>/dev/null | head
EXS="$EX/lib/xmlparse.c $EX/lib/xmlrole.c $EX/lib/xmltok.c"
$CC -static -O2 -Wall -DHAVE_EXPAT_CONFIG_H -DXML_POOR_ENTROPY -I$S -I$TA/include -I$AR/include -I$AR -I$EX -I$EX/lib \
  -o $B/out/bin/a6l-audio-route $SRC/audio/route/a6l_audio_route.c $AR/audio_route.c \
  $TA/mixer.c $TA/mixer_hw.c $TA/mixer_plugin.c $TA/pcm.c $TA/pcm_hw.c $TA/pcm_plugin.c $TA/snd_utils.c $EXS -ldl > $B/route.log 2>&1 \
  && $NDK/llvm-strip $B/out/bin/a6l-audio-route && echo ROUTE_STATIC_PASS || { echo ROUTE_STATIC_FAIL; head -40 $B/route.log; }
grep -a "a6l_audio_route.c" $B/route.log | head
# -Werror check of the daemon source alone, as soong compiles it
$CC -fsyntax-only -Wall -Werror -I$S -I$TA/include -I$AR/include $SRC/audio/route/a6l_audio_route.c && echo ROUTE_WERROR_PASS
xmllint --noout $SRC/audio/mixer_paths_a6l.xml $SRC/audio/audio_policy_configuration.xml && echo XML_PASS
# ---- 4. DT overlays
DTB=$W/firmware/extracted/recovery-v74-candidate-20260923/merged-captured-abl.dtb; CK="python3 $W/tools/check-audio-dt-links.py"
for v in internal-v75 speaker-v75 speaker-onbase-v75; do dtc -@ -q -I dts -O dtb -o $B/out/dt/a6l-audio-$v.dtbo $SRC/kernel/a6l-audio-$v.dtso || echo "DTC_FAIL $v"; done
fdtoverlay -i $DTB -o $B/out/dt/v74+speaker-onbase-v75.dtb $B/out/dt/a6l-audio-speaker-onbase-v75.dtbo && $CK $B/out/dt/v74+speaker-onbase-v75.dtb --rule reg --expect-tfa | tail -1
$CK $DTB --rule index | tail -1; $CK $DTB --rule reg | tail -1
# ---- 5. bundle
V71=$W/firmware/extracted/v71-attended-bundle-20260922/audio; HDR=$B/hdr.sh; sed -n '1,11p' $V71/run.sh > $HDR
grep -q "a6l-controls" $HDR && grep -q "^load()" $HDR && grep -q "^fw()" $HDR || { echo "header extraction failed"; exit 1; }
U=$B/bundle/audio3; mkdir -p $U/extra $U/firmware $U/bin $U/wav
cp -r $A2/v74/audio2/modules $U/; cp $B/out/modules/q6asm-dai.ko $U/modules/q6asm-dai.ko
cp $A2/v74/audio2/extra/pinctrl-sdm660-lpass-lpi.ko $B/out/modules/snd-soc-tfa98xx.ko $U/extra/; cp $W/firmware/extracted/vendor/firmware/tfa98xx.cnt $U/firmware/
cp $A2/v74/audio2/bin/* $U/bin/; [ -f $B/out/bin/a6l-audio-route ] && cp $B/out/bin/a6l-audio-route $U/bin/
cp $A2/v74/audio2/wav/*.wav $U/wav/; cp -r $SRC/audio/v75-test/mixer3 $U/; cp $SRC/audio/v74-test/wav-level.py $U/
cat $HDR $SRC/audio/v75-test/audio3-run.body.sh > $U/run.sh
( cd $U && sed -i 's/\r$//' run.sh mixer3/*.txt; find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS )
bash -n $U/run.sh && echo SYNTAX_OK
rm -rf $A; mkdir -p $A/modules $A/dt $A/v75; cp -r $U $A/v75/; cp $B/out/modules/*.ko $A/modules/; cp $B/out/dt/* $A/dt/; [ -f $B/out/bin/a6l-audio-route ] && cp $B/out/bin/a6l-audio-route $A/modules/
( cd $A && find . -type f ! -name SHA256SUMS-all | sort | xargs sha256sum > SHA256SUMS-all ); grep -v "/modules/q6a[fd]\|/modules/q6core\|/modules/q6r\|/modules/snd\|/modules/qmi\|/modules/qcom\|/modules/pdr\|/modules/apr\|/modules/sound\|/modules/pinctrl\|/modules/mc\|/modules/soc-usb\|/wav/\|/bin/tiny" $A/SHA256SUMS-all
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"; SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
timeout 60 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75 && rm -rf A6L-usb-20260915/v75/audio3' < /dev/null && \
timeout 120 $SCP -r -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio3-20260924/v75/audio3 a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/audio3 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_AUDIO3_OK; du -sh .' < /dev/null || echo LAPTOP_COPY_FAIL
echo AUDIO3_V75_DONE
