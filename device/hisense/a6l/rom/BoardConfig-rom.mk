# A6L rom-v2 board flags (agent merge, 25 Sep 2026). Included from BoardConfig.mk.
# Wi-Fi: nl80211 supplicant/hostapd, vendor HAL libwifi-hal-a6l, STA-or-AP combinations (r5 review fix F5)
include device/hisense/a6l/wifi/BoardConfig-wifi.mk
# Per-area vendor/system_ext sepolicy (the ROM still boots PERMISSIVE: androidboot.selinux=permissive; these give the
# daemons their domains/labels so the avc log is readable and enforcing can be worked on). A dir that does not compile
# is listed in docs/merge-20260925.md and left out here.
A6L_SEPOLICY_DIRS ?= radio/sepolicy audio/sepolicy kvoice/q6voiced/sepolicy gnss/sepolicy/vendor eink/sepolicy/vendor eink/switcher/sepolicy/vendor usb/sepolicy/vendor
A6L_SEPOLICY_DIRS += rom/r6/sepolicy/vendor
# lc2 (29 Sep 2026): camera provider + libcamera (hal_camera_default, /dev/media* /dev/v4l-subdev* labels)
A6L_SEPOLICY_DIRS += camera/sepolicy/vendor
# release-prep (27 Sep 2026): ROM-level policy (scripts, radio companions, unlabeled HALs, eMMC partition labels, a6l props)
# for enforcing. Opt-in: A6L_SEPOLICY_ROM=1 (userdebug test of the policy while still permissive) or A6L_RELEASE=1.
# Checked offline: tools/release/check-a6l-sepolicy.sh {userdebug|user} rom/sepolicy/vendor -> A6L_SEPOLICY_CHECK PASS.
# selinux-release (29 Sep 2026): A6L_SELINUX_PREP=1 (rc transform + enforcing-prep policy, rom/selinux) implies it too.
ifneq ($(filter 1,$(A6L_SEPOLICY_ROM) $(A6L_RELEASE) $(A6L_SELINUX_PREP)),)
A6L_SEPOLICY_DIRS += rom/sepolicy/vendor
endif
BOARD_VENDOR_SEPOLICY_DIRS += $(addprefix device/hisense/a6l/,$(A6L_SEPOLICY_DIRS))
ifneq ($(filter eink/switcher/sepolicy/vendor,$(A6L_SEPOLICY_DIRS)),)
SYSTEM_EXT_PUBLIC_SEPOLICY_DIRS += device/hisense/a6l/eink/switcher/sepolicy/system_ext/public
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/hisense/a6l/eink/switcher/sepolicy/system_ext/private
endif
# Google apps (MindTheGapps board flags, if present)
-include vendor/gapps/arm64/BoardConfigVendor.mk
# selinux-release (29 Sep 2026; docs/selinux-release-20260929.md): enforcing-prep policy dir + tree check (opt-in A6L_SELINUX_PREP=1),
# charger UI image density
include device/hisense/a6l/rom/selinux/BoardConfig-selinux.mk
include device/hisense/a6l/rom/charger/BoardConfig-charger.mk
# r6e (1 Oct 2026, docs/rom-r6e-20261001.md): executable modes for /vendor/a6l/radio/bin (were 0644 -> radio daemons never ran)
TARGET_FS_CONFIG_GEN += device/hisense/a6l/rom/config.fs

# Bounded debug-only AVC stage measurements.
SYSTEM_EXT_PRIVATE_SEPOLICY_DIRS += device/hisense/a6l/rom/media/sepolicy/system_ext/private
