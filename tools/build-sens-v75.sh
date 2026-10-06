#!/bin/bash
# sens agent (26 Sep 2026): build + assemble the attended bundles v75/sens (SMGR accel/gyro/COMPASS readout) and v75/speaker
# (TFA9894 loudspeaker, superset of v75/audio6), copy them to the repo and the laptop. Offline only; no `m`.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L
K=/home/a6l/kernel/a6l-baseline-7.2
O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
B=/home/a6l/build/sens; rm -rf $B; mkdir -p $B
SRC=$R/device/hisense/a6l/sens
X=$R/firmware/extracted
OUTR=$X/sens-20260926
export PATH=$CL:$PATH
st() { echo "=== $(date +%T) $*"; }
fail() { echo "SENS_BUILD_FAIL $*"; exit 1; }

st "1. a6l_imu: host build + dry-run on fake sysfs, then NDK static aarch64"
mkdir -p $B/imu; cp $SRC/imu/a6l_imu.c $SRC/imu/mkfake.py $B/imu/; cd $B/imu
gcc -Wall -Wextra -Werror -Wno-format-truncation -O2 -o a6l_imu-host a6l_imu.c -lm || fail "host build"
for m in flat turn tilt; do rm -rf fk; python3 mkfake.py fk $m; A6L_IMU_SYS=$PWD/fk/sys A6L_IMU_DEV=$PWD/fk/dev ./a6l_imu-host -n $m -s 1 -c $m -p 5000 | grep -E "_PASS|_FAIL|GRAVITY|HEADING" ; done
$NDK/aarch64-linux-android34-clang -static -O2 -Wall -Wextra -Werror -Wno-unknown-warning-option -Wno-format-truncation -o a6l_imu a6l_imu.c -lm || fail ndk
$NDK/llvm-strip a6l_imu; file a6l_imu | cut -c1-120

st "2. speaker overlay module (kvoice source, dtbo = ROM a6l-voice-speaker-onbase-v75)"
KV=$R/device/hisense/a6l/kernel/kvoice; OV=$B/spkovl; mkdir -p $OV
dtc -@ -q -I dts -O dtb -o $OV/spk.dtbo $KV/dt/a6l-voice-speaker-onbase-v75.dtso || fail dtc
V=$X/recovery-v74-candidate-20260923/merged-captured-abl.dtb
fdtoverlay -i $V -o $OV/m-v74-spk.dtb $OV/spk.dtbo || fail fdtoverlay
python3 $R/tools/check-voice-dt-links.py $OV/m-v74-spk.dtb | tail -1
python3 $R/tools/check-audio-dt-links.py $OV/m-v74-spk.dtb --rule reg --expect-tfa | tail -1
echo "chosen: $(fdtget $OV/m-v74-spk.dtb /chosen hisense,a6l-voice) amp: $(fdtget $OV/m-v74-spk.dtb /soc@0/i2c@c1b6000/audio-amplifier@34 compatible 2>&1)"
sed -e 's/a6l_voice_ovl (kvoice agent, 24 Sep 2026)/a6l_spk_ovl (sens agent, 26 Sep 2026, from kvoice a6l_voice_ovl)/' \
    -e 's/Applies a6l-voice-onbase-v75.dtbo/Applies a6l-voice-speaker-onbase-v75.dtbo (ROM DT #1: voice + TFA9894 speaker link)/' \
    -e 's/q6mvm\/q6cvs\/q6cvp + VoiceMMode1 link/q6mvm\/q6cvs\/q6cvp + VoiceMMode1 + Speaker Playback (speaker variant)/' \
    -e 's/MODULE_DESCRIPTION("A6L attended-test DT overlay for q6voice (call audio)")/MODULE_DESCRIPTION("A6L attended-test DT overlay: q6voice + TFA9894 speaker")/' \
    $KV/ovl/a6l_voice_ovl.c > $OV/a6l_spk_ovl.c
grep -q "speaker variant" $OV/a6l_spk_ovl.c || fail "sed ovl"
echo 'obj-m += a6l_spk_ovl.o' > $OV/Kbuild
python3 - "$OV/spk.dtbo" "$OV/a6l_voice_dtbo.h" <<'PY'
import sys
b=open(sys.argv[1],'rb').read()
l=[', '.join('0x%02x'%x for x in b[i:i+12]) for i in range(0,len(b),12)]
open(sys.argv[2],'w').write('/* generated from a6l-voice-speaker-onbase-v75.dtbo (%d bytes) */\nstatic const unsigned char a6l_voice_dtbo[] __aligned(8) = {\n\t%s\n};\n'%(len(b),',\n\t'.join(l)))
PY
make -C $K O=$O ARCH=arm64 LLVM=1 M=$OV W=1 modules > $B/spkovl-build.log 2>&1 || { tail -20 $B/spkovl-build.log; fail ovl; }
grep -i warning $B/spkovl-build.log | head -5
echo "vermagic spk_ovl: $(modinfo -F vermagic $OV/a6l_spk_ovl.ko) | audio6 q6afe: $(modinfo -F vermagic $X/audio6-20260925/v75/audio6/modules/q6afe.ko) | tfa: $(modinfo -F vermagic $X/audio3-20260924/v75/audio3/extra/snd-soc-tfa98xx.ko)"
cp $OV/a6l_spk_ovl.c $OV/a6l_voice_dtbo.h $OV/Kbuild $SRC/speaker/

