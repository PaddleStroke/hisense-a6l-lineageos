# r6j Camera: capture-request metadata abort

## Physical observation

On 2 October, r6j Camera closed with its generic critical-error message. The
camera provider died twice, at 22:39:17.237 and 22:39:30.186 phone time, with:

```
camera_metadata.c:630: append_camera_metadata:
assertion "validate_camera_metadata_structure(dst, NULL) == OK" failed
```

The native stack is `CameraDevice::processCaptureRequest` →
`Camera3RequestDescriptor` → `CameraMetadata` copy constructor →
`clone_camera_metadata` → `append_camera_metadata`. This is the initial capture
request, after stream configuration, rather than the earlier r6g stream guard.

The same attempt successfully configured the rear IMX576 at 1600x1200 NV12/sYCC
with a mapped JPEG stream. Aperture logged `CaptureSessionState: Configured` at
22:39:17.139. Patch 0013's native-orientation change has therefore passed this
physical configuration step. Preview and a saved photo are still unproven.

Small retained evidence:
`firmware/extracted/rom-r6j-20261002/camera-failure/camera-metadata-abort.txt`.
Original logs are in the laptop kit:
`rom-r6j/logs/live-r6j-firstboot-20261002/logcat-live.txt`.

## Cause and correction

The HAL statically bundles libcamera's older Android camera metadata library and
tag schema. Its strict validator compares every standard entry's stored type
with that schema's type lookup. Modern standard tags such as
`ANDROID_CONTROL_ZOOM_RATIO` and `ANDROID_CONTROL_SETTINGS_OVERRIDE` are absent
from the old table. The lookup returns -1, validation fails, and cloning a valid
current-framework request reaches the observed assertion.

This is a demonstrated compatibility failure: a request produced with the
current platform schema reproduces the exact assertion against the old HAL's
metadata implementation. The physical log does not identify the individual tag
in the failing request, because the metadata library writes its detailed
validation error to the provider's discarded stderr. The claim is schema
incompatibility with a reproduced failure mechanism, not a captured tag dump.

Patch `0014-android-update-camera-metadata-schema.patch` replaces the generated
tag enum and name/type table together with the exact Android 17 platform copies
used by this ROM. It leaves the metadata packet implementation and strict
validation intact. All 263 older standard tag IDs, types and public names are
unchanged. New entries follow the platform's append-only enum contract.

Canonical source: [LineageOS android_system_media revision
a6e89ff7fdee50366c2790e09531fd94f81e522b](https://github.com/LineageOS/android_system_media/tree/a6e89ff7fdee50366c2790e09531fd94f81e522b/camera).
Both generated file hashes and paths are recorded in
`firmware/extracted/camera-metadata-schema-20261002/platform-provenance.json`.

## Validation and candidate payload

- Host regression uses the actual old/new `camera_metadata.c` implementations,
  with each library bound to its own schema. Old cloning aborts with SIGABRT at
  the same assertion; updated cloning passes 24 checks and preserves modern and
  legacy values, including out-of-line arrays and aligned int64 data. Wrong
  entry type, excessive count and bad data offset still fail validation.
- Separate enum and lookup checks preserve all 263 legacy tag IDs, types and
  names. The patch passes reverse-application checking against the compiled
  source.
- Incremental NDK r27c/API30 compilation passed 22 Ninja steps. Only dependent
  HAL objects and its metadata archive were rebuilt. No ROM image was built.
- The candidate replaces only `lib64/hw/camera.libcamera.so` in a separate
  package. Existing `libcamera.so`, `libcamera-base.so`, IPA and IPA signature
  are retained byte-for-byte. IPA signature verification passed. The generated
  libcamera version dependency in the build directory was deliberately not
  packaged, because the metadata schema has no libcamera ABI change.

Candidate package:
`/home/a6l/libcamera-work/halpkg-metadata-schema-20261002`.
Workspace copy:
`firmware/extracted/camera-metadata-schema-20261002/payload`.
New stripped camera HAL SHA-256:
`819f9824886d7549c74d63e2177dd4d5e28e13d8f63e6f45e22fab34f71596d2`.

Build source: `/home/a6l/libcamera-work/src-native-orientation-20261002`, now
with patch 0014 as well. The earlier signed native-orientation package remains
unchanged. The isolated host fixture is
`/home/a6l/libcamera-work/src-metadata-schema-20261002`.
Regression sources, reports and incremental build log are retained beside the
workspace candidate payload.

No phone commands, settings changes, app launches, flashes or device prebuilt
replacement were performed for this investigation. The correction was later
integrated into r6k, whose 3 October physical Camera attempt passed this
metadata-copy barrier and reached Active repeating capture. It then aborted
while importing the first Android output buffer. See
`docs/camera-buffer-import-20261003.md` for that new failure and patch 0015.
On-device preview, a saved rear photo, rotation and front/rear switching remain
necessary.
