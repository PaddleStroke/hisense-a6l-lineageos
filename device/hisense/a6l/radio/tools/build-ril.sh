#!/usr/bin/env bash
# Hisense A6L radio HAL (agent ril): build everything offline in WSL.
#  1. host unit tests (g++ ASan/UBSan and clang)       -> $RO/host-tests.txt
#  2. static NDK a6l-qmi CLI for the recovery RAM test -> $RO/a6l-qmi (aarch64, static bionic)
#  3. Lineage tree: m android.hardware.radio-service.a6l a6l-qmi a6l-qmi-tests (+ run the host test)
# usage: build-ril.sh <radio src dir> <out dir> [nom]   (nom = skip step 3)
set -uo pipefail
SRC=$1; RO=$2; mkdir -p "$RO"
T=/home/a6l/android/a6l-lineage24
NDK=/home/a6l/ndk/android-ndk-r27c
echo "== 1 host tests $(date)"
( cd "$SRC" && CXX=g++ OUT=$RO/qmi-tests-gcc sh tests/run-host-tests.sh ) > "$RO/host-tests.txt" 2>&1; echo "gcc rc=$?" >> "$RO/host-tests.txt"
if command -v clang++ >/dev/null; then ( cd "$SRC" && CXX=clang++ OUT=$RO/qmi-tests-clang sh tests/run-host-tests.sh ) >> "$RO/host-tests.txt" 2>&1; echo "clang rc=$?" >> "$RO/host-tests.txt"; fi
tail -4 "$RO/host-tests.txt"
echo "== 2 NDK static CLI $(date)"
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android34-clang++
$CC -std=c++17 -O2 -static -Wall -Wextra -Werror -DA6L_QMI_NO_LIBLOG -I"$SRC/qmi/include" \
    "$SRC"/qmi/src/*.cc "$SRC/tools/a6l_qmi_cli.cc" -o "$RO/a6l-qmi-static" && \
    $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip "$RO/a6l-qmi-static" && file "$RO/a6l-qmi-static" && sha256sum "$RO/a6l-qmi-static"
[ "${3:-}" = nom ] && exit 0
echo "== 3 Lineage m $(date)"
rm -rf "$T/device/hisense/a6l/radio"; cp -r "$SRC" "$T/device/hisense/a6l/radio"
cd "$T"
export A6L_SOONG_GOMEMLIMIT=36GiB
set +u
source build/envsetup.sh >/dev/null
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug >/dev/null
set -u
NINJA_ARGS="-k 0" m -j8 android.hardware.radio-service.a6l a6l-qmi a6l-qmi-tests > "$RO/m.log" 2>&1; rc=$?
echo "m rc=$rc"; grep -E 'error:|FAILED|Error' "$RO/m.log" | head -60
if [ $rc = 0 ]; then
  P=$T/out/target/product/a6l
  for f in vendor/bin/hw/android.hardware.radio-service.a6l vendor/bin/a6l-qmi; do sha256sum "$P/$f"; cp "$P/$f" "$RO/"; done
  H=$(find $T/out/host/linux-x86 -name a6l-qmi-tests -type f | head -1); echo "host test: $H"; [ -n "$H" ] && "$H" | tail -2
fi
echo "== done $(date)"
