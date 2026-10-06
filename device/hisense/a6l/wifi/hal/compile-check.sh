#!/usr/bin/env bash
# Offline compile check of wifi/hal with the Lineage tree's clang and soong flags (no `m`: the tree's out/ is partly
# root-owned). Flags: global cc config + wpa_supplicant's own cFlags taken from the tree's last soong ninja.
# usage: compile-check.sh <lineage tree> <this dir> <work dir>   -> A6L_WIFI_HAL_COMPILE PASS|FAIL
set -u
T=$1; S=$2; W=$3; mkdir -p $W; cd $T
N=$(ls out/soong/build.lineage_gsi_a6l.*.ninja)
[ -s $W/gflags.txt ] || grep -h -E '^g\.android\.soong\.cc\.config\.[A-Za-z0-9-]+ = ' $N > $W/gflags.txt
[ -s $W/wpa.txt ] || { for f in $N; do grep -m1 -E '^m\.wpa_supplicant_android_vendor_arm64_armv8-a_cfi\.cFlags1 ' $f; done > $W/wpa.txt; }
expand() {  # replace ${g.android.soong.cc.config.X} by its value
    python3 - "$W/gflags.txt" "$1" <<'PY'
import re,sys
g={}
for l in open(sys.argv[1]):
    k,_,v=l.rstrip('\n').partition(' = '); g.setdefault(k,v)
s=sys.argv[2]
for _ in range(5): s=re.sub(r'\$\{(g\.[^}]+)\}', lambda m: g.get(m.group(1),''), s)
print(s)
PY
}
CL=prebuilts/clang/host/linux-x86/clang-r596125/bin/clang
CXXLIB="-Iprebuilts/clang/host/linux-x86/clang-r596125/android_libc++/platform/aarch64/include/c++/v1 -Iprebuilts/clang/host/linux-x86/clang-r596125/include/c++/v1"
SYS="-isystem bionic/libc/include -isystem bionic/libc/kernel/uapi/asm-arm64 -isystem bionic/libc/kernel/uapi -isystem bionic/libc/kernel/android/scsi -isystem bionic/libc/kernel/android/uapi"
BASE=$(expand '-nostdlibinc -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -D__ANDROID_VENDOR_API__=202604 ${g.android.soong.cc.config.Arm64Cflags} ${g.android.soong.cc.config.CommonGlobalCflags} ${g.android.soong.cc.config.DeviceGlobalCflags} ${g.android.soong.cc.config.Arm64armv8-aVariantCflags} -target aarch64-linux-android37 -fPIC')
TAIL=$(expand '${g.android.soong.cc.config.NoOverrideGlobalCflags} ${g.android.soong.cc.config.NoOverride64GlobalCflags}')
CPP=$(expand '${g.android.soong.cc.config.CommonGlobalCppflags} ${g.android.soong.cc.config.DeviceGlobalCppflags} ${g.android.soong.cc.config.Arm64Cppflags}')
fails=0
echo "== libwifi-hal-a6l"
$CL -x c++ $BASE $CPP -Ihardware/interfaces/wifi/legacy_headers/include -Iexternal/libnl/include -Isystem/core/libcutils/include \
    -Isystem/logging/liblog/include_vndk $CXXLIB $SYS -Wall -Wextra -Werror -std=gnu++20 -fno-rtti $TAIL \
    -c $S/a6l_wifi_hal.cpp -o $W/a6l_wifi_hal.o && echo "ok   libwifi-hal-a6l" || { echo "FAIL libwifi-hal-a6l"; fails=$((fails+1)); }
# wpa_supplicant cFlags (module-specific part after the target flags; its own -I list included)
WPA=$(expand "$(sed 's/^[^=]*= //' $W/wpa.txt)")
for v in qca plain; do
    F="$WPA"; [ $v = plain ] && F=$(echo "$F" | sed 's/-DCONFIG_DRIVER_NL80211_QCA//g')
    echo "== lib_driver_cmd_a6l ($v nl80211)"
    eval "set -- $F"
    $CL -x c "$@" -Iexternal/libnl/include -Iexternal/wpa_supplicant_8/src -Iexternal/wpa_supplicant_8/src/common \
        -Iexternal/wpa_supplicant_8/src/drivers -Iexternal/wpa_supplicant_8/src/utils -Iexternal/wpa_supplicant_8/wpa_supplicant \
        -Isystem/core/libcutils/include -DCONFIG_ANDROID_LOG -Wall -Werror -Wno-unused-parameter -fPIC \
        -c $S/driver_cmd_a6l.c -o $W/driver_cmd_a6l.$v.o && echo "ok   lib_driver_cmd_a6l $v" || { echo "FAIL lib_driver_cmd_a6l $v"; fails=$((fails+1)); }
done
prebuilts/clang/host/linux-x86/clang-r596125/bin/llvm-nm $W/a6l_wifi_hal.o | grep -E ' T init_wifi_vendor_hal_func_table'
prebuilts/clang/host/linux-x86/clang-r596125/bin/llvm-nm $W/driver_cmd_a6l.plain.o | grep -E ' T '
[ $fails = 0 ] && echo "A6L_WIFI_HAL_COMPILE PASS" || echo "A6L_WIFI_HAL_COMPILE FAIL $fails"
# The AIDL HAL's chip modes with our WIFI_HAL_INTERFACE_COMBINATIONS (soong passes it as ONE -D argument)
if [ -d out/soong/.intermediates/hardware/interfaces/wifi/aidl/android.hardware.wifi-V4-ndk-source/gen/include ]; then
    COMBO=$(sed -n 's/^WIFI_HAL_INTERFACE_COMBINATIONS := //p' $S/../BoardConfig-wifi.mk)
    echo "== wifi_feature_flags.cpp with WIFI_HAL_INTERFACE_COMBINATIONS=$COMBO"
    $CL -x c++ $BASE $CPP "-DWIFI_HAL_INTERFACE_COMBINATIONS=$COMBO" -Ihardware/interfaces/wifi/aidl/default \
        -Iout/soong/.intermediates/hardware/interfaces/wifi/aidl/android.hardware.wifi-V4-ndk-source/gen/include \
        -Iout/soong/.intermediates/hardware/interfaces/wifi/common/aidl/android.hardware.wifi.common-V2-ndk-source/gen/include \
        -Iframeworks/native/libs/binder/ndk/include_cpp -Iframeworks/native/libs/binder/ndk/include_ndk \
        -Iframeworks/native/libs/binder/ndk/include_platform -Isystem/libbase/include -Iexternal/fmtlib/include \
        -Isystem/core/libcutils/include -Isystem/logging/liblog/include_vndk -Isystem/core/libutils/include \
        -Isystem/core/libsystem/include -Ihardware/interfaces/wifi/legacy_headers/include \
        -Iframeworks/opt/net/wifi/libwifi_hal/include $CXXLIB $SYS -std=gnu++20 -fno-rtti -Wno-error $TAIL \
        -c hardware/interfaces/wifi/aidl/default/wifi_feature_flags.cpp -o $W/wifi_feature_flags.o \
        && echo "ok   wifi_feature_flags.cpp (kV3 mode: [STA] or [AP])" || echo "FAIL wifi_feature_flags.cpp (info only)"
fi
