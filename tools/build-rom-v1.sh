set +e
exec < /dev/null
R=/mnt/c/Users/Pierre/Desktop/A6L; OB=$R/.relay/outbox/flash-10-build-progress.log
pgrep -f "soong_ui|ninja" > /dev/null && { echo "ANOTHER BUILD IS RUNNING - refusing"; ps aux | grep -E "soong_ui|ninja" | grep -v grep | cut -c1-200 | head; exit 1; }
nohup bash -c '
cd /home/a6l/android/a6l-lineage24
export A6L_SOONG_GOMEMLIMIT=36GiB
source build/envsetup.sh > /dev/null
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug > /dev/null 2>&1
date; echo BUILD_START
m -j12 systemimage vendorimage > /home/a6l/rom-v1/build.log 2>&1; rc=$?
date; echo "BUILD_RC=$rc"
grep -E "error:|FAILED:|ninja: build stopped" /home/a6l/rom-v1/build.log | head -40
tail -5 /home/a6l/rom-v1/build.log
ls -la out/target/product/a6l/*.img
echo BUILD_DONE' > $OB 2>&1 < /dev/null &
echo launched
