# SPDX-License-Identifier: Apache-2.0
# A6L rom-v1 (agent flash, 24 Sep 2026): installable-ROM vendor additions on top of the realinit vendor image.
# Inherited from lineage_gsi_a6l.mk. Prebuilt payloads (kernel modules, firmware, radio/e-ink daemons, Mesa) are staged
# into device/hisense/a6l/rom/prebuilt by tools/stage-rom-v1-prebuilts.sh; rom-files.mk (generated) lists them.
ROM_DIR := device/hisense/a6l/rom

PRODUCT_COPY_FILES += \
    $(ROM_DIR)/vendor-etc/fstab.qcom:$(TARGET_COPY_OUT_VENDOR)/etc/fstab.qcom \
    $(ROM_DIR)/vendor-etc/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.rc \
    $(ROM_DIR)/vendor-etc/mke2fs.a6l.conf:$(TARGET_COPY_OUT_VENDOR)/etc/mke2fs.a6l.conf \
    $(ROM_DIR)/init/init.qcom.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.qcom.rc \
    $(ROM_DIR)/init/init.a6l.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.a6l.usb.rc \
    $(ROM_DIR)/bin/a6l-modules.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-modules.sh \
    $(ROM_DIR)/bin/a6l-radio.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-radio.sh \
    $(ROM_DIR)/bin/a6l-logcat.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-logcat.sh \
    $(ROM_DIR)/init/init.a6l-rebootguard.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l-rebootguard.rc \
    $(ROM_DIR)/bin/a6l-reboot-guard.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-reboot-guard.sh

-include $(ROM_DIR)/rom-files.mk

# Graphics on the phone: msm DPU (card0) + Adreno 512 via Mesa freedreno (proven 21 Sep, V71 LineageOS-from-RAM run),
# selected at boot by a6l-modules.sh (persist.graphics.egl=mesa when renderD128 exists); ro.hardware.egl=angle remains
# the software fallback (and the QEMU test path).
# r6f: provisional screen-off workaround. Physical lock/wake failed with tiled
# freedreno rendering and passed with sysmem. Keep GPU shaders, bypass GMEM
# tiling until the underlying fault is fixed; this may increase memory traffic.
# r6g: recovery off-screen CopyTex tests fail on the a5xx 2D copy path.
# The generic GPU blit path passes all 338 format/copy cases. Android UI and
# stability validation are pending; this keeps FD512 shader rendering active.
PRODUCT_VENDOR_PROPERTIES += \
    debug.mesa.fd.mesa.debug=sysmem,noblit \
    debug.renderengine.backend=skiaglthreaded \
    debug.hwui.renderer=skiagl \
    debug.stagefright.c2inputsurface=-1 \
    ro.sf.lcd_density=400 \
    ro.surface_flinger.default_composition_pixel_format=5 \
    service.sf.prime_shader_cache=false \
    ro.vendor.a6l.rom=v2 \
    persist.vendor.a6l.radio=1 \
    persist.sys.usb.config=adb

# CameraX recording uses a persistent encoder surface. The software Codec2
# service has no native input-surface factory; select Android's built-in AIDL
# GraphicBufferSource, as in the platform goldfish product, instead of falling
# through to the absent legacy OMX HAL.

# GNSS (agent gnss, 24 Sep 2026): AIDL GNSS HAL over the modem's QMI LOC service + a6l_gnss_test (device/hisense/a6l/gnss,
# copied into the tree by tools/rom-v1-pipeline.sh). Idle until the radio is enabled (waits for the LOC service).
$(call inherit-product-if-exists, device/hisense/a6l/gnss/gnss.mk)

