#!/usr/bin/env bash
# V59: supervisor (open property service + idmap2d ctl.start) and the control-socket launcher.
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
log=/home/a6l/logs/build-framework-netd-v59-r${1:-1}.log
test ! -e "$log"
exec > "$log" 2>&1
W=/mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l
diff <(tr -d '\r' < "$W/Android.bp" | head -n -12) <(tr -d '\r' < device/hisense/a6l/Android.bp | head -n 400) > /dev/null || echo "NOTE: Android.bp differed beyond the new module"
cp "$W/Android.bp" device/hisense/a6l/
cp "$W/diagnostic/framework_root_services.c" "$W/diagnostic/framework_root_properties.h" "$W/diagnostic/socket_exec.c" device/hisense/a6l/diagnostic/
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_framework_root_services a6l_socket_exec
