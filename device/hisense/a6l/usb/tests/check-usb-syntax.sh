#!/usr/bin/env bash
# A6L USB gadget HAL: -fsyntax-only -Wall -Wextra -Werror of every source with the Lineage tree's clang, bionic/libc++,
# libbinder_ndk and the generated android.hardware.usb.gadget-V2-ndk headers (no `m`, nothing written to the tree).
# WSL, as a6l:  bash device/hisense/a6l/usb/tests/check-usb-syntax.sh   -> A6L_USB_SYNTAX PASS|FAIL
set -u
T=${A6L_TREE:-/home/a6l/android/a6l-lineage24}; D=${A6L_USB_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
CC=$T/prebuilts/clang/host/linux-x86/clang-r596125/bin/clang++
G=out/soong/.intermediates/hardware/interfaces/usb/gadget/aidl/android.hardware.usb.gadget-V2-ndk-source/gen/include
F="-nostdlibinc -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -D__ANDROID_VENDOR_API__=202604 -D__BIONIC_NO_PAGE_SIZE_MACRO -target aarch64-linux-android37 -fPIE -Wimplicit-fallthrough -Wno-gnu-include-next -fno-short-enums -Werror=non-virtual-dtor -I$D/gadget -I$G -Iframeworks/native/libs/binder/ndk/include_cpp -Iframeworks/native/libs/binder/ndk/include_ndk -Iframeworks/native/libs/binder/ndk/include_platform -Isystem/libbase/include -Iexternal/fmtlib/include -Isystem/logging/liblog/include_vndk -Iprebuilts/clang/host/linux-x86/clang-r596125/android_libc++/platform/aarch64/include/c++/v1 -Iprebuilts/clang/host/linux-x86/clang-r596125/include/c++/v1 -isystem bionic/libc/include -isystem bionic/libc/kernel/uapi/asm-arm64 -isystem bionic/libc/kernel/uapi -isystem bionic/libc/kernel/android/uapi -Wall -Wextra -Werror -std=gnu++20 -fno-rtti"
cd $T || exit 1
[ -d $G ] || { echo "missing $G (needs a previous build)"; echo "A6L_USB_SYNTAX FAIL"; exit 1; }
rc=0
for f in $D/gadget/a6l_gadget_core.cpp $D/gadget/UsbGadget.cpp $D/gadget/service.cpp; do
  if $CC $F -fsyntax-only $f 2> /tmp/a6l-usb-syn.err; then echo "OK $(basename $f)"; else echo "FAIL $(basename $f)"; head -40 /tmp/a6l-usb-syn.err; rc=1; fi
done
[ $rc = 0 ] && echo "A6L_USB_SYNTAX PASS" || echo "A6L_USB_SYNTAX FAIL"; exit $rc
