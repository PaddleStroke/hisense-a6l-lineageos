#!/usr/bin/env bash
# A6L USB gadget core host tests (android-usb, 29 Sep 2026): builds a6l_gadget_core.cpp + the test with ASan/UBSan
# (g++ or clang++) and runs it 3 times (monitor thread timing). usage: bash usb/tests/run-host-tests.sh
set -u
D=$(cd "$(dirname "$0")/.." && pwd); W=$(mktemp -d); CXX=${CXX:-$(command -v g++ || command -v clang++)}
$CXX -std=c++17 -Wall -Wextra -Werror -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -pthread \
  -I$D/gadget $D/gadget/a6l_gadget_core.cpp $D/tests/test_gadget_core.cpp -o $W/t || { echo "A6L_USB_HOST_TESTS FAIL (build)"; exit 1; }
rc=0; for i in 1 2 3; do $W/t || rc=1; done
rm -rf $W; [ $rc = 0 ] && echo "A6L_USB_HOST_TESTS PASS" || echo "A6L_USB_HOST_TESTS FAIL"; exit $rc
