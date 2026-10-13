#!/usr/bin/env bash
# audio2 agent, 23 Sep 2026 evening. OFFLINE build (nothing touches the phone) of:
#  - v74/audio2 : PCM-open diagnosis (pcmprobe, dynamic debug, DAPM dump) + corrected tests (real "Digital " names, RAW volumes)
#  - v74/als2   : tmd3702.ko v2 (stock-equivalent proximity set-up, live register variants)
#  - extra      : pinctrl-sdm660-lpass-lpi.ko with ter_mi2s functions (for the V74 speaker DT), not loaded by any bundle
# Reuses the static tinyalsa tools + V71 audio modules of firmware/extracted/audio-20260923 (built by build-audio-sensors-v74.sh).
set -uo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/audio2-v74; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
A1=$W/firmware/extracted/audio-20260923; A=$W/firmware/extracted/audio2-20260923; SRC=$W/device/hisense/a6l
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin; CC=$NDK/aarch64-linux-android34-clang
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
rm -rf $B; mkdir -p $B/out/bin $B/out/wav $B/out/modules $A
# ---- pcmprobe (static; kernel uapi <sound/asound.h> from the NDK sysroot)
$CC -static -O2 -Wall -o $B/out/bin/pcmprobe $SRC/audio/v74-test/pcmprobe.c > $B/pcmprobe.log 2>&1 || { cat $B/pcmprobe.log; exit 1; }
$NDK/llvm-strip $B/out/bin/pcmprobe; cat $B/pcmprobe.log; echo PCMPROBE_PASS
cp $A1/v74/audio/bin/tinyplay $A1/v74/audio/bin/tinymix $A1/v74/audio/bin/tinycap $A1/v74/audio/bin/tinypcminfo $B/out/bin/
cp $A1/v74/audio/wav/*.wav $B/out/wav/
python3 - $B/out/wav <<'PY'
import math,struct,sys,wave
d=sys.argv[1];R=48000
def mk(name,secs,lg,rg,dbfs,f=1000.0):
    a=32767*10**(dbfs/20);n=int(R*secs);fade=int(R*0.05)
    w=wave.open(f'{d}/{name}','wb');w.setnchannels(2);w.setsampwidth(2);w.setframerate(R);fr=bytearray()
    for i in range(n):
        env=min(1.0,i/fade,(n-1-i)/fade);s=a*env*math.sin(2*math.pi*f*i/R);fr+=struct.pack('<hh',int(round(s*lg)),int(round(s*rg)))
    w.writeframes(bytes(fr));w.close()
mk('sine1k-m40dBFS-stereo-2s.wav',2,1,1,-40.0)
PY
echo WAV_PASS
# ---- tmd3702 v2
M=$B/tmd3702; mkdir -p $M; cp $SRC/kernel/tmd3702/tmd3702.c $M/; printf "obj-m += tmd3702.o\n" > $M/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$M modules > $M/build.log 2>&1 || { grep -a -B2 -A5 "error" $M/build.log | head -40; exit 1; }
echo "tmd3702 warnings: $(grep -a -c 'warning:' $M/build.log)"; grep -a "warning:" $M/build.log | head -5
llvm-strip --strip-debug -o $B/out/modules/tmd3702.ko $M/tmd3702.ko; echo TMD3702_PASS
# ---- LPI pinctrl with ter_mi2s functions (module replaces the in-tree one of the same name)
L=$B/lpi; mkdir -p $L; cp $SRC/kernel/lpi-pinctrl-v74/pinctrl-sdm660-lpass-lpi.c $L/; cp $SRC/kernel/lpi-pinctrl-v74/Makefile.kbuild $L/Makefile
( cd $K && patch -p1 --dry-run -s < $SRC/kernel/a6l-lpi-ter-mi2s-v74.patch && echo LPI_PATCH_APPLIES_TO_TREE )
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$L modules > $L/build.log 2>&1 || { grep -a -B2 -A5 "error" $L/build.log | head -40; exit 1; }
llvm-strip --strip-debug -o $B/out/modules/pinctrl-sdm660-lpass-lpi.ko $L/pinctrl-sdm660-lpass-lpi.ko; echo "LPI_PASS warnings=$(grep -a -c 'warning:' $L/build.log)"
strings $B/out/modules/pinctrl-sdm660-lpass-lpi.ko | grep -c "ter_mi2s"
modinfo -F vermagic $B/out/modules/*.ko; modinfo -F depends $B/out/modules/pinctrl-sdm660-lpass-lpi.ko
# ---- DT overlays: compile + apply on the V71 base, assert link order
DTB=$W/firmware/extracted/recovery-v71-candidate-20260921/base.dtb
for v in a6l-audio-speaker-onbase-v74; do dtc -@ -q -I dts -O dtb -o $B/$v.dtbo $SRC/kernel/$v.dtso && fdtoverlay -i $DTB -o $B/$v-merged.dtb $B/$v.dtbo && \
  echo "$v links: $(dtc -q -I dtb -O dts $B/$v-merged.dtb | sed -n '/^\tsound {/,/^\t};/p' | sed -n 's/.*link-name = "\(.*\)";/\1/p' | tr '\n' '|')"; done
dtc -@ -q -I dts -O dtb -o $B/a6l-audio-speaker-v74.dtbo $SRC/kernel/a6l-audio-speaker-v74.dtso && echo SPEAKER_SUPERSET_DTBO_OK
cp $B/*.dtbo $B/out/
# ---- bundles
V71=$W/firmware/extracted/v71-attended-bundle-20260922/audio; HDR=$B/hdr.sh; sed -n '1,11p' $V71/run.sh > $HDR
grep -q "a6l-controls" $HDR && grep -q "^load()" $HDR && grep -q "^klog()" $HDR || { echo "header extraction failed"; exit 1; }
mkdir -p $B/bundle/audio2/extra $B/bundle/als2/modules
cp -r $V71/modules $B/bundle/audio2/; cp -r $B/out/bin $B/out/wav $B/bundle/audio2/
mkdir $B/bundle/audio2/mixer2; cp $SRC/audio/v74-test/mixer2/*.txt $B/bundle/audio2/mixer2/
cp $B/out/modules/pinctrl-sdm660-lpass-lpi.ko $A1/modules/snd-soc-tfa98xx.ko $A1/modules/tfa98xx.cnt $B/bundle/audio2/extra/
cat $HDR $SRC/audio/v74-test/audio2-run.body.sh > $B/bundle/audio2/run.sh
cp $B/out/modules/tmd3702.ko $B/bundle/als2/modules/; cat $HDR $SRC/audio/v74-test/als2-run.body.sh > $B/bundle/als2/run.sh
for d in audio2 als2; do ( cd $B/bundle/$d && sed -i 's/\r$//' run.sh mixer2/*.txt 2>/dev/null; find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS ); done
bash -n $B/bundle/audio2/run.sh && bash -n $B/bundle/als2/run.sh && echo SYNTAX_OK
rm -rf $A/v74 $A/modules $A/dt; mkdir -p $A/v74 $A/modules $A/dt; cp -r $B/bundle/audio2 $B/bundle/als2 $A/v74/; cp $B/out/modules/*.ko $A/modules/; cp $B/out/bin/pcmprobe $A/modules/; cp $B/*.dtbo $B/*-merged.dtb $A/dt/
( cd $A && find . -type f ! -name SHA256SUMS-all | sort | xargs sha256sum > SHA256SUMS-all ); grep -v "/modules/q6\|/modules/snd\|/modules/qmi\|/modules/qcom\|/modules/pdr\|/modules/apr\|/modules/sound\|/modules/pinctrl-lpass\|/modules/mc\|/modules/soc-usb\|/wav/\|/bin/tiny" $A/SHA256SUMS-all
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"; SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
timeout 60 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v74 && rm -rf A6L-usb-20260915/v74/audio2 A6L-usb-20260915/v74/als2' < /dev/null && \
timeout 120 $SCP -r -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio2-20260923/v74/audio2 C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio2-20260923/v74/als2 a6l-laptop:A6L-usb-20260915/v74/ < /dev/null && \
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v74 && (cd audio2 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_AUDIO2_OK); (cd als2 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_ALS2_OK); du -sh audio2 als2' < /dev/null || echo LAPTOP_COPY_FAIL
echo AUDIO2_ALS2_V74_DONE
