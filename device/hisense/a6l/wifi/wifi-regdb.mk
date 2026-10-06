# wifi agent (26 Sep 2026): signed wireless regulatory database for cfg80211 (kernel CONFIG_CFG80211_REQUIRE_SIGNED_REGDB=y
# + USE_KERNEL_REGDB_KEYS=y). The ROM sets firmware_class.path=/vendor/firmware (rom/init/init.qcom.rc), so the files go
# there. Without them cfg80211 stays in world domain "00" (country FR ignored: no-IR on ch 12/13 and most 5 GHz channels).
# Same bytes as v75/wifi/firmware (sha 5560f4f0... / 5dd27969...). For the flash agent: inherit from the ROM product .mk.
PRODUCT_COPY_FILES += \
    external/wireless-regdb/regulatory.db:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db \
    external/wireless-regdb/regulatory.db.p7s:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db.p7s
