#!/bin/sh
# Offline host build + run of the QMI/QRTR unit tests (no Android deps). Usage: sh run-host-tests.sh [-v]
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
CXX=${CXX:-g++}
OUT=${OUT:-/tmp/a6l-qmi-tests}
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/qmi_tests.cc -o "$OUT"
"$OUT" "$@"
