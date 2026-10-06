# Hisense A6L radio HAL (agent ril, 24 Sep 2026): inherit from lineage_gsi_a6l.mk (flash agent):
#   $(call inherit-product, device/hisense/a6l/radio/radio.mk)
# and in BoardConfig.mk:  -include device/hisense/a6l/radio/BoardConfig-radio.mk
PRODUCT_PACKAGES += \
    android.hardware.radio-service.a6l \
    a6l-qmi \
    a6l-imsdcm

# Telephony features (GSM/UMTS/LTE, dual SIM dual standby like stock: ril3 25 Sep 2026). Only valid together with the radio HAL above.
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
    ro.vendor.a6l.ril.dpm_open_port=1 \
    ro.vendor.a6l.ril.wda_agg=5 \
    ro.telephony.default_network=9,9 \
    ro.telephony.sim_slots.count=2 \
    ro.vendor.a6l.ril.slots=2 \
    persist.radio.multisim.config=dsds \
    telephony.lteOnCdmaDevice=0
# ril3 (25 Sep 2026): DSDS. Stock ran persist.radio.multisim.config=dsds; the HAL publishes slot1+slot2
# (IRadioConfig: 2 logical modems, 1 active data). To force single SIM set ro.vendor.a6l.ril.slots=1
# and persist.radio.multisim.config=ssss (Android then creates one phone; the HAL publishes slot1 only).
# Android's UiccController must also know there are two physical slots. New
# vendor API levels no longer enlarge the default one-slot array to fit DSDS;
# without this explicit count, a slot2 card-status response crashes Phone.
# persist.vendor.a6l.ril.auto_provision=false stops the HAL from activating a USIM in slot 2 itself.
# volte2 (25 Sep 2026): modem-centric VoLTE. a6l-imsdcm (QMI IMSDCM 770 server, init/a6l-imsdcm.rc) starts only
# with persist.vendor.a6l.ril.volte=1 (and the modem enabled). Off until the attended IMS registration test passes.
# volte5 (28 Sep 2026): a6l-imsdcm --imsa also binds IMSA/IMSS to the subscription and switches IMS on (IMSS
# ims_service_enabled=1 volte=1, what stock ImsService/qcril do at boot) when imsdcm_imss=enable.
# volte6 (29 Sep 2026): imsdcm_kick=force re-asserts IMS (IMSS set + IMSA re-bind) when LTE reaches full service and
# after a modem 0x33 SUB_DESTROY_INSTANCE, like stock ImsService does after every SIM load. imsdcm_vdp stays read
# until the attended test shows whether NAS voice_domain_pref needs the stock "VoLTE on" value 3.
PRODUCT_VENDOR_PROPERTIES += \
    persist.vendor.a6l.ril.volte=0 \
    persist.vendor.a6l.ril.imsdcm_imss=enable \
    persist.vendor.a6l.ril.imsdcm_kick=force
