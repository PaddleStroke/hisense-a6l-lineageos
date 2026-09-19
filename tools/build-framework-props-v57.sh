#!/usr/bin/env bash
# V57: rebuild the VM supervisor with the widened sys.* property allowlist and ctl.* value logging.
set -eo pipefail
cd /home/a6l/android/a6l-lineage24
log=/home/a6l/logs/build-framework-props-v57-r${1:-1}.log
test ! -e "$log"
exec > "$log" 2>&1
cp /mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l/diagnostic/framework_root_services.c \
   /mnt/c/Users/Pierre/Desktop/A6L/device/hisense/a6l/diagnostic/framework_root_properties.h device/hisense/a6l/diagnostic/
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 a6l_framework_root_services
