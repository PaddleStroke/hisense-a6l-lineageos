# r6k Camera: minigbm buffer import

## Physical failure and progression

The 3 October Camera attempt successfully configured rear preview and mapped
JPEG, submitted a repeating request, and reached the Active capture-session
state at 07:52:57.076. It passed the r6j metadata-clone barrier corrected by
patch 0014. At 07:52:57.167 the provider aborted with:

```
ERROR HAL generic_camera_buffer.cpp:84 Discontiguous planes are not supported
FATAL HAL camera_device.cpp:897 Failed to create CameraBuffer
```

Stack: `CameraDevice::processCaptureRequest` → `createFrameBuffer` →
`LogMessageAbortGuard`. This is a new initial output-buffer import failure, not
the prior metadata assertion or the unrelated Bluetooth crashes.

Exact provider excerpt:
`firmware/extracted/rom-r6k-20261003/camera-failure/camera-buffer-crash.txt`.
Original laptop log:
`rom-r6k/logs/live-r6k-firstboot-20261003/logcat-live.txt`.

## Cause and correction

The generic Android importer assumes one image DMA buffer and treats all native
handle FDs as image descriptors. It rejects distinct numeric FDs even when they
are `dup()` aliases of the same allocation. This ROM's minigbm handle contains
one FD per image plane, plus an optional separate metadata-region FD. It also
contains the actual strides, offsets, sizes and format modifier.

The exact minigbm contract is established from the ROM's source at revision
`ce79bbee67b3e2ea7abb110e1415e9ceceaa6471`: `cros_gralloc_handle.h`,
`cros_gralloc_driver.cc`, `cros_gralloc_helpers.cc` and `msm.c`. The physical
error identifies the multiple-FD rejection; the device log does not print the
individual handle fields. Real minigbm-layout fixtures reproduce the same old
rejection and demonstrate corrected importing. Source provenance and the
vendored header hash are retained in `minigbm-provenance.json` beside the tests.

Patch `0015-android-import-minigbm-plane-layout.patch`:

- Recognizes the exact minigbm native-handle ABI and magic, validates plane/FD
  counts, requested format/dimensions, linear modifier, descriptor lengths,
  strides, offsets and plane sizes. Imports image planes individually, including
  duplicate aliases or separate allocations, and excludes the metadata FD.
  Unknown handles retain the generic fallback. Invalid input fails safely.
- Uses internal software-ISP buffers for NV12 and the existing YUV postprocessor
  to deliver Android output. The MSM allocator's Venus padding means a nominal
  1600-pixel NV12 row has a 1664-byte stride, and chroma follows aligned Y
  scanlines. Writing the ISP's packed layout directly would therefore corrupt
  output even after accepting the FDs. Postprocessing now uses the actual
  destination plane strides, with corresponding bounds checks.
- Imports a postprocessed destination using its own Android format and size.
  A mapped JPEG destination is BLOB/R8 storage, rather than its NV12 source's
  two-plane layout. This source-path bug would otherwise block photo capture.
- Changes invalid frame-buffer import logs from Fatal to Error, allowing the
  existing request error path to return a failure instead of aborting the
  provider. The generic fallback also marks undersized planes invalid.

The minigbm handle definition and BSD license are vendored together in the
camera patch. No gralloc, kernel, IPA, sensor, or libcamera core behavior changed.

## Verification and candidate

The regression compiles the actual `CameraBuffer`, `PostProcessorYuv`, request
buffer lifetime code and real libcamera/libyuv libraries. It uses real memfd,
dup and mmap buffers rather than a mirrored importer implementation.

The old importer reproduces the physical `Discontiguous planes` error. The
corrected code passes 44 checks covering metadata exclusion, shared and separate
plane allocations, real offsets/stride/capacity, JPEG BLOB capacity, and malformed
handles. Actual NV12 postprocessing preserves every Y/UV row at its padded
destination offset, leaves row/scanline padding unchanged, and leaves metadata
untouched. The host core fixtures contain no pipelines or IPAs and do not build
an Android image.

NDK r27c/API30 incremental HAL compilation passed. Source changes rebuilt only
the necessary HAL objects; the build's generated core version object was not
packaged. The isolated package replaces only `camera.libcamera.so`; existing
libcamera/base libraries and signed IPA remain byte-for-byte unchanged. IPA
signature verification passed.

Package: `/home/a6l/libcamera-work/halpkg-minigbm-import-validated-20261003`.
Workspace copy: `firmware/extracted/camera-minigbm-import-20261003/payload-validated`.
New HAL SHA-256:
`ee1e8f28e1f998b4e763d7b45bfc325d6fea51b7259a134dffd311987a98412e`.
The complete manifest is the package's `SHA256SUMS`.

Re-run the regression in WSL with:

```
python3 /mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/camera-minigbm-import-20261003/run-regression.py
```

The recorded host build dependencies are
`/home/a6l/libcamera-work/build-host-camera-buffer-20261003` and
`build-host-libyuv-camera-20261003`; the compile manifest lists all source and
include paths. Production source remains
`/home/a6l/libcamera-work/src-native-orientation-20261002`, now with patch 0015.

No phone interaction, ROM image build or device-prebuilt replacement occurred.
Physical preview and a saved photo are still required after integration. Check
portrait/landscape orientation, front/rear switching, preview color/stride,
JPEG saving and several minutes of continued frames. The extra NV12 copy costs
CPU time; it is needed for the current gralloc layout, and performance remains
to be measured on the phone. Later capture/ISP/JPEG faults remain possible.
