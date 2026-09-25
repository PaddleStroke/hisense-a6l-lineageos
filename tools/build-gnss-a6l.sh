#!/usr/bin/env bash
# A6L GNSS (agent gnss, 24 Sep 2026): offline build + tests, WSL. No phone.
#  1. host unit/integration tests (g++, ASan/UBSan, TSan) of liba6l_qmiloc against the fake modem
#  2. CLI replay of a synthesized QMI LOC log (make_replay.py) through the real client/engine
#  3. static NDK arm64 build of a6l_gnss_test for the V74 recovery RAM session
#  4. artifacts + sha256 -> firmware/extracted/gnss-20260924/
# usage: build-gnss-a6l.sh   (prints GNSS_BUILD_PASS / GNSS_BUILD_FAIL)
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L
G=$R/device/hisense/a6l/gnss
W=/home/a6l/gnss-build
OUT=$R/firmware/extracted/gnss-20260924
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
rm -rf $W; mkdir -p $W $OUT; cp -r $G/. $W/src/
find $W/src -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.py' -o -name '*.bp' -o -name '*.rc' -o -name '*.mk' \) -exec sed -i 's/\r$//' {} +
cd $W/src
SRC="lib/qmi.cpp lib/loc_v02.cpp lib/nmea.cpp lib/loc_client.cpp lib/android_map.cpp lib/qrtr_transport.cpp"
fail=0
echo "== host tests (asan+ubsan)"
g++ -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined -Ilib test/host_test.cpp $SRC -o $W/host_asan && $W/host_asan || fail=1
echo "== host tests (tsan)"
g++ -std=c++17 -O1 -g -pthread -fsanitize=thread -Ilib test/host_test.cpp $SRC -o $W/host_tsan && setarch "$(uname -m)" -R $W/host_tsan > $W/tsan.log 2>&1   # -R: TSan cannot map with WSL high-entropy ASLR
grep -q "WARNING: ThreadSanitizer" $W/tsan.log && { echo TSAN_RACE; grep -A12 "WARNING: ThreadSanitizer" $W/tsan.log | head -40; fail=1; }
tail -1 $W/tsan.log; grep -q "A6L_GNSS_HOST_TESTS PASS" $W/tsan.log || { echo TSAN_NOT_RUN; fail=1; }
echo "== CLI replay"
g++ -std=c++17 -O1 -Wall -Wextra -Werror -pthread -Ilib tools/a6l_gnss_test.cpp $SRC -o $W/a6l_gnss_test_host || fail=1
python3 test/make_replay.py $W/synth.log
$W/a6l_gnss_test_host --replay $W/synth.log --speed 4 --seconds 30 --record $W/rec.log > $W/replay.out; rc=$?
tail -1 $W/replay.out; [ $rc = 0 ] && grep -q "result=FIX fixes=5 nmea=10 sv_reports=8 intermediate=3" $W/replay.out || { echo REPLAY_FAIL; fail=1; }
# the tool's own packet log must replay too (record -> replay round trip); it holds only responses + the same indications
$W/a6l_gnss_test_host --replay $W/rec.log --speed 8 --seconds 30 > $W/replay2.out; tail -1 $W/replay2.out
grep -q "result=FIX fixes=5" $W/replay2.out || { echo REPLAY2_FAIL; fail=1; }
$W/a6l_gnss_test_host --list > $W/list.out; echo "list rc=$? (4 = no AF_QIPCRTR in WSL, expected)"; cat $W/list.out
echo "== NDK static arm64"
$NDK/aarch64-linux-android34-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -static -static-libstdc++ -Ilib \
    tools/a6l_gnss_test.cpp $SRC -o $W/a6l_gnss_test || fail=1
$NDK/llvm-strip $W/a6l_gnss_test
file $W/a6l_gnss_test
QA=$(command -v qemu-aarch64-static || command -v qemu-aarch64)
if [ -n "$QA" ]; then
  $QA $W/a6l_gnss_test --replay $W/synth.log --speed 4 --seconds 30 > $W/replay-arm64.out; tail -1 $W/replay-arm64.out
  grep -q "result=FIX fixes=5" $W/replay-arm64.out || { echo ARM64_REPLAY_FAIL; fail=1; }
else echo "no qemu-aarch64 user emulator: arm64 replay skipped"; fi
echo "== NDK compile-check of the HAL sources' library (Android bionic, -Werror)"
for f in $SRC; do $NDK/aarch64-linux-android34-clang++ -std=c++17 -O2 -Wall -Wextra -Werror -Ilib -c $f -o /dev/null || fail=1; done
cp $W/a6l_gnss_test $W/synth.log $W/replay.out $OUT/
( cd $OUT && sha256sum a6l_gnss_test synth.log > SHA256SUMS && cat SHA256SUMS )
[ $fail = 0 ] && echo GNSS_BUILD_PASS || echo GNSS_BUILD_FAIL
