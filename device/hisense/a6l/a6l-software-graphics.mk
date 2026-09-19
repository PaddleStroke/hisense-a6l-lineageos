# SPDX-License-Identifier: Apache-2.0
# Staged bring-up configuration for a future native A6L product.
# Not inherited by the current GSI compile probe: service/runtime integration
# and the software renderer still need validation before system installation.
# Requires the recorded minigbm simpleDRM backend patch and a simpleDRM device.

PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm \
    android.hardware.composer.hwc3-service.drm \
    vulkan.pastel \
    libEGL_angle \
    libGLESv1_CM_angle \
    libGLESv2_angle

PRODUCT_VENDOR_PROPERTIES += \
    ro.hardware.gralloc=minigbm \
    ro.hardware.egl=angle \
    ro.hardware.vulkan=pastel