# ART heap sizes (agent gnss, 24 Sep 2026). Without a dalvik-heap config the GSI product runs every VM - system_server
# included - with the 16 MiB default growth limit: QEMU r6 system_server died with OutOfMemoryError ("target footprint
# 16777216, growth limit 16777216") ~16 min into the first boot.
# merge2 (25 Sep, Astra review S18): the A6L has **6 GiB** (stock /proc/meminfo MemTotal 6291456 kB,
# captures/stock-readonly-20260920/cpuinfo.txt; hardware-readiness-20260920.md), not 4 GiB -> 6144 profile
# (heapgrowthlimit 256m, heapsize 512m). Usable RAM after V75 carve-outs still to be read on the phone (MemTotal in bootinfo).
$(call inherit-product, frameworks/native/build/phone-xhdpi-6144-dalvik-heap.mk)
# 6 GiB memory profile: not a low-RAM device; lmkd in PSI mode (CONFIG_PSI=y in the V67 config); zram swap = 50 % of RAM,
# lz4 (CONFIG_ZRAM/ZSMALLOC/CRYPTO_LZ4=y), enabled after boot_completed by init.qcom.rc (swapon_all). Conservative values;
# retune after the first installed-system memory-pressure test.
PRODUCT_VENDOR_PROPERTIES += \
    ro.config.low_ram=false \
    ro.lmk.use_psi=true \
    ro.lmk.use_minfree_levels=false \
    ro.lmk.kill_heaviest_task=true \
    ro.lmk.swap_free_low_percentage=10 \
    ro.lmk.thrashing_limit=30 \
    ro.lmk.thrashing_limit_decay=50 \
    ro.lmk.psi_partial_stall_ms=70 \
    ro.lmk.psi_complete_stall_ms=700

# =====================================================================================================================
# rom-v2 integration (agent merge, 25 Sep 2026): everything proven or built on 23-25 Sep, merged into ONE product.
# Ledger: docs/rom-integration-ledger.md. Details: docs/merge-20260925.md. Kernel modules/firmware are staged by
# tools/stage-rom-v2-prebuilts.sh (rom-files.mk); the V75 DTB is built by tools/build-rom-v2-dt.sh.
# RF stays OFF by default (persist.vendor.a6l.radio=0: Pierre enables it); charger driver and IPA are loaded only when
# their property is set (attended C2 / ipa2b walk first).
# =====================================================================================================================
PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.a6l.rom.variant=v2 \
    persist.vendor.a6l.charger=1 \
    persist.vendor.a6l.ipa=1 \
    persist.vendor.a6l.camera=1 \
    persist.vendor.a6l.audio.q6routing=perdir \
    ro.vendor.a6l.audio.capture_pcm=1 \
    ro.vendor.a6l.rom.build=r7c \
    wifi.interface=wlan0

# merge r4 (28 Sep 2026; docs/merge-20260928.md): persist.vendor.a6l.ipa now defaults to 1 - ipa4 (data3) proven on the
# phone 27 Sep (data call, ping/DNS/HTTP) and the IPA driver MUST be loaded before the modem starts (a modem without the
# AP IPA asserts when IMS comes up; order enforced in rom/bin/a6l-radio.sh). RF itself stays off (persist.vendor.a6l.radio=0).

# --- charger policy (power 26 Sep / pwr27 27 Sep / merge r4): software JEITA + input-current guard for qcom_smbx, started
# by init only when the charger group is enabled (persist.vendor.a6l.charger=1). Stock limits: rom/modules/charger.txt.
PRODUCT_COPY_FILES += \
    device/hisense/a6l/power/rom/a6l-chg-guard.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-chg-guard.sh \
    device/hisense/a6l/power/rom/init.a6l-power.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l-power.rc

# --- Wi-Fi regulatory database (wifi 26 Sep): signed regulatory.db(.p7s) -> /vendor/firmware, so cfg80211 applies the
# FR country rules (else world domain 00). Same bytes as the attended v75/wifi run (sha 5560f4f0 / 5dd27969).
$(call inherit-product, device/hisense/a6l/wifi/wifi-regdb.mk)

# --- audio: AIDL example HAL (realinit) + a6l-audio-route (audio3, in-call paths by merge) + q6voiced (kvoice) ---------
# audio3's audio_policy_configuration.xml is copied over the realinit path by the pipeline (same destination).
# merge3 r3: capture = FE MultiMedia2 (pcmC0D1c) mono: mixer_paths top-level route MM2<-LPI_MI2S_TX_3, policy inputs MONO,
# HAL tree patch audio/patches/0001 (input pcm device = ro.vendor.a6l.audio.capture_pcm). Mic select/ADC gain: audio6 (r4).
PRODUCT_PACKAGES += \
    a6l-audio-route \
    mixer_paths_a6l.xml \
    a6l-q6voiced
DEVICE_PACKAGE_OVERLAYS += device/hisense/a6l/audio/overlay

