#!/usr/bin/env bash
# audio agent, 23 Sep 2026. OFFLINE build of the v74 audio + light-sensor test bundles (nothing touches the phone):
#  - static tinyalsa tools (NDK r27c, AOSP external/tinyalsa) : tinyplay tinymix tinycap tinypcminfo
#  - 1 kHz test tones at -30 dBFS (48 kHz, 16-bit stereo; left-only / right-only / both)
#  - tmd3702.ko (new minimal IIO driver) and snd-soc-tfa98xx.ko (NXP v6.7.14 ported to 7.2; artifact only, not in a bundle)
#  - bundles v74/audio and v74/als (V71 recovery header, SHA256SUMS), copied to the repo and to the laptop.
set -uo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/audio-v74; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
A=$W/firmware/extracted/audio-20260923; SRC=$W/device/hisense/a6l
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin; CC=$NDK/aarch64-linux-android34-clang
TA=/home/a6l/android/a6l-lineage24/external/tinyalsa
export PATH=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
rm -rf $B/out; mkdir -p $B/out/bin $B/out/wav $B/out/modules $A
for t in tinyplay tinymix tinycap tinypcminfo; do
  $CC -static -O2 -Wall -I$TA/include -o $B/out/bin/$t $TA/$t.c $TA/pcm.c $TA/pcm_hw.c $TA/pcm_plugin.c $TA/mixer.c $TA/mixer_hw.c $TA/mixer_plugin.c $TA/snd_utils.c -ldl > $B/$t.log 2>&1 || { echo "FAIL $t"; exit 1; }
  $NDK/llvm-strip $B/out/bin/$t; done; echo TINYALSA_PASS
python3 - $B/out/wav <<'PY'
import math,struct,sys,wave
d=sys.argv[1];R=48000
def mk(name,secs,lg,rg,dbfs=-30.0,f=1000.0):
    a=32767*10**(dbfs/20);n=int(R*secs);fade=int(R*0.05)
    w=wave.open(f'{d}/{name}','wb');w.setnchannels(2);w.setsampwidth(2);w.setframerate(R);fr=bytearray()
    for i in range(n):
        env=min(1.0,i/fade,(n-1-i)/fade);s=a*env*math.sin(2*math.pi*f*i/R);fr+=struct.pack('<hh',int(round(s*lg)),int(round(s*rg)))
    w.writeframes(bytes(fr));w.close()
mk('sine1k-m30dBFS-stereo-3s.wav',3,1,1);mk('sine1k-m30dBFS-left-2s.wav',2,1,0);mk('sine1k-m30dBFS-right-2s.wav',2,0,1)
PY
echo WAV_PASS
M=$B/tmd3702; rm -rf $M; mkdir -p $M; cp $SRC/kernel/tmd3702/tmd3702.c $M/; printf "obj-m += tmd3702.o\n" > $M/Makefile
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$M modules > $M/build.log 2>&1 || { grep -a -A5 error $M/build.log | head -30; exit 1; }
llvm-strip --strip-debug -o $B/out/modules/tmd3702.ko $M/tmd3702.ko; grep -a -c "warning:" $M/build.log; echo TMD3702_PASS
T=$B/tfa-a6l-final; rm -rf $T; cp -r $B/tfa98xx $T; rm -rf $T/.git; git -C $B/tfa98xx rev-parse HEAD
bash $SRC/kernel/tfa98xx-a6l/apply-a6l-patches.sh $T
make -C $K O=$O ARCH=arm64 LLVM=1 -j8 M=$T modules > $T/build.log 2>&1 || { grep -a "error:" $T/build.log | head -30; exit 1; }
llvm-strip --strip-debug -o $B/out/modules/snd-soc-tfa98xx.ko $T/snd-soc-tfa98xx.ko; echo "TFA98XX_PASS warnings=$(grep -a -c 'warning:' $T/build.log)"
modinfo -F vermagic $B/out/modules/*.ko
# ---- bundles
V71=$W/firmware/extracted/v71-attended-bundle-20260922/audio; HDR=$B/hdr.sh; sed -n '1,11p' $V71/run.sh > $HDR
grep -q "a6l-controls" $HDR && grep -q "^fw()" $HDR || { echo "header extraction failed"; exit 1; }
rm -rf $B/bundle; mkdir -p $B/bundle/audio $B/bundle/als/modules
cp -r $V71/modules $B/bundle/audio/; cp -r $B/out/bin $B/out/wav $B/bundle/audio/; mkdir $B/bundle/audio/mixer; cp $SRC/audio/v74-test/mixer/*.txt $B/bundle/audio/mixer/
cp $SRC/audio/v74-test/wav-level.py $B/bundle/audio/
cat $HDR $SRC/audio/v74-test/audio-run.body.sh > $B/bundle/audio/run.sh
cp $B/out/modules/tmd3702.ko $B/bundle/als/modules/; cat $HDR $SRC/audio/v74-test/als-run.body.sh > $B/bundle/als/run.sh
for d in audio als; do ( cd $B/bundle/$d && sed -i 's/\r$//' run.sh mixer/*.txt 2>/dev/null; find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS ); done
bash -n $B/bundle/audio/run.sh && bash -n $B/bundle/als/run.sh && echo SYNTAX_OK
rm -rf $A/v74; mkdir -p $A/v74 $A/modules; cp -r $B/bundle/audio $B/bundle/als $A/v74/; cp $B/out/modules/*.ko $A/modules/
cp $W/firmware/extracted/vendor/firmware/tfa98xx.cnt $A/modules/tfa98xx.cnt
( cd $A && find . -type f ! -name SHA256SUMS-all | sort | xargs sha256sum > SHA256SUMS-all ); grep -v "/modules/q6\|/modules/snd\|/modules/qmi\|/modules/qcom\|/modules/pdr\|/modules/apr\|/modules/sound\|/modules/pinctrl\|/modules/mc\|/modules/soc-usb" $A/SHA256SUMS-all
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"; SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
timeout 60 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v74 && rm -rf A6L-usb-20260915/v74/audio A6L-usb-20260915/v74/als' && \
timeout 120 $SCP -r -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio-20260923/v74/audio C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio-20260923/v74/als a6l-laptop:A6L-usb-20260915/v74/ && \
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v74 && (cd audio && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_AUDIO_OK); (cd als && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_ALS_OK); du -sh audio als' || echo LAPTOP_COPY_FAIL
echo AUDIO_SENSORS_V74_DONE
