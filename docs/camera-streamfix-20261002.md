# Camera stream setup — 2 October 2026

r6f camera launch under Mesa sysmem triggered a GPU fault before a camera
opened. After reboot and selection of ANGLE/SwiftShader, Aperture passed
the splash screen but reported an error preparing the stream and closed.
These are separate failures.

The software-renderer log shows camera 0 (rear IMX576) opens successfully.
It requests PRIVATE/NV12 preview and JPEG, both 1600x1200. JPEG is mapped
to the same libcamera stream. `validate()` returns Adjusted, with the logged
image still 1600x1200 NV12/sYCC. The HAL returns EINVAL, and Android reports
the inputs/outputs unsupported. No preview or photo was produced.

Source inspection found `CameraDevice::configureStreams()` constructs stream
configurations with bufferCount=0. `SimpleCameraConfiguration::validate()`
supplies its default buffer count and marks the configuration Adjusted.
The HAL rejected every Adjusted result, even if Android's requested image
requirements stayed unchanged. This explains a source-level failure matching
the observed rejection; confirmation on the phone remains pending.

Patch `device/hisense/a6l/camera/libcamera/patches/0012-android-accept-internal-stream-adjustments.patch`
accepts adjustments of internal buffer count, stride and frame size. It still
rejects Invalid configurations and changes to stream count, orientation,
dimensions, pixel format, or explicitly requested colour space.

Verification:

- Regression compiled the actual old/new HAL validation blocks and the actual
  simple-pipeline buffer-count code. All 11 cases passed: old rejection
  reproduced, internal adjustments accepted, image-changing cases rejected.
- Patch applies to the preserved r6f source; isolated Android HAL compile passed.
- Packaged IPA signature and embedded public key checks passed.
- New prebuilts staged for the next ROM; HAL SHA-256 starts a407088a.

The patch was subsequently installed in r6g (HAL SHA-256 starts a407088a).
The physical test on 2 October still fails: rear camera 0 opens, requests
1600x1200 NV12 preview plus mapped JPEG, logs Adjusted, then returns the
unsupported-stream error. Pierre confirms that Aperture closes with its error
message. Preview, saving and front/rear switching remain unverified.

The new log lacks the patch's `Requested image configuration changed` message,
so the earlier stream-count/orientation guard is a candidate for further
inspection. The exact changed field is not yet logged or established.
The source-level regression covered internal buffer changes but did not
establish that these were the only adjustments on the phone.

r6g evidence:
`firmware/extracted/rom-r6g-20261002/r6g-camera-eink-attended-20261002.tar.gz`.

## Native sensor orientation follow-up

The remaining silent rejection matches another source-level failure. The HAL
generates an empty configuration, whose orientation defaults to Rotate0.
`SimpleCameraConfiguration::validate()` calls the sensor's `computeTransform()`.
For native 90/270-degree mounting, the requested upright image needs a transpose
that this sensor path cannot provide, so validation replaces the orientation
with the native mounting orientation. Patch 0012 then rejects that change.
The A6L device tree specifies 90 degrees for the rear sensors and 270 for the
front sensor. The logged stream count and image stay at one 1600x1200 NV12
stream; the old log does not print the orientation field itself.

Android already receives the sensor's clockwise rotation correction through
SENSOR_ORIENTATION. Asking libcamera to rotate these buffers upright duplicates
that responsibility. New patch
`0013-android-request-native-sensor-orientation.patch` explicitly requests the
native mounting orientation by inverting the metadata correction that
`CameraDevice::initialize()` computed. This produces identity sensor transform
and leaves the Android metadata responsible for display orientation. It retains
the existing image, colour, stream-count and unexpected-orientation guards.
Each early guard now logs its changed field; malformed rotation metadata is
rejected explicitly.

This follows the documented [libcamera orientation contract](https://docs.libcamera.org/master/public-api/classlibcamera_1_1CameraConfiguration.html)
and the [sensor transform contract](https://docs.libcamera.org/master/internal-api/classlibcamera_1_1CameraSensor.html).
The rejection diagnosis is an inference from the physical log plus the exact
pipeline path; the phone did not previously log the changed orientation value.

Verification on 2 October:

- 35 regression checks compile the actual orientation/transform sources,
  sensor `computeTransform()` body, simple-pipeline orientation and allocation
  code, and old/new HAL guards. They reproduce the r6g rejection for 90/270
  mounting and verify all four mounting rotations, with and without sensor
  flip support, request native untransformed buffers successfully.
- Dimension, pixel-format, explicit colour, stream-count, unexpected
  orientation, Invalid configuration and malformed rotation changes still
  reject. Internal allocation changes and unspecified colour defaults pass.
- The patch applies to the preserved r6g camera source. The isolated Android
  build completed all 247 targets. Stripped IPA signature and embedded public
  key checks passed.
- New payload is staged separately at
  `firmware/extracted/camera-native-orientation-20261002/payload/`; no installed
  r6g pins, prior payload or device prebuilts were overwritten. New camera HAL
  SHA-256 is `2cb55e78b99797b9a9fa9ca928d64c7b42249f2fd247f44f799893d22c64c658`.

Source: `/home/a6l/libcamera-work/src-native-orientation-20261002`.
Build: `/home/a6l/libcamera-work/build-hal-native-orientation-20261002`.
Signed package: `/home/a6l/libcamera-work/halpkg-native-orientation-20261002`.
Regression and build reports are in
`firmware/extracted/camera-native-orientation-20261002/`.

The native-orientation fix was subsequently installed in r6j. The physical
Camera test passed stream configuration and reached the Configured state,
then the provider aborted while cloning the first capture-request metadata.
See `docs/camera-metadata-crash-20261002.md` for the reproduced schema
incompatibility and patch 0014 candidate. Rear preview, one saved photo,
portrait/landscape orientation and front/rear switching remain unproven.

Evidence: `logs/r6f-install-20261002/camera-stream-error-20261002T072214Z/` and
`firmware/extracted/camera-streamfix-20261002/`.