# --- telephony: radio HAL (ril + misc SMS bare-TPDU/DTMF, voice.active glue by merge) + a6l-qmi ---------------------
$(call inherit-product, device/hisense/a6l/radio/radio.mk)

# --- Wi-Fi / Bluetooth / lights / power / thermal (hals subset, as the rom-v1 "full" variant) -------------------------
PRODUCT_PACKAGES += \
    android.hardware.wifi-service \
    wpa_supplicant \
    hostapd \
    wificond \
    iw \
    android.hardware.bluetooth-service.default \
    android.hardware.light-service.lineage \
    android.hardware.power-service.example
$(call soong_config_set,wpa_supplicant,platform_version,$(PLATFORM_VERSION))
# r5 review fix F5: vendor Wi-Fi HAL + plain nl80211 supplicant (no QCA vendor commands), wifi/wifi-hal.mk
$(call inherit-product, device/hisense/a6l/wifi/wifi-hal.mk)
PRODUCT_COPY_FILES += \
    $(ROM_DIR)/v2/init.a6l.wifibt.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.wifibt.rc \
    device/hisense/a6l/hals/wifi/wpa_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant.conf \
    device/hisense/a6l/hals/wifi/wpa_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/wpa_supplicant_overlay.conf \
    device/hisense/a6l/hals/wifi/p2p_supplicant.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant.conf \
    device/hisense/a6l/hals/wifi/p2p_supplicant_overlay.conf:$(TARGET_COPY_OUT_VENDOR)/etc/wifi/p2p_supplicant_overlay.conf \
    frameworks/native/data/etc/android.hardware.wifi.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.wifi.xml \
    frameworks/native/data/etc/android.hardware.bluetooth.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth.xml \
    frameworks/native/data/etc/android.hardware.bluetooth_le.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.bluetooth_le.xml
# bt-audio (29 Sep 2026): Bluetooth profiles (none start without bluetooth.profile.*), software A2DP + HCI SCO (HFP
# software data path, audio-managed SCO), LE audio off. SCO ports: audio/audio_policy_configuration.xml bluetooth module.
$(call inherit-product, device/hisense/a6l/audio/bluetooth/bt-audio.mk)

# --- sensors: multihal + sensors.a6l (LineageOS sensors@2.0-subhal-impl-1.0) --------------------------------------------
# senshal (26 Sep): the trout IIO sub-HAL is REMOVED (it only accepts scmi.iio.* names and reads 64-bit channels: it
# listed 0 SMGR sensors). sensors.a6l now serves accelerometer, gyroscope, magnetometer (+ uncalibrated) from the SMGR
# IIO devices with hard-iron calibration, plus the FRONT STK3338 light + WAKE-UP proximity (merge2, proven attended stk
# T2, 0x67). The rear TMD3702 is deliberately not exposed as TYPE_PROXIMITY (docs/stk-20260925.md 4).
# docs/senshal-20260926.md. NOT declared: step counter/detector, significant motion, barometer.
PRODUCT_PACKAGES += \
    android.hardware.sensors-service.multihal
PRODUCT_COPY_FILES += \
    $(ROM_DIR)/v2/sensors/hals.conf:$(TARGET_COPY_OUT_VENDOR)/etc/sensors/hals.conf \
    frameworks/native/data/etc/android.hardware.sensor.accelerometer.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.accelerometer.xml \
    frameworks/native/data/etc/android.hardware.sensor.gyroscope.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.gyroscope.xml \
    frameworks/native/data/etc/android.hardware.sensor.compass.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.compass.xml \
    frameworks/native/data/etc/android.hardware.sensor.light.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.light.xml \
    frameworks/native/data/etc/android.hardware.sensor.proximity.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.sensor.proximity.xml
$(call inherit-product, device/hisense/a6l/hals/sensors/stk3338/stk3338.mk)
# handheld_core_hardware.xml still declares a camera: withdrawn with <unavailable-feature> (see the XML; the compass is
# served by sensors.a6l since senshal, 26 Sep)
PRODUCT_COPY_FILES += \
    $(ROM_DIR)/vendor-etc/a6l-unavailable-features.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/a6l-unavailable-features.xml

