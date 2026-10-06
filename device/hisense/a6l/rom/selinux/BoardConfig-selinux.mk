# SPDX-License-Identifier: Apache-2.0
# A6L SELinux enforcing readiness (selinux-release, 29 Sep 2026; docs/selinux-release-20260929.md). Included from
# rom/BoardConfig-rom.mk. Opt-in: A6L_SELINUX_PREP=1 (or A6L_RELEASE=1) in the build environment. Off = r6 as before.
# The flag does NOT make the ROM enforcing: the boot cmdline keeps androidboot.selinux=permissive unless the pipeline's
# boot phase runs with A6L_SELINUX=enforcing (tools/Prepare-RomV2Boot.py); a `user` build is always enforcing.
# With the flag: rom/sepolicy/vendor (BoardConfig-rom.mk) + this dir's policy, and the tree's rc/ueventd files rewritten by
# tools/release/a6l_selinux_prep.py apply (pipeline prep phase, same flag) -> no vendor_modprobe seclabels, debug logcat
# service only in userdebug/eng, /dev/dri/card* 0660.
ifneq ($(filter 1,$(A6L_SELINUX_PREP) $(A6L_RELEASE)),)
ifeq ($(wildcard device/hisense/a6l/rom/selinux/.prep-applied),)
$(error A6L_SELINUX_PREP=1 / A6L_RELEASE=1 but tools/release/a6l_selinux_prep.py was not applied to this tree: run tools/rom-v2-pipeline.sh <tag> prep with the same environment)
endif
BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/rom/selinux/sepolicy/vendor
endif
