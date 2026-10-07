# SPDX-License-Identifier: Apache-2.0
# venus-impl (6 Oct 2026; firmware/extracted/venus-impl-20261006/README.md): hardware H.264 encoding on the SDM660 Venus
# (upstream venus V4L2 stateful encoder) through the ChromeOS/AOSP V4L2 Codec2 HAL: external/v4l2_codec2 (in the Lineage
# tree, rev 6685af1c) + A6L patches rom/android/patches/external/v4l2_codec2/0001-0003 (find the encoder by driver name via
# sysfs, never open camera nodes, any /dev/videoN; advertise the encoder only when the device exists; MMAP coded output).
# Kernel side: boot DTB with &venus okay + per-Venus CX vote (boot-venus-cx.img), rom/modules/video.txt loaded by
# a6l-modules.sh misc when persist.vendor.a6l.venus=1, firmware /vendor/firmware/qcom/venus-4.4 (stage-rom-v2-prebuilts.sh).
# Stage B PASSED 7 Oct 2026 (hfi3f: 1080p 43.8 fps max, 29.85 fps paced, EOS ok, High profile, ffprobe-clean): default ON.
# Round 13c (7 Oct 2026): default 1 again - prod3 cold power cycle passed stage-b9b (5 cold resumes ~100 ms, sleep A/B).
# prod3 (stagec/modules, 8 Oct 2026): fixes that SYS_INIT -110 (stale CPU_CS_SCIACMDARG0 from the previous image made the
# boot wait end before the new firmware was up; cleared at cold suspend/resume) + balanced venc runtime PM. Set this back
# to 1 once stage-b9b-pc.sh and stage-b9b-sleep.sh pass with prod3.
# Off switch: setprop persist.vendor.a6l.venus 0 + reboot -> no modules -> no node -> the V4L2 store advertises nothing ->
# MediaCodecList keeps c2.android.avc.encoder (software), exactly as before. Venus modules = venus-impl stagec (hfi3 prod:
# SDM660 HFI3 caps fill, 133 MHz init clock, no TRANSFORM_8X8 on HFI3, firmware buffer mode, real EOS buffer) + prod2 COLD
# power cycle: firmware shut down when the last session closes / at system suspend, full reboot (PAS auth + SYS_INIT) at the
# next open (warm HFI3 resume leaves the firmware deaf, out8); venus_core.a6l_keep_on=1 (insmod param) = always powered.
# output_mmap: the HFI3 firmware uses static output buffers -> coded output through fixed MMAP buffers + copy (patch 0003).
# HEVC encode: the firmware supports it, but external/v4l2_codec2 has no HEVC encoder component -> H.264 only.
# Decoder: venus-dec.ko is staged (persist.vendor.a6l.venus.dec=1 loads it) but untested; no Codec2 decoder is enabled.
# Inherited from rom/rom.mk; sepolicy via BoardConfig-venus.mk.
A6L_VENUS_DIR := device/hisense/a6l/video/venus

PRODUCT_SOONG_NAMESPACES += external/v4l2_codec2
# The service's required: pulls android.hardware.media.c2-default-seccomp_policy; its init rc (class hal, user media,
# group mediadrm drmrpc) and the AIDL IComponentStore/default VINTF fragment come from the upstream module.
PRODUCT_PACKAGES += \
    android.hardware.media.c2-service-v4l2 \
    libc2plugin_store

PRODUCT_VENDOR_PROPERTIES += \
    ro.vendor.v4l2_codec2.encoder.supported.h264=true \
    ro.vendor.v4l2_codec2.device_name_filter=venus \
    ro.vendor.v4l2_codec2.encoder.output_mmap=true \
    persist.vendor.a6l.venus=1 \
    persist.vendor.a6l.venus.dec=0

PRODUCT_COPY_FILES += \
    $(A6L_VENUS_DIR)/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    $(A6L_VENUS_DIR)/android.hardware.media.c2-extended-seccomp_policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/android.hardware.media.c2-extended-seccomp_policy \
    $(A6L_VENUS_DIR)/init.a6l.venus.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.venus.rc