# --- vibrator: QTI VibratorOL, LED backend /sys/class/leds/vibrator of a6l_gpio_vib (TLMM GPIO79; r5 review fix F49) ------
$(call inherit-product, vendor/qcom/opensource/vibrator/vibrator-vendor-product.mk)

# --- rear e-ink: eink3 (a6l_epdd v4 + mirror v2, composer lease patch 0001) + dualux (daemon, app, composer 0002) -----
$(call inherit-product, device/hisense/a6l/eink/eink.mk)
$(call inherit-product, device/hisense/a6l/eink/switcher/dualux.mk)

# --- graphics capability claims (Astra H3): Adreno 512 is a5xx; Mesa Turnip does not support a5xx, so there is NO hardware
# Vulkan and no Turnip in this product. GLES = Mesa freedreno (persist.graphics.egl=mesa, set by a6l-modules.sh). The only
# Vulkan driver is the software vulkan.pastel (a6l-software-graphics.mk); no android.hardware.vulkan.* feature XML is
# installed by this product, so apps are not told a hardware Vulkan level exists. HWUI/RenderEngine stay on skiagl.

# --- Google apps (Pierre: required): MindTheGapps for Android 17 ("cinnamonbun"), linked to vendor/gapps by the pipeline
# MTG's generated mk adds $(LOCAL_PATH) as the soong namespace, which is not its own dir when inherited from here:
# name both namespaces explicitly (arm64 + common).
# overlays-carrier-updater (29 Sep 2026): A6L_NO_GAPPS=1 in the build environment builds a vanilla (redistributable) image
# even when the clone is in the tree. Licensing/plan: docs/android-overlays-carrier-updater-20260929.md.
ifneq ($(A6L_NO_GAPPS),1)
ifneq ($(wildcard vendor/gapps/arm64/arm64-vendor.mk),)
PRODUCT_SOONG_NAMESPACES += vendor/gapps/arm64 vendor/gapps/common
$(call inherit-product, vendor/gapps/arm64/arm64-vendor.mk)
endif
endif

# --- USB device mode (android-usb, 29 Sep 2026; docs/android-usb-20260929.md): gadget AIDL HAL (MTP/PTP/ADB/charging-only/
# RNDIS+NCM tethering/MIDI when usb_f_midi.ko exists) on the configfs gadget of rom/init/init.a6l.usb.rc (sys.usb.configfs=2),
# UDC-sysfs USB state overlay. Replaces the excluded hals/usb-gadget stub.
$(call inherit-product, device/hisense/a6l/usb/usb.mk)

# --- camera (lc2, 29 Sep 2026; docs/libcamera-plan-20260929.md section 10): libcamera v0.7.2 HAL + A6L AIDL provider fork
# (torch strength levels), camera features (no autofocus), media profiles. persist.vendor.a6l.camera now defaults to 1
# (above): the camera stack (qcom-camss rom1, a6l_wm=3) loads with the misc module group and the provider starts after it.
# Set it to 0 to boot without the camera stack (the features stay declared; the provider then lists no camera).
$(call inherit-product, device/hisense/a6l/camera/camera.mk)

# r6 (completeness audit 29 Sep 2026; docs/completeness-audit-20260929.md)
$(call inherit-product, device/hisense/a6l/rom/r6/r6.mk)

# android-side product config (overlays-carrier-updater, 29 Sep 2026; docs/android-overlays-carrier-updater-20260929.md):
# framework overlay, Wi-Fi/CarrierConfig/SystemUI/Settings RROs, Orange France APN fix-up, Updater placeholder URI
$(call inherit-product, device/hisense/a6l/rom/android/android.mk)

# selinux-release (29 Sep 2026; docs/selinux-release-20260929.md): off-mode charger UI (health HAL charger mode + Lineage images)
# and the SELinux enforcing-prep product bits (inert unless A6L_SELINUX_PREP=1 or A6L_RELEASE=1)
$(call inherit-product, device/hisense/a6l/rom/charger/charger.mk)
$(call inherit-product, device/hisense/a6l/rom/selinux/selinux.mk)

# r6b boot fix (30 Sep 2026; docs/rom-r6b-bootfix-20260930.md): userdebug/eng-only persistent boot log
# (/metadata/a6l/boot-kmsg.txt, a6l_bootlog service); a user build installs nothing from it
$(call inherit-product, device/hisense/a6l/rom/debug/bootlog.mk)
