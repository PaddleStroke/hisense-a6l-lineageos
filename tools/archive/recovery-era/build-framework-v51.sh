#!/usr/bin/env bash
set -euo pipefail
cd /home/a6l/android/a6l-lineage24
workspace=/mnt/c/Users/Pierre/Desktop/A6L
log=${A6L_FRAMEWORK_BUILD_LOG:-/home/a6l/logs/build-framework-v51-r1.log}
test ! -e "$log"
cp "$workspace/device/hisense/a6l/diagnostic/framework_root_properties.h" device/hisense/a6l/diagnostic/
cp "$workspace/device/hisense/a6l/diagnostic/framework_root_services.c" device/hisense/a6l/diagnostic/
prebuilts/build-tools/linux-x86/bin/ninja -f out/combined-lineage_gsi_a6l.ninja out/target/product/a6l/system/bin/a6l_framework_root_services > "$log" 2>&1
tail -n 5 "$log"
