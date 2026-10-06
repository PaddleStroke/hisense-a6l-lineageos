# stk agent (25 Sep 2026): FRONT STK3338 light + WAKE-UP proximity for the sensors multihal.
# senshal agent (26 Sep 2026): sensors.a6l also serves accelerometer, gyroscope, magnetometer (+ uncalibrated) from the
# ADSP SMGR IIO devices (qcom-smgr-accel/gyro/mag) with magnetometer hard-iron calibration; it is the ONLY sub-HAL
# (the trout IIO sub-HAL cannot read SMGR). Properties: persist.vendor.a6l.sensors.map[.accel|.gyro|.mag] (default
# +y+x-z), .mag_unit (auto|gauss|ut), .magcal (1), .tick_hz (32768), .motion (1; 0 hides accel/gyro/mag).
# Inherit from the ROM product (full variant, next to hals.mk):
#   $(call inherit-product, device/hisense/a6l/hals/sensors/stk3338/stk3338.mk)
# Needs: kernel module stk3338_a6l.ko loaded at boot (a6l-modules.sh adsp group) + DT a6l-stk3338-v75.dtso merged,
# hals.conf listing android.hardware.sensors@2.0-subhal-impl-1.0.so (added 25 Sep).
PRODUCT_SOONG_NAMESPACES += device/hisense/a6l/hals/sensors/stk3338
PRODUCT_PACKAGES += \
    sensors.a6l \
    android.hardware.sensors@2.0-subhal-impl-1.0
# hw_get_module("sensors") looks for sensors.<ro.hardware.sensors>.so first
PRODUCT_VENDOR_PROPERTIES += ro.hardware.sensors=a6l
# /data/vendor/sensors for the magnetometer calibration
PRODUCT_COPY_FILES += \
    device/hisense/a6l/hals/sensors/stk3338/a6l-sensors-hal.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/a6l-sensors-hal.rc
