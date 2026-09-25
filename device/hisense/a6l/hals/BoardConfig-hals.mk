# Hisense A6L HAL layer board flags (agent hals). include from BoardConfig.mk:
#   include device/hisense/a6l/hals/BoardConfig-hals.mk
# Wi-Fi: mainline ath10k via plain nl80211; no QCA vendor HAL. BOARD_WLAN_DEVICE left UNSET on purpose =>
# libwifi-hal-fallback (see docs/hals-20260924.md section Wi-Fi for the fallback caveat and the plan B).
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_HOSTAPD_DRIVER        := NL80211
WPA_SUPPLICANT_VERSION      := VER_0_8_X
WIFI_HIDL_FEATURE_DUAL_INTERFACE := true
BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/hals/sepolicy
