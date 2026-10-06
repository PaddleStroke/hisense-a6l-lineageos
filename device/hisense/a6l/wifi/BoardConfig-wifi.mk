# Hisense A6L Wi-Fi board flags (r5 review fix F5, agent wifi-hal 29 Sep 2026; docs/wifi-hal-20260929.md).
# Included by rom/BoardConfig-rom.mk (and hals/BoardConfig-hals.mk, rom/full/BoardConfig-full.mk).
#
# Chip: WCN3990 on mainline ath10k_snoc (cfg80211/mac80211, nl80211). No Qualcomm proprietary WLAN stack.
# BOARD_WLAN_DEVICE stays UNSET on purpose: soong then links libwifi-hal-fallback into android.hardware.wifi-service,
# its init_wifi_vendor_hal_func_table() returns NOT_SUPPORTED, and WifiLegacyHalFactory loads our libwifi-hal-a6l
# through /vendor/etc/wifi/vendor_hals/a6l_wifi_vendor_hal.xml (wifi/wifi-hal.mk). Every BOARD_WLAN_DEVICE value known
# to frameworks/opt/net/wifi/libwifi_hal selects a vendor-specific HAL (bcmdhd, qcwcn = QCA vendor nl80211 commands of
# the proprietary qcacld driver, emulator = goldfish hwsim HAL with fake APF/keep-alive): none fits ath10k.
BOARD_WPA_SUPPLICANT_DRIVER := NL80211
BOARD_HOSTAPD_DRIVER        := NL80211
WPA_SUPPLICANT_VERSION      := VER_0_8_X
# COUNTRY/MACADDR driver commands over plain nl80211 (without it every DRIVER command fails: stub build).
# hostapd keeps the stub build (it gets the country from its own config).
BOARD_WPA_SUPPLICANT_PRIVATE_LIB := lib_driver_cmd_a6l
# Interface combinations reported by the AIDL HAL. Only what is going to be validated first: one STA, or one AP on
# wlan0 (hostapd switches the iftype), never both at once. The kernel's ath10k TLV combination allows up to
# {2 STA, 2 AP/P2P-client/GO, 1 P2P-device} on ONE channel (4 ifaces), but concurrency needs virtual interface creation,
# which libwifi-hal-a6l does not implement, and has never run on this firmware: widen only after `iw phy` + attended
# tests (docs/wifi-hal-20260929.md 5). P2P (android.hardware.wifi.direct) is not declared by the product either.
# Replaces WIFI_HIDL_FEATURE_DUAL_INTERFACE := true (= STA+AP or STA+P2P concurrency, unproven).
WIFI_HAL_INTERFACE_COMBINATIONS := {{{STA}, 1}}, {{{AP}, 1}}
