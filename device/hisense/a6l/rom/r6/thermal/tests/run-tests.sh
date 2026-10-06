#!/usr/bin/env bash
# Host test of the A6L thermal HAL core (no Android build). usage: bash rom/r6/thermal/tests/run-tests.sh
# ASan/UBSan when the host toolchain has them, plain -O1 otherwise (WSL image without libasan).
set -u; T=$(cd "$(dirname "$0")" && pwd); O=$(mktemp -d); CXX=${CXX:-$(command -v g++ || command -v clang++)}
[ -n "$CXX" ] || { echo "A6L_THERMAL_TEST FAIL (no C++ compiler)"; exit 1; }
F="-std=c++17 -Wall -Wextra -Werror -g"
$CXX $F -fsanitize=address,undefined -o $O/t $T/test_thermal_logic.cpp $T/../thermal_logic.cpp 2> $O/err \
  || $CXX $F -O1 -o $O/t $T/test_thermal_logic.cpp $T/../thermal_logic.cpp 2>> $O/err \
  || { tail -5 $O/err; echo "A6L_THERMAL_TEST FAIL (build)"; rm -rf $O; exit 1; }
# thermal-r5prep (29 Sep): second binary = source alternatives / IIO scan / cooling devices (test_sources_cooling.cpp)
S="$T/test_sources_cooling.cpp $T/../thermal_logic.cpp $T/../cooling_logic.cpp"
$CXX $F -fsanitize=address,undefined -o $O/s $S 2>> $O/err || $CXX $F -O1 -o $O/s $S 2>> $O/err \
  || { tail -5 $O/err; echo "A6L_THERMAL_TEST FAIL (build sources/cooling)"; rm -rf $O; exit 1; }
tr -d '\r' < $T/../thermal-a6l.conf > $O/conf
$O/s $O/conf | tail -1; rs=${PIPESTATUS[0]}
$O/t $O/conf; rc=$?; [ $rs = 0 ] || { echo "A6L_THERMAL_TEST FAIL (sources/cooling)"; rc=1; }; rm -rf $O; exit $rc
