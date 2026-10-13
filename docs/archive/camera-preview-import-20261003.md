# r6l Camera preview: SurfaceFlinger import failure

## What the physical test established

The retained evidence is
`firmware/extracted/rom-r6l-20261003/camera-failure-snapshot.tar.gz`.
At 09:21:52.329 the camera provider configured a 1600x1200 NV12/sYCC
source shared by PRIVATE preview and mapped JPEG. The software ISP received
2880x1620 RGGB10 input. The sensor streaming check passed, and the first
captured frame reached the provider at 09:21:52.950.

SurfaceFlinger then aborted at 09:21:53.025:

```
Failed to create a valid texture. [...]:[1600,1200] isProtected:0 isWriteable:0 format:34
```

Skia reported `Could not create EGL image, err=(0x3000)`. Aperture reported
preview STREAMING at 09:21:53.034; that means frames reached the consumer,
not that a visible preview was successfully composited. The provider did not
abort at the previous metadata or buffer-import barriers.

Init restarted zygote following the SurfaceFlinger death, then Android returned
to its boot animation. The kernel boot ID remained
`569ce3af-80a6-4ca8-bb2e-c85d0a29930e`, with uptime continuing past the event.
This test caused an Android graphics/framework restart, rather than an observed
kernel reboot or GPU fault. The unrelated recurring Bluetooth aborts are not
the first crash in this sequence.

## Importer mismatch and selected correction

The original SurfaceFlinger startup already contains:

```
Gralloc doesn't support lock_ycbcr (video buffers won't be supported)
```

The GPU investigation followed the actual retained Mesa source. Its Android
fallback treats HAL format 34 (IMPLEMENTATION_DEFINED) as potentially YUV;
without a legacy `lock_ycbcr` callback it returns `-EINVAL` before importing
the DMA buffer into DRI. The early return leaves the EGL error as EGL_SUCCESS
(0x3000), matching the physical Skia message.

Both vendor ABIs contain `gralloc.default.so`, while the configured selector is
`ro.hardware.gralloc=minigbm`. The selected next correction is to package the
existing generic `gralloc.minigbm` module for both ABIs. It shares the same
`libminigbm_gralloc` driver instance as the installed minigbm mapper and exposes
the metadata operations used by Mesa's CrOS importer. The GPU agent owns its
source-level import regression and packaging review; root owns integration.
The camera HAL remains the validated patch-0015 candidate:

```
ee1e8f28e1f998b4e763d7b45bfc325d6fea51b7259a134dffd311987a98412e
```

Changing only PRIVATE usage to allocate RGB would still leave HAL format 34
and the same missing-callback failure. A possible separate fallback is explicit
PRIVATE-output override to RGBA_8888 through the existing AIDL wrapper, which
already exports `camera3_stream_t::format` as `HalStream.overrideFormat`.
The platform's current `HalStream.aidl` explicitly allows this for PRIVATE
output streams. Mesa's explicit format-1 fallback imports one linear ABGR8888
plane without entering the YUV path. That alternative would require a real
NV12-to-RGBA postprocessor and corresponding format/stride/color tests. It has
not been implemented because restoring the existing minigbm bridge preserves
the current NV12 path and avoids the additional conversion.

## Focused physical probe after integration

Root should retain a pre-test boot ID, uptime, and SurfaceFlinger/system_server
PIDs together with live logcat and kernel logging. Verify both ABI modules in
the installed image and the selected gralloc property before launching Camera.
Do not regard a module's presence alone as proof Mesa selected that backend.

1. Open the rear Camera once and wait for a visible, moving preview. Observe
   it for ten seconds. The old missing-lock_ycbcr warning and EGL-image/texture
   fatal must be absent from the new process startup/test interval. Confirm
   boot ID and graphics/framework PIDs stay stable.
2. Inspect the preview for correct orientation, Y/UV color, full image bounds,
   and horizontal row corruption. Patch 0015 deliberately copies packed ISP
   NV12 into minigbm's padded planes; successful GPU import alone does not prove
   that image content is correct.
3. Take one photo and open the saved result. Record JPEG dimensions, orientation,
   byte size, and whether the file decodes. This exercises the separate BLOB/R8
   destination and JPEG encoder, which the r6l attempt did not reach.
4. Only after the rear preview and photo pass, switch to the other cameras,
   then reopen Camera and rotate once. Keep sensor-specific failures separate
   from the original format-34 compositor failure.

If the screen returns to a boot animation, preserve the earliest fatal and
compare boot ID, uptime, and PIDs before any forced reset. If import succeeds
but JPEG fails, capture the provider/JPEG error rather than rebuilding unrelated
graphics components. Video recording and prolonged camera performance remain
separate later tests.

The existing 44-check buffer-import regression covers actual minigbm native
handles, image-plane strides/offsets, metadata exclusion, JPEG destination
capacity, and NV12 postprocessing. It does not emulate SurfaceFlinger's EGL
import or prove a saved physical photo. No HAL change, phone operation, or new
camera payload was made for this investigation.
