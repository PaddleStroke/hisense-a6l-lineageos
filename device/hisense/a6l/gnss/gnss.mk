# A6L GNSS (agent gnss, 24 Sep 2026): inherit from lineage_gsi_a6l.mk (or rom/variant) to ship the QMI LOC GNSS HAL.
PRODUCT_PACKAGES += \
    android.hardware.gnss-service.a6l \
    a6l_timekeep   # r5 bug hunt round2 R1: wall clock across reboots (read-only PM660 RTC)
# user-build-20261012: a6l_gnss_test is a root CLI test tool (QMI LOC record/replay), userdebug/eng only
ifneq ($(TARGET_BUILD_VARIANT),user)
PRODUCT_PACKAGES += \
    a6l_gnss_test
endif

# Tells apps/Play services that the device has a GPS receiver (LocationManager uses the HAL either way).
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.location.gps.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.location.gps.xml \
    device/hisense/a6l/gnss/psds/gps_debug.conf:$(TARGET_COPY_OUT_VENDOR)/etc/gps_debug.conf

PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.a6l.gnss.mode=standalone \
    persist.vendor.a6l.gnss.unlock_engine=false \
    persist.vendor.a6l.gnss.xtra=true

# Enforcing builds: BOARD_VENDOR_SEPOLICY_DIRS += device/hisense/a6l/gnss/sepolicy/vendor
