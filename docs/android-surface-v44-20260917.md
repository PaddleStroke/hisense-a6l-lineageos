# V44: actual SurfaceFlinger with software Vulkan

V43 is complete: physical composer presentation and colours passed, stock
Android return and host cleanup verified. The existing V38 recovery remains
installed. V44 preparation has not changed the phone.

V44 starts actual SurfaceFlinger, SwiftShader Vulkan, the verified minigbm
allocator/mapper and DRM composer in a private RAM root. A native client creates
four colour layers through SurfaceComposerClient and asks SurfaceFlinger to
compose them. Diskless QEMU must verify the complete resulting image before
the phone trial. This is not a full Android framework/launcher boot.

The private environment now includes fresh Bionic property areas populated
from the diagnostic snapshot and a restricted property socket for actual
service-manager readiness and debug settings. Renderer selection and BGRA
composition-format settings affect that private environment only. No storage
nodes, persistent mounts or image writes are provided.

The first build graph exposed missing vendor variants for libskia_skcms and
libhwy, referenced by libjxl. Their original definitions and the two added
vendor_available declarations are archived under research/android-surface-v44-20260917.

The next coordinator uses Wait-AndroidRamReady.py: exact USB identity and port,
authenticated ADB, expected kernel/security/root/virtual mounts stable for four
seconds. Its 90-second post-selection limit is a failure ceiling, not a wait.
The full V38 serial/storage collector stays available for regressions.

References consulted alongside the pinned local sources:
- https://source.android.com/docs/core/graphics/arch-sh
- https://skia.googlesource.com/skia/+/5aa32d5293f0/site/docs/user/special/vulkan.md
- https://android.googlesource.com/platform/external/swiftshader/+/3cb9efb0b72ae80aa9db1166b1209a0d40def4ec

Build and emulator validation passed (attempt 7). No phone test has started.

The exact 64-bit build passed in 9m34s after the all-variant build was stopped to avoid unused host/32-bit work. QEMU r2 reached actual SurfaceFlinger but rejected SwiftShader's absent SYNC_FD semaphore support. The pinned SwiftShader source has no implementation of that handle type. Switched to SkiaGL threaded through the existing ANGLE libraries; SkiaGLRenderEngine.cpp implements sync_wait and GrSyncCpu::kYes fallbacks when native fence export is unavailable. No capability check was bypassed.

QEMU r3 exposed costly optional shader warmup; r4 disables service.sf.prime_shader_cache only in the private test properties. SurfaceFlinger and all services then completed, but the full-pixel check caught an entirely black display. SurfaceFlinger remains in BOOTLOADER until a real image buffer is latched; four effect-only layers do not trigger that transition. The client now submits a small black buffer under the four colour layers.

QEMU r5 verified entry into boot animation/composition, then caught RGBA/AB24 client targets rejected by simpleDRM. The default format property alone does not control this path: RenderSurface initializes RGBA and takes a format update through the HWC client-target property response. Fix-SurfaceFormatV44.py archives and adds a simpleDRM-only BGRA/SRGB response through the standard AIDL validation result. Other DRM devices keep their existing behavior.

QEMU r6 caught the renderer's missing BGRA native-buffer mapping. An upstream search found the exact fix, Skia ed4e2bf5398b997c9beb87e832a2f9a5cdc71485, merged September 11, 2026. Applied the unmodified reviewed patch after git apply --check, retaining author attribution and an archived patch/provenance. It pairs the BGRA SkColorType with the GL_BGRA8 backend format and retains the driver's renderability checks. SurfaceFlinger rebuild r7 finished; QEMU r7 now also requires actual client composition and zero reported composer commit failures. No V44 phone action yet.

Upstream fix: https://skia.googlesource.com/skia/+/ed4e2bf5398b997c9beb87e832a2f9a5cdc71485

QEMU r7 PASSED all twelve checks: module, zero exit, live services, restored global readiness property, no panic, successful native transaction, sampled framebuffer, actual ANGLE/SwiftShader renderer, actual client composition, zero reported composer commit failures, all 2,527,200 pixels exact, adjacent 4KiB untouched. Build r7 took 54.83 seconds. Package has 146 files (80,661,462 bytes). Phone package and all runner hashes are staged on the laptop, with eight pins and nine existing V38 tools verified. Awaiting user availability before launching the physical boot; stock Android continues running.
