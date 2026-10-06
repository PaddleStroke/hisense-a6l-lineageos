# SPDX-License-Identifier: Apache-2.0
# Graphics HALs of the A6L product (inherited by lineage_gsi_a6l.mk since realinit2, 23 Sep 2026): minigbm allocator +
# stable-C mapper, drm_hwcomposer HWC3, ANGLE/SwiftShader software fallback (Mesa freedreno EGL is selected at boot by
# rom/bin/a6l-modules.sh). minigbm carries the recorded simpleDRM backend patch (QEMU / LCD simplefb fallback) and, since
# r6c, the msm backend (below).

PRODUCT_PACKAGES += \
    android.hardware.graphics.allocator-service.minigbm \
    mapper.minigbm \
    gralloc.minigbm \
    android.hardware.composer.hwc3-service.drm \
    vulkan.pastel \
    libEGL_angle \
    libGLESv1_CM_angle \
    libGLESv2_angle

# r6c (30 Sep 2026, docs/rom-r6c-20260930.md): the allocator must also serve the msm GPU. On the phone the only DRM
# devices are renderD128/card0 = "msm" (Adreno 512, msm.ko separate_gpu_kms=1) and card1 = "msm-kms" (DPU); the generic
# libminigbm_gralloc has no backend for either name (drv.c: the msm backend is compiled only with -DDRV_MSM), so
# android.hardware.graphics.allocator-service.minigbm exited 1 ("Failed to initialize driver") at every start in r6/r6b
# and surfaceflinger/zygote were SIGKILLed in a loop (its onrestart). V71 (21 Sep) only worked because a6l_simplefb gave
# it a simpledrm card. external/minigbm/Android.bp: minigbm_defaults selects msm_cflags (-DDRV_MSM
# -DHAS_DMABUF_SYSTEM_HEAP -DQCOM_DISABLE_COMPRESSED_NV12) for EVERY minigbm library (the allocator service AND
# mapper.minigbm link libminigbm_gralloc) when soong_config minigbm.platform=msm. The backend list keeps every dumb
# backend (simpledrm, virtio_gpu, vkms...), so QEMU and the LCD simplefb fallback are unchanged; the render node
# (renderD128 = msm) is tried first (cros_gralloc_driver.cc init_try_nodes), so buffers come from the GPU device and the
# composer imports them into card1 by PRIME. Linear only: ro.hardware.vulkan=pastel makes minigbm add CPU usage to every
# GPU buffer (is_running_with_software_rendering), and vendor.minigbm.debug=nocompression disables the a6xx-layout UBWC
# modifier outright (Adreno 512 is a5xx; DPU/Mesa a5xx UBWC unverified).
$(call soong_config_set,minigbm,platform,msm)

# r6m: Mesa's CrOS perform bridge must use the same libminigbm_gralloc and
# imported-handle registry as mapper.minigbm. Without this legacy module,
# hw_get_module falls back to gralloc.default, which cannot describe YUV
# camera buffers; SurfaceFlinger then aborts importing the preview texture.
# The cc_library_shared module installs both ABIs. Keep the existing selector.

PRODUCT_VENDOR_PROPERTIES += \
    vendor.minigbm.debug=nocompression \
    ro.hardware.gralloc=minigbm \
    ro.hardware.egl=angle \
    ro.hardware.vulkan=pastel
