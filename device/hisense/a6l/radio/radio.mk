# Hisense A6L radio HAL (agent ril, 24 Sep 2026): inherit from lineage_gsi_a6l.mk (flash agent):
#   $(call inherit-product, device/hisense/a6l/radio/radio.mk)
# and in BoardConfig.mk:  -include device/hisense/a6l/radio/BoardConfig-radio.mk
PRODUCT_PACKAGES += \
    android.hardware.radio-service.a6l \
    a6l-qmi

# Telephony features (GSM/UMTS/LTE, single SIM). Only valid together with the radio HAL above.
# Android 14+ telephony sub-features (TelephonyManager APIs check them on newer vendor API levels).
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.telephony.gsm.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.gsm.xml \
    frameworks/native/data/etc/android.hardware.telephony.radio.access.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.radio.access.xml \
    frameworks/native/data/etc/android.hardware.telephony.subscription.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.subscription.xml \
    frameworks/native/data/etc/android.hardware.telephony.calling.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.calling.xml \
    frameworks/native/data/etc/android.hardware.telephony.messaging.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.messaging.xml \
    frameworks/native/data/etc/android.hardware.telephony.data.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.telephony.data.xml

PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.a6l.ril.data_parent=rmnet_ipa0 \
    ro.vendor.a6l.ril.rmnet_flags=1 \
    ro.vendor.a6l.ril.ep_type=4 \
    ro.vendor.a6l.ril.ep_iface=1 \
    ro.telephony.default_network=9 \
    telephony.lteOnCdmaDevice=0
