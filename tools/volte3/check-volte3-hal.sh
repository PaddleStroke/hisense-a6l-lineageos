#!/bin/bash
# volte3: HAL syntax re-check (sendImsSms) vs AIDL V4 headers + rom-v2 kernel series composition in a private scratch dir.
set -u
R=/mnt/c/Users/Pierre/Desktop/A6L; W=/home/a6l/volte3-work; rm -rf $W/radio; cp -r $R/device/hisense/a6l/radio $W/radio
T=/home/a6l/android/a6l-lineage24; S=$W/radio
CC=$T/prebuilts/clang/host/linux-x86/clang-r596125/bin/clang++
G=out/soong/.intermediates/hardware/interfaces/radio/aidl
F=$(sed -n 's/^F="\(-nostdlibinc.*\)"$/\1/p' $R/tools/volte3/build-volte3.sh | head -1)
F=$(eval echo "$F")
for x in radio radio.config radio.data radio.messaging radio.modem radio.network radio.sim radio.voice; do F="$F -I$G/android.hardware.$x-V4-ndk-source/gen/include"; done
F2=$(grep -o '^F="\$F -Isystem.*"$' $R/tools/volte3/build-volte3.sh | sed 's/^F="\$F //; s/"$//')
F="$F $F2"
( cd $T; for f in $S/hal/*.cpp; do
  if $CC $F -fsyntax-only $f 2> $W/out/syn2.err; then echo "OK $(basename $f)"; else echo "FAIL $(basename $f)"; head -30 $W/out/syn2.err; fi; done ) | tee $W/out/syntax2.txt
grep -q FAIL $W/out/syntax2.txt && echo "SYNTAX2 rc=1" || echo "SYNTAX2 rc=0"
sed 's|S=/home/a6l/rom-v2/kseries|S=/home/a6l/volte3-kseries|' $R/tools/check-rom-v2-kernel-series.sh | tr -d '\r' > $W/kseries.sh
bash $W/kseries.sh 2>&1 | tail -16
echo "== DONE $(date)"
