# SPDX-License-Identifier: Apache-2.0
# Hisense A6L USB device mode (android-usb, 29 Sep 2026; docs/android-usb-20260929.md). Inherited from rom/rom.mk.
# Gadget skeleton + FunctionFS mounts: rom/init/init.a6l.usb.rc (sys.usb.configfs=2); function switching (MTP, PTP,
# ADB, charging-only, RNDIS/NCM tethering, MIDI when usb_f_midi.ko exists): android.hardware.usb.gadget-service.a6l.
# NOT provided: IUsb (android.hardware.usb) port HAL - the port has no Type-C class device on mainline (usb_nop_phy,
# USB2 only); UsbPortManager runs without it (no role-swap UI). No android.hardware.usb.accessory feature (no AOA).
PRODUCT_SOONG_NAMESPACES += device/hisense/a6l/usb
PRODUCT_PACKAGES += \
    android.hardware.usb.gadget-service.a6l
DEVICE_PACKAGE_OVERLAYS += device/hisense/a6l/usb/overlay
