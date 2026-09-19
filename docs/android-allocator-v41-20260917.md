# V41: Android allocator integration

V40 produced a visible LCD pattern using simpleDRM and passed raw DRM atomic
updates and PRIME buffer sharing. V41 moves up to the actual Android minigbm
allocator. This is still preparation for a running Android compositor and
framework, not a claim that the launcher works.

## Source change

Pinned minigbm revision: `ce79bbee67b3e2ea7abb110e1415e9ceceaa6471`.
The source tree was clean before the change. Three additions register
`simpledrm` with the existing dumb-buffer backend and include it in the driver
lookup table. Original files, patched files, hashes and a Git patch are saved
in `research/android-allocator-v41-20260917`.

No other backend or kernel code is changed. The new backend uses the same
allocation, mapping and PRIME operations exercised by V40. Advertising a
format through the common backend is not by itself proof that every Android
format/usage combination works, so the probe tests the concrete UI formats.

## Combined probe

The vendor executable links the real `libminigbm_gralloc`. It requests full
1080×2340 RGBA8888 and RGBX8888 Android buffers with compositor, render, texture
and CPU access usage flags. It enables the allocator's metadata FD and checks
the buffer name/dimensions/stride. It writes every pixel, passes the native
handle and its FDs over SCM_RIGHTS to a fresh executable process, imports it
through the allocator and verifies every pixel. Each process releases its
allocator references and native handles.

The known V40 simpleDRM module is reused. Device access is isolated in a private
mount namespace. The allocator singleton retains a device FD until process
exit, so the probe detaches its private device mount before normal teardown.
It does not access persistent partitions or change display modes.

The same build also prepares the real allocator 4.0 service, mapper 4.0 library
and AIDL composer3 V5 DRM service. Building those artifacts is not equivalent
to starting or validating the services on the phone.

## Following step

The compositor uses `GraphicBufferMapper` and requires mapper metadata at least
at gralloc 4. It must run with working Binder/HIDL service discovery, a matching
VINTF declaration and vendor library paths. Software rendering is available
in source as SwiftShader `vulkan.pastel`, with ANGLE for GLES; it has not yet
been built or validated for the A6L. Full framework/APEX startup and product
configuration remain ahead.

Status: physical test passed at 2026-09-17 14:59:38 UTC. All 38 payload files
were verified again on the phone. Both full-screen format allocations passed,
including metadata handling, fresh-process imports and verification of every
pixel. Authenticated ADB remained alive and global mounts were unchanged.
Evidence is archived in `captures/capture-allocator-user-v41/allocator`.
The capture coordinator restored laptop services at 15:13:55 UTC after its
return window expired. Stock Android's return is still unverified: the phone
was absent from USB at the last check. The original session record is kept.

The 38-file, 8,327,008-byte payload also passed diskless QEMU checks. Before
launch, all payload hashes, four package/runner pins and the nine original V38
tool hashes were verified. Existing V38 recovery was reused; no image was
flashed and no persistent filesystem was mounted by this test.

The first build found a missing native-window header dependency in the probe.
Its build definition now links `libnativewindow`, matching minigbm's defaults.
The original failure log is preserved; the corrected build completed in 4m31s
and is recorded in `build-android-allocator-v41-r2.log`. The allocator 4.0,
mapper 4.0 and HWC3 DRM binaries all built, with hashes saved separately.

Packaging attempt 1 exposed the runtime library `libdl_android.so` living in
the runtime APEX. The package now resolves that explicit location. Attempt 2
passed both format tests, each importing into a fresh process and verifying
all 2,527,200 pixels. It uses the exact V38 kernel and V40 display module.
Evidence is in `firmware/extracted/android-allocator-v41-20260917-r2`.

The minimal recovery still lacks generated linker configuration; QEMU reports
that warning but completes the allocator checks. Proper linker namespaces and
service discovery remain work for full graphics-service integration.
The simpleDRM module remains in RAM until reboot. The capture coordinator has
finished and the laptop's services are restored, with no owned pause remaining.
V40's final return evidence remains separate from its original timed-out report.

Sources checked: [minigbm](https://android.googlesource.com/platform/external/minigbm/)
and [DRM hardware composer](https://android.googlesource.com/platform/external/drm_hwcomposer/),
plus the pinned local sources and their build definitions.