st "3. tones (-40 / -20 dBFS, 1 kHz, 48 kHz stereo 16 bit, 2 s, 20 ms fades) + regbytes"
mkdir -p $B/wav; python3 - $B/wav <<'PY'
import sys, math, struct, wave
for lvl in (40, 20):
    a = 32767 * 10 ** (-(lvl + 0.05) / 20); n = 96000; fr = bytearray()
    for i in range(n):
        g = min(1.0, i / 960, (n - 1 - i) / 960); v = int(round(a * g * math.sin(2 * math.pi * 1000 * i / 48000)))
        fr += struct.pack('<hh', v, v)
    w = wave.open('%s/sine1k-m%ddBFS-stereo-2s.wav' % (sys.argv[1], lvl), 'wb'); w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000); w.writeframes(bytes(fr)); w.close()
PY
python3 -c "open('$B/regbytes','wb').write(bytes(range(256)))"
gcc -O2 -o $B/wavlevel-host $R/device/hisense/a6l/audio/v75-test/audio6/a6l_wavlevel.c -lm && for w in $B/wav/*.wav; do $B/wavlevel-host $w | grep A6L_AU6_LEVEL | head -1; done

st "4. bundle v75/sens"
BS=$B/bundle/sens; mkdir -p $BS/bin $BS/modules $BS/firmware/qcom/sensors
SA=$X/v71-attended-bundle-20260922/sensors-adsp
cp $SA/modules/*.ko $SA/modules/order.txt $BS/modules/ && cp $SA/firmware/qcom/sensors/sns.reg $BS/firmware/qcom/sensors/ || fail "sensors-adsp copy"
cmp $SA/firmware/qcom/sensors/sns.reg $X/sensors-registry-20260922/sns.reg && echo "sns.reg = sensors-registry-20260922 copy"
cp $B/imu/a6l_imu $BS/bin/; cp $SRC/imu/run.sh $BS/run.sh
( cd $BS && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo "sens bundle-hash-ok $(wc -l < SHA256SUMS) files" )

st "5. bundle v75/speaker (audio6 modules + TFA pieces)"
A6=$X/audio6-20260925/v75/audio6; A3=$X/audio3-20260924/v75/audio3
BP=$B/bundle/speaker; mkdir -p $BP/bin $BP/extra $BP/firmware $BP/mixer $BP/wav
cp -r $A6/modules $BP/ || fail "audio6 modules"
for f in tinymix tinyplay a6l_wavlevel pcmprobe; do cp $A6/bin/$f $BP/bin/ || fail "bin $f"; done
cp $B/regbytes $BP/bin/regbytes
cp $OV/a6l_spk_ovl.ko $A3/extra/pinctrl-sdm660-lpass-lpi.ko $A3/extra/snd-soc-tfa98xx.ko $BP/extra/ || fail extra
cp $X/vendor/firmware/tfa98xx.cnt $BP/firmware/ || fail cnt
cmp $X/vendor/firmware/tfa98xx.cnt $A3/firmware/tfa98xx.cnt && echo "tfa98xx.cnt = stock vendor copy (unchanged)"
cp $SRC/speaker/mixer/*.txt $BP/mixer/; cp $B/wav/*.wav $BP/wav/; cp $SRC/speaker/run.sh $BP/run.sh
modinfo $BP/extra/snd-soc-tfa98xx.ko | grep -E "^(depends|vermagic|alias:.*of)" | head -5
strings $BP/extra/snd-soc-tfa98xx.ko | grep -c A6L_TFA_STUB | sed 's/^/stub strings: /'
( cd $BP && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS && sha256sum -c --quiet SHA256SUMS && echo "speaker bundle-hash-ok $(wc -l < SHA256SUMS) files" )

st "6. repo copy + laptop"
rm -rf $OUTR/v75; mkdir -p $OUTR/v75 $OUTR/dt $OUTR/logs
cp -r $BS $BP $OUTR/v75/; cp $OV/spk.dtbo $OUTR/dt/a6l-voice-speaker-onbase-v75.dtbo; cp $OV/m-v74-spk.dtb $OUTR/dt/; cp $B/spkovl-build.log $OUTR/logs/
( cd $OUTR && find . -type f ! -name SHA256SUMS-all | sort | xargs sha256sum > SHA256SUMS-all )
SCP=/mnt/c/Windows/System32/OpenSSH/scp.exe
for b in sens speaker; do
  WIN=$(wslpath -w $OUTR/v75/$b)
  timeout 120 $SCP -r -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf "$WIN" a6l-laptop:A6L-usb-20260915/v75/ && echo "LAPTOP_STAGED $b" || echo "LAPTOP_STAGE_FAILED $b"
done
timeout 60 $R/.relay/lap.sh 55 'cd ~/A6L-usb-20260915/v75 && for b in sens speaker; do (cd $b && sha256sum -c --quiet SHA256SUMS && echo "LAPTOP_HASH_OK $b $(wc -l < SHA256SUMS) files"); done' 2>&1 | tail -4
grep -E "run.sh|a6l_imu|a6l_spk_ovl|tfa98xx|pinctrl-sdm660|wav$" $BS/SHA256SUMS $BP/SHA256SUMS | sed 's|^.*/SHA256SUMS:||'
echo SENS_BUILD_DONE
