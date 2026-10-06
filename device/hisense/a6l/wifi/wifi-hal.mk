# Hisense A6L Wi-Fi vendor HAL (r5 review fix F5, agent wifi-hal 29 Sep 2026; docs/wifi-hal-20260929.md).
# Inherited by rom/rom.mk (and hals/hals.mk, rom/full/full.mk). Board flags: wifi/BoardConfig-wifi.mk.
# libwifi-hal-a6l: nl80211 legacy HAL dlopen()ed by android.hardware.wifi-service via
# /vendor/etc/wifi/vendor_hals/a6l_wifi_vendor_hal.xml (installed as its `required`).
PRODUCT_PACKAGES += \
    libwifi-hal-a6l \
    a6l_wifi_vendor_hal.xml
# wpa_supplicant (and lib_driver_cmd_a6l, same cflags) are built for plain nl80211: no
# `soong_config_set,wpa_supplicant,nl80211_driver,CONFIG_DRIVER_NL80211_QCA` any more. That flag compiles in the QCA
# vendor-command paths of the proprietary qcacld driver, which mainline ath10k does not implement; the attended
# recovery WPA2/DHCP test (docs/wifi-20260926.md) used a plain-nl80211 supplicant too.
