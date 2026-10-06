#!/usr/bin/env bash
# A6L camera provider fork (lc2, 29 Sep 2026): -fsyntax-only -Wall -Werror of every source with the Lineage tree's clang,
# bionic/libc++ and NDK headers of the FROZEN V1 camera AIDL interfaces (generated here with the tree's aidl into
# $G, because the last ROM build only generated V2+/V4 headers). No `m`, nothing written to the tree or out/.
# WSL, as a6l:  bash device/hisense/a6l/camera/provider/tests/check-provider-syntax.sh   -> A6L_PROVIDER_SYNTAX PASS|FAIL
set -u
T=${A6L_TREE:-/home/a6l/android/a6l-lineage24}; D=${A6L_PROV_DIR:-$(cd "$(dirname "$0")/.." && pwd)}
CLANG=${A6L_CLANG:-clang-r584948}; CC=$T/prebuilts/clang/host/linux-x86/$CLANG/bin/clang++
G=${A6L_PROV_GEN:-/tmp/a6l-camprov-aidl}; AIDL=$T/out/host/linux-x86/bin/aidl
cd $T || exit 1
[ -x $AIDL ] || { echo "missing $AIDL (needs a previous build)"; echo "A6L_PROVIDER_SYNTAX FAIL"; exit 1; }
H=hardware/interfaces
api() { echo $H/$1/aidl_api/android.hardware.$2/$3; }
IMPORTS="$(api common/aidl common 2) $(api common/fmq/aidl common.fmq 1) $(api camera/common/aidl camera.common 1) $(api camera/metadata/aidl camera.metadata 1) $(api graphics/common/aidl graphics.common 7) $(api camera/device/aidl camera.device 1) $(api camera/provider/aidl camera.provider 1)"
rm -rf $G; mkdir -p $G/include $G/src
for root in $IMPORTS; do
  inc=""; for r in $IMPORTS; do inc="$inc -I $r"; done
  v=$(basename $root); $AIDL --lang=ndk --structured --stability=vintf --version=$v --hash=$(tail -1 $root/.hash) $inc -o $G/src -h $G/include $(find $root -name '*.aidl') > $G/aidl.log 2>&1 || { cat $G/aidl.log | head -20; echo "A6L_PROVIDER_SYNTAX FAIL (aidl $root)"; exit 1; }
done
LCXX=prebuilts/clang/host/linux-x86/$CLANG
F="-nostdlibinc -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -D__ANDROID_VENDOR_API__=202604 -D__BIONIC_NO_PAGE_SIZE_MACRO -target aarch64-linux-android37 -fPIE
 -Wno-gnu-include-next -fno-short-enums -Werror=non-virtual-dtor -Wall -Werror -std=gnu++20 -fno-rtti
 -I$D/device -I$D/provider -I$G/include
 -I$H/camera/common/default/include -I$H/common/support/include
 -Iframeworks/native/libs/binder/ndk/include_cpp -Iframeworks/native/libs/binder/ndk/include_ndk -Iframeworks/native/libs/binder/ndk/include_platform
 -Isystem/libbase/include -Iexternal/fmtlib/include -Isystem/logging/liblog/include_vndk -Isystem/logging/liblog/include
 -Isystem/core/libcutils/include -Isystem/core/libutils/include -Isystem/core/libsystem/include -Isystem/core/libprocessgroup/include
 -Isystem/libfmq/include -Isystem/libfmq/base -Isystem/libhidl/base/include -Isystem/libhidl/transport/include -Isystem/libhwbinder/include
 -Ihardware/libhardware/include -Isystem/media/camera/include -Isystem/media/private/camera/include
 -Iframeworks/native/libs/ui/include_vndk -Iframeworks/native/libs/ui/include -Iframeworks/native/libs/nativewindow/include
 -Iframeworks/native/libs/nativebase/include -Iframeworks/native/libs/arect/include -Iframeworks/native/libs/math/include
 -Iframeworks/native/libs/gralloc/types/include -Iframeworks/native/include
 -I$LCXX/android_libc++/platform/aarch64/include/c++/v1 -I$LCXX/include/c++/v1
 -isystem bionic/libc/include -isystem bionic/libc/kernel/uapi/asm-arm64 -isystem bionic/libc/kernel/uapi -isystem bionic/libc/kernel/android/uapi"
rc=0
for f in $D/device/A6lTorch.cpp $D/device/CameraDevice.cpp $D/device/CameraDeviceSession.cpp $D/device/convert.cpp \
         $D/provider/CameraProvider.cpp $D/provider/service.cpp; do
  if $CC $F -fsyntax-only $f 2> /tmp/a6l-camprov-syn.err; then echo "OK $(basename $f)"; else echo "FAIL $(basename $f)"; grep -E "error|fatal" /tmp/a6l-camprov-syn.err | head -20; rc=1; fi
done
[ $rc = 0 ] && echo "A6L_PROVIDER_SYNTAX PASS" || echo "A6L_PROVIDER_SYNTAX FAIL"; exit $rc
