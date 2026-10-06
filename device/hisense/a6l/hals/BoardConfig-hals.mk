# Hisense A6L HAL layer board flags (agent hals). include from BoardConfig.mk:
#   include device/hisense/a6l/hals/BoardConfig-hals.mk
# Wi-Fi: mainline ath10k via plain nl80211; no QCA vendor HAL. BOARD_WLAN_DEVICE left UNSET on purpose (linked
# fallback) and libwifi-hal-a6l loaded dynamically: r5 review fix F5, docs/wifi-hal-20260929.md.
include device/hisense/a6l/wifi/BoardConfig-wifi.mk
BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/hals/sepolicy
