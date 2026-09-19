#!/usr/bin/env bash
set -eo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
source_dir="$HOME/android/a6l-lineage24"
exec > >(tee "$workspace/logs/build-fastboot-vendor.log") 2>&1
python3 "$workspace/tools/Prepare-FastbootVendorCommand.py" "$source_dir/system/core"
cd "$source_dir"
test -r out/combined-lineage_gsi_a6l.ninja
unset A6L_QUERY_PAD64 A6L_VENDOR_UNLOCK
prebuilts/build-tools/linux-x86/bin/ninja -f out/combined-lineage_gsi_a6l.ninja -j4 \
    out/host/linux-x86/bin/fastboot \
    out/host/linux-x86/nativetest64/fastboot_test/fastboot_test
out/host/linux-x86/nativetest64/fastboot_test/fastboot_test --gtest_filter='DriverTest.*'
git -C system/core diff -- fastboot/fastboot.cpp fastboot/fastboot_driver.cpp fastboot/fastboot_driver_test.cpp > "$workspace/logs/fastboot-vendor.patch"
sha256sum out/host/linux-x86/bin/fastboot
echo A6L_FASTBOOT_VENDOR_BUILD_AND_TEST_SUCCESS
