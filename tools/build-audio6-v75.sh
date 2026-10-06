#!/bin/bash
# audio6 (25 Sep 2026): builds the v75/audio6 bundle (mic selection + levels on MultiMedia2 mono) from the audio5 bundle
# (same modules/binaries) + mixer6 + run.sh + a6l_wavlevel (static aarch64, NDK). Stages it on the laptop and checks it.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L; S=$R/device/hisense/a6l/audio/v75-test/audio6
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
B=/home/a6l/build/audio6; rm -rf $B; mkdir -p $B
f() { echo "A6L_AUDIO6_BUILD_FAIL $1"; exit 1; }
cp $S/a6l_wavlevel.c $B/; sed -i 's/\r$//' $B/a6l_wavlevel.c
gcc -O2 -Wall -Wextra -Werror $B/a6l_wavlevel.c -lm -o $B/wl-host || f host-gcc
$NDK/aarch64-linux-android34-clang -O2 -Wall -Wextra -Werror -static $B/a6l_wavlevel.c -lm -o $B/a6l_wavlevel || f ndk
$NDK/llvm-strip $B/a6l_wavlevel; file $B/a6l_wavlevel
# host tests: the real 25 Sep headset capture must be SILENT; a -20 dBFS sine OK; a truncated header (tinycap killed) readable
$B/wl-host $R/firmware/extracted/audio6-20260925/au5/cap-D-headset-16k.wav | tee $B/t1; grep -q "verdict=SILENT" $B/t1 || f test-silent
python3 - "$B" <<'PY' || f pygen
import struct, math, wave, sys
b = sys.argv[1]
w = wave.open(b + '/sine.wav', 'wb'); w.setnchannels(1); w.setsampwidth(2); w.setframerate(48000)
w.writeframes(b''.join(struct.pack('<h', int(3276.8 * math.sin(2 * math.pi * 440 * i / 48000))) for i in range(96000))); w.close()
d = open(b + '/sine.wav', 'rb').read(); d = d[:40] + b'\0\0\0\0' + d[44:]; open(b + '/trunc.wav', 'wb').write(d)
PY
$B/wl-host $B/sine.wav | tee $B/t2; grep -q "peak=-20.0 .*verdict=OK" $B/t2 || f test-sine
$B/wl-host $B/trunc.wav | tee $B/t3; grep -q "frames=96000 .*verdict=OK" $B/t3 || f test-trunc
A5=$R/firmware/extracted/audio5-20260925/v75/audio5
O=$R/firmware/extracted/audio6-20260925; U=$O/v75/audio6; rm -rf $O/v75; mkdir -p $U/mixer6 $U/bin
cp -r $A5/bin $A5/extra $A5/modules $A5/wav $U/; cp $A5/wav-level.py $U/
cp $B/a6l_wavlevel $U/bin/; for m in $S/mixer6/*.txt; do sed 's/\r$//' "$m" > $U/mixer6/${m##*/}; done
sed 's/\r$//' $S/run.sh > $U/run.sh
bash -n $U/run.sh || f bash-n; dash -n $U/run.sh || f dash-n
cmp -s $U/modules/q6adm.ko $R/firmware/extracted/audio5-20260925/modules/q6adm.ko || f q6adm-differs
(cd $U && find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs -d '\n' sha256sum > SHA256SUMS; grep -E "run.sh|wavlevel|mixer6|q6adm|q6routing" SHA256SUMS; wc -l SHA256SUMS)
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes -o ConnectTimeout=10"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o ConnectTimeout=10"
timeout 60 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75 && cd A6L-usb-20260915/v75 && rm -rf audio6' < /dev/null
timeout 180 $SCP -r -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio6-20260925/v75/audio6 a6l-laptop:A6L-usb-20260915/v75/ < /dev/null || f scp
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/audio6 && sha256sum -c --quiet SHA256SUMS && echo "LAPTOP_AUDIO6_OK $(find . -type f | wc -l) files"' < /dev/null | tr -d '\r'
echo A6L_AUDIO6_BUILD_PASS
