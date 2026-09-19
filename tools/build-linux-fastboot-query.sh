#!/usr/bin/env bash
set -eo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
source_dir="$HOME/android/a6l-lineage24"
mkdir -p "$HOME/logs"
exec > >(tee "$HOME/logs/build-fastboot-query.log" "$workspace/logs/build-fastboot-query.log") 2>&1
python3 "$workspace/tools/Prepare-FastbootQueryPadding.py" "$source_dir/system/core"
cd "$source_dir"
export A6L_SOONG_GOMEMLIMIT=36GiB
if [[ "${1:-}" == --resume-ninja ]]; then
    # Reuse the already generated graph for source-only fixes after a compile failure.
    test -r out/combined-lineage_gsi_a6l.ninja
    prebuilts/build-tools/linux-x86/bin/ninja -f out/combined-lineage_gsi_a6l.ninja -j4 \
        out/host/linux-x86/bin/fastboot \
        out/host/linux-x86/nativetest64/fastboot_test/fastboot_test
else
    source build/envsetup.sh
    source vendor/lineage/vars/aosp_target_release
    lunch lineage_gsi_a6l "$aosp_target_release" userdebug
    m -j4 out/host/linux-x86/bin/fastboot out/host/linux-x86/nativetest64/fastboot_test/fastboot_test
fi
unset A6L_QUERY_PAD64
out/host/linux-x86/nativetest64/fastboot_test/fastboot_test --gtest_filter='DriverTest.*'
git -C system/core diff -- fastboot/fastboot_driver.cpp fastboot/fastboot_driver_test.cpp > "$workspace/logs/fastboot-query-padding.patch"
sha256sum out/host/linux-x86/bin/fastboot
echo A6L_FASTBOOT_QUERY_BUILD_AND_TEST_SUCCESS
