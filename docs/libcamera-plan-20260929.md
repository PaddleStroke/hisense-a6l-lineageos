# libcamera for the A6L camera: architecture, plan and phase-1 build (29 Sep 2026)

**Status:** offline only. No adb, no phone, nothing flashed, the Lineage tree and its `out/` untouched (the r5 image build ran concurrently).
- **Built (phase 1):** upstream libcamera **v0.7.2** + 3 A6L patches, cross-built with NDK r27c for aarch64 Android (bionic, API 30). It includes the simple pipeline handler, SoftISP (CPU debayer), the simple IPA (in-process, signed) and the `cam` utility.
- **Bundle:** `firmware/extracted/libcamera-20260929/` (12 files, `SHA256SUMS` sha `3be25657…`), staged on the laptop as `~/A6L-usb-20260915/v75/libcam1` (12/12 OK).
- **First on-phone run done (t37, 29 Sep, libcam1):** env, list (3 cameras) and a rear 1440x1078 capture at 30 fps with AGC PASS; full-resolution capture failed on the dma-heap. Results and the libcam2 fixes: §10.
- **libcam2 bundle:** `firmware/extracted/libcamera-20260929b/` = laptop `v75/libcam2` (12/12 OK, `SHA256SUMS` `03a1392b…`). Next attended step: §10.4.
- **Phase 2 (lc2, 29 Sep, offline):** the libcamera Android HAL (v0.7.2 + patches 0001-0007) and the A6L camera provider fork (torch strength levels) are wired into the ROM product. Integrated, checked and first installed-ROM tests: §11.

Hardware basis (proven 29 Sep, `docs/camera-rom-20260929.md`, `docs/camfix12-20260929.md`):
- Mainline qcom-camss with the rom1 patch, RDI raw path.
- Media graph: `msm_csiphyN → msm_csidN → msm_ispifN → msm_vfe0_rdiN → video`.
- Rear imx576: RAW10 RGGB, 2880×2156.
- Front s5k3t1: RAW10 GRBG, 2304×1728. GRBG is confirmed by the t36 front PNGs and is the driver's default mbus code.
- hi846 does not stream yet.
- GT9769 VCM, and the PM660L flash LED `white:flash`.

## 1. Architecture (target: Lineage 24 / Android 17)

```
 Camera2 app ─ cameraserver ──AIDL── android.hardware.camera.provider-service.lineage  (vendor, ICameraProvider/internal/0, V1)
                                        │  hw_get_module("camera") → /vendor/lib64/hw/camera.<ro.hardware.camera>.so
                                        ▼
                         libcamera Android HAL3 adapter (src/android, camera3_hal.cpp, HAL_MODULE_INFO_SYM)
                                        │  libcamera C++ API
                                        ▼
            libcamera core: simple pipeline handler ("qcom-camss", SoftISP on) + ipa_soft_simple (AGC/AWB/BLC/CCM/gamma)
                                        │  V4L2 / MC / subdev ioctls, dmabuf
                                        ▼
       kernel: qcom-camss (rom1, a6l_wm=3 zero-copy) ─ imx576_a6l / s5k3t1 / hi846 ─ gt9769 VCM ─ leds-qcom-flash
```

**Camera provider: use the existing Lineage AIDL wrapper. No new provider has to be written.**
- The Android 17 framework matrices (`compatibility_matrix.202604/202704.xml`) list **only AIDL** `android.hardware.camera.provider` 1-4. The HIDL 2.x legacy wrapper cannot be used.
- The tree already has `hardware/lineage/interfaces/camera/aidl/` (`android.hardware.camera.provider-service.lineage` and `camera.device-impl.lineage`). It is an AIDL provider that loads a legacy **camera3 HAL module** through `hw_get_module(CAMERA_HARDWARE_MODULE_ID)` and wraps `camera3_device_t` in AIDL `ICameraDevice`/`ICameraDeviceSession` (FMQ metadata, buffer caches).
- That is exactly what the libcamera Android adapter provides.
- Plan: `PRODUCT_PACKAGES += android.hardware.camera.provider-service.lineage`, the HAL module installed as `/vendor/lib64/hw/camera.libcamera.so`, and `ro.hardware.camera=libcamera`.
- **Gap:** the Lineage wrapper stubs `turnOnTorchWithStrengthLevel` and `getTorchStrengthLevel` as `OPERATION_NOT_SUPPORTED`. The libcamera HAL has no torch at all. See §6.

**libcamera HAL build route.**
- Build it with meson + NDK as a **prebuilt**: `-Dandroid=enabled -Dandroid_platform=generic`. libcamera ships its own copies of the libhardware, system/core and camera_metadata headers under `include/android`.
- Needs libjpeg, libexif and libyuv (wrap), built statically.
- Import through `cc_prebuilt_library_shared` in `device/hisense/a6l/camera/`, plus `libcamera.so`, `libcamera-base.so`, `ipa_soft_simple.so(.sign)` and the tuning yamls in `/vendor/lib64/libcamera/ipa` and `/vendor/share/libcamera/ipa/simple`.
- `/vendor/etc/libcamera/camera_hal.yaml` is already drafted.
- Prefix: build with `--prefix=/vendor` so the compiled-in paths are right.
- libc++ is static (NDK `std::__ndk1`, no clash with the platform libc++ in the provider process).
- The Lineage tree's `external/libcamera` (AOSP snapshot 0.1.0, virtual pipeline only) stays unused. Porting upstream meson to Soong is more work than a prebuilt, and the prebuilt keeps the phase-1 recovery build identical except for the `android` option.

**Buffer path.**
- The sensor streams through the CSI-2 path (CSIPHY → CSID → ISPIF → VFE RDI WM) into vb2 **dma-sg** buffers, which reach the V4L2 capture node.
- The simple pipeline allocates them with `VIDIOC_REQBUFS` and exports them with **`VIDIOC_EXPBUF`**. vb2-dma-sg implements `get_dmabuf`, so qcom-camss supports it with no change. The first attended run proves it (SoftISP maps the input dmabuf).
- **`a6l_wm=3` (zero-copy) is required.** With wm=6 the driver copies each frame into the queued buffer and the VFE sequence number steps by 2.
- The SoftISP reads the RAW10 CSI2P buffer (CPU mmap of the dmabuf), debayers it and writes the output buffer:
  - **Standalone (`cam`):** the output buffers come from `DmaBufAllocator`, which tries `/dev/dma_heap/linux,cma`, then `reserved`, `system`, and `/dev/udmabuf`. Both ROM kernels have `CONFIG_DMABUF_HEAPS_SYSTEM/CMA=y` and `CONFIG_UDMABUF=y`.
  - **Under the HAL:** the output buffers are the **gralloc buffers** that cameraserver passes to `process_capture_request`. The ROM's allocator is minigbm (`android.hardware.graphics.allocator-service.minigbm`, `mapper.minigbm`), so the buffers are dmabuf-backed. libcamera's `generic_camera_buffer.cpp` maps `buffer_handle_t` fds as dmabuf planes. SurfaceFlinger or the encoder consumes them directly.
- **Internal buffers:** libcamera's `generic_frame_buffer_allocator.cpp` uses the legacy **gralloc0** module for the buffers it allocates itself (JPEG/BLOB post-processing, mapped streams). Options:
  - ship minigbm's `gralloc.minigbm` (gralloc0), or
  - patch the allocator to use `DmaBufAllocator` (preferred, about 50 lines).
- **SoftISP output formats:** RGB888/BGR888/XRGB8888/ABGR8888 (CPU), with no NV12/YUV.
  - Android preview (`IMPLEMENTATION_DEFINED` → minigbm picks `XBGR8888` for camera+composer usage) and JPEG (the HAL encodes from RGB via libyuv) work.
  - **Video (MediaCodec needs YCbCr_420_888/NV12)** needs a SoftISP YUV output. Upstream work on it is in progress; otherwise use a libyuv post-processor in the HAL. That is a later milestone.

**libcamera version.**
- Use **v0.7.2** (latest tag, `191e2021`).
- 0.7 has the SoftISP with CCM, the GPU (EGL) debayer option (`softisp-gpu`), the `qcom-camss` simple-pipeline entry with SoftISP enabled, and the signed-IPA in-process path.
- Pin it. Rebase A6L patches only at releases.

## 2. Kernel features (checked against the v67 and r5 `.config`)

| feature | status |
|---|---|
| `CONFIG_MEDIA_CONTROLLER`, `CONFIG_VIDEO_V4L2_SUBDEV_API` | y / y |
| vb2 dma-sg (`VIDEOBUF2_DMA_SG=m`), dmabuf export (`VIDIOC_EXPBUF`) | built; EXPBUF path proven by the first `cam` capture |
| dma-heaps `SYSTEM`/`CMA`, `UDMABUF`, `SYNC_FILE` | y |
| sensor drivers | imx576_a6l and s5k3t1 already expose the libcamera mandatory controls:<br>• ANALOGUE_GAIN, EXPOSURE, HBLANK, VBLANK, PIXEL_RATE, LINK_FREQ<br>• HFLIP/VFLIP with MODIFY_LAYOUT<br>• TEST_PATTERN<br>• selection targets CROP/CROP_DEFAULT/CROP_BOUNDS/NATIVE_SIZE<br>• `v4l2_fwnode_device_parse` (DT `rotation`/`orientation` → CAMERA_ORIENTATION/SENSOR_ROTATION) |
| VCM | gt9769 v4l2 subdev with `V4L2_CID_FOCUS_ABSOLUTE`, linked by DT `lens-focus` |
| flash | `CONFIG_LEDS_QCOM_FLASH=m` and `LEDS_CLASS_FLASH=m`, but **`V4L2_FLASH_LED_CLASS` is not set**, so there is no v4l2-flash subdev. Torch goes through the LED sysfs (§6), which is all that Android torch needs. |

**Sensor model names.**
- libcamera takes the model from the entity name before the I2C address, so the models are **`imx576_a6l`** (driver name) and **`s5k3t1`**.
- The A6L patch registers properties and helpers under `imx576_a6l`, `imx576` and `s5k3t1`. hi846 is already upstream.

## 3. Sensor support added (patch `0001-a6l-imx576-s5k3t1-sensor-support.patch`)

| | imx576 / imx576_a6l | s5k3t1 |
|---|---|---|
| unit cell | 0.9 µm (5760×4312 native, 1/2.8") | 0.8 µm (5184×3880 native) |
| gain model | SMIA `1024/(1024-code)`, code 0..960 (1..16x) → `AnalogueGainLinear{0,1024,-1,1024}` | `code/32`, 32..512 (1..16x) → `{1,0,0,32}` |
| black level | 64 @10 bit (4096 @16 bit) | 64 @10 bit |
| test pattern | V4L2 menu 0 off, 1 solid, 2 bars, 3 fade, 4 PN9 (CCS 0x0600) | same |
| sensor delays | exposure/gain/vblank/hblank = 2 frames (conservative, no grouped hold) | same |
| tuning | `imx576_a6l.yaml`: BlackLevel, Awb, Adjust, Agc (**uncalibrated**, CCM off) | `s5k3t1.yaml` (same) |

Other patches:
- `0002`: bionic has no `pthread_setaffinity_np`, so the patch uses `sched_setaffinity(pthread_gettid_np())`.
- `0003`: `apps/common` misses the libevent include path when libevent is not in a system path.

Both are upstreamable.

**CCM and LSC come later.** Chromatix decode is still open (`docs/cam-20240924.md` §5). The fallback is a colour-checker shot, fitted with libcamera's `utils/tuning` (simple IPA CCM per CT).

## 4. Autofocus

- **Kernel:** the VCM works as a subdev (`MODE=focus` sweep).
- **libcamera:** the simple pipeline has **no lens/AF support** in 0.7.2: it does not open the `lens-focus` ancillary subdev, and the simple IPA has no AF algorithm.

Plan:
1. **Phase 1-2:** fixed focus. Set `FOCUS_ABSOLUTE` to the stock hyperfocal DAC from userspace (the provider init) or by a gt9769 module parameter.
2. **Phase 3:** a libcamera patch.
   - The simple pipeline finds `CameraLens` through the sensor's ancillary link, the way rkisp1 and ipu3 do.
   - The IPA gets a contrast-AF algorithm: the SoftISP stats gain a per-window sharpness sum (sum of |G(x)-G(x+2)| in the debayer pass), plus a hill-climb in `algorithms/af.cpp`.
   - It exposes `AfMode`/`AfTrigger`/`LensPosition`, which the Android adapter already maps to `CONTROL_AF_*`.
3. **Later:** tap-to-focus (AfWindows).

## 5. Mounting and rotation

The camera IDs are the DT paths. They are in `device/hisense/a6l/camera/camera_hal.yaml`, which the attended `cam --list` confirms.

| camera | DT path | location | rotation | source |
|---|---|---|---|---|
| imx576 main | `/base/soc@0/cci@ca0c000/i2c-bus@0/camera@1a` | back | 90 | stock camera_config.xml MountAngle + stock DT |
| s5k3t1 | `…/i2c-bus@1/camera@2d` | front | 270 | same |
| hi846 wide | `…/i2c-bus@1/camera@20` | back | 90 | same |

The kernel sets `rotation`/`orientation` in `a6l-camera-v75.dtso`, so libcamera reads them as sensor properties and the yaml only needs to agree.

## 6. Torch with adjustable strength (Pierre's request)

- The LED is `/sys/class/leds/white:flash` (leds-qcom-flash, PM660L, channel 1).
- `max_brightness` = torch current steps up to `led-max-microamp`. The DT test value is 100 mA; stock is 500 mA maximum and 200 mA default.

Implementation:
- Fork `hardware/lineage/interfaces/camera/aidl` into `device/hisense/a6l/camera/provider`, or carry it as a patch.
  - For the back camera: `setTorchMode(on)` writes `brightness` (the default level, or off).
  - `turnOnTorchWithStrengthLevel(n)` writes `n`. `getTorchStrengthLevel()` returns the current level.
  - In `getCameraCharacteristics`, add `FLASH_INFO_AVAILABLE=1`, `FLASH_INFO_STRENGTH_MAXIMUM_LEVEL=max_brightness` and `FLASH_INFO_STRENGTH_DEFAULT_LEVEL`.
  - Send torch status callbacks.
- The Lineage QS torch-strength tile then works.
- SELinux: `hal_camera_default` needs write access to `sysfs_leds` (a genfscon for the flash LED).
- Raise the DT `led-max-microamp` to 200-300 mA after an attended thermal check. That is a DT change.
- Still-capture flash (pre-flash + main flash through `flash_brightness`/`flash_strobe`) comes later, in the provider or HAL, driven by AE precapture.

## 7. Milestones (agent effort + attended points)

| # | milestone | agent | attended test point |
|---|---|---|---|
| M1 | **libcam1: `cam` in recovery**, list + capture both sensors (this bundle) | done | **A1:** `cam --list` shows 2 (3) cameras; 10 frames per sensor; PNG sane; fps; EXPBUF/dma-heap OK; tells whether the simple pipeline walks csiphy→csid→ispif→rdi |
| M2 | Fix whatever A1 shows (media-graph walk through ISPIF, format/size negotiation, controls); smaller sensor modes for preview speed (imx576 1440×1078 / s5k3t1 binned from stock res tables); SoftISP GPU debayer build (`softisp-gpu`, EGL on freedreno) and benchmark | 3-5 d | **A2:** fps per mode CPU vs GPU, AGC/AWB convergence on a real scene |
| M3 | Android HAL: `-Dandroid=enabled` prebuilt, `camera.libcamera.so`, DmaBufAllocator patch for internal buffers, vendor packaging, camera_hal.yaml, SELinux (`hal_camera_default`: video/media/v4l-subdev/dma_heap/udmabuf), ueventd perms, `ro.hardware.camera=libcamera`, Lineage AIDL provider in PRODUCT_PACKAGES, `persist.vendor.a6l.camera=1` | 1-1.5 wk | **A3:** ROM boot, `dumpsys media.camera` lists the cameras, Aperture preview + JPEG front/rear |
| M4 | Torch + strength (§6), fixed focus position | 3-5 d | **A4:** QS torch on/off/strength, current/thermal check |
| M5 | Contrast AF (§4), tuning (BLC measured, CCM, LSC-lite) | 1.5-2 wk | **A5:** focus sweep near/far, colour chart shots |
| M6 | Video: YUV output (SoftISP NV12 or libyuv post-proc), media_profiles, hi846 as the third camera (after its stream fix) | 1-2 wk | **A6:** 720p/1080p recording |

**Realistic timeline:**
- M1-M4 give a usable camera: preview, photos front/rear, torch with strength, fixed focus. About **3-4 weeks** of agent work plus 4 attended sessions.
- AF, tuning and video take another 3-4 weeks.

**Risks:**
- CPU SoftISP throughput on 4×Kryo-260: expect about 3-6 fps at 6 MP. Binned modes or the GPU debayer are mandatory for preview.
- The simple pipeline's graph walk may pick the VFE PIX node, or fail at ISPIF.
- The gralloc0 dependency in the HAL.
- The Lineage provider wrapper is written against camera3 device API 3.x; the libcamera HAL reports `CAMERA_DEVICE_API_VERSION_3_3`. Check this at M3.

## 8. Attended test A1 (libcam1; fresh V75-usb recovery boot; about 5 min; after or together with camera15)

**Prerequisites:**
- `v75/camera15` is on the laptop and loads the rom1 modules.
- `v75/libcam1` is the bundle (sha `SHA256SUMS` = `3be2565776b9add8557a61065143ea622e367ca5435c7ea216ca82c43ba8b0cf`).

```
A="adb -s HLTE730T-PROBE"; cd ~/A6L-usb-20260915
$A shell 'rm -rf /tmp/camera15 /tmp/libcam1'; $A push v75/camera15 /tmp/camera15; $A push v75/libcam1 /tmp/libcam1
$A shell 'T=/system/bin/toybox; $T mkdir -p /tmp/bin; for a in $($T); do $T ln -sf $T /tmp/bin/$a; done; chmod -R 755 /tmp/camera15 /tmp/libcam1'
mkdir -p v75/logs/t37; nohup setsid sh -c "adb -s HLTE730T-PROBE shell /tmp/bin/dmesg -w > v75/logs/t37/kmsg-live.txt 2>&1" >/dev/null 2>&1 &
# 1. load the camera modules (camera15 probe), then libcamera env check
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=probe sh /tmp/camera15/run-camera.sh 2>&1 | grep -v linker | grep -E "PASS|FAIL"'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=env sh /tmp/libcam1/run-libcam.sh 2>&1 | grep A6L_LC'
# 2. list cameras (IDs = DT paths; note which index is imx576 / s5k3t1)
$A shell 'export PATH=/tmp/bin:$PATH; MODE=list sh /tmp/libcam1/run-libcam.sh 2>&1 | grep -v linker'
# 3. the requested capture, directly:   (CAM index from step 2; superseded: libcam2 selects by name, §10)
$A shell 'export PATH=/tmp/bin:$PATH; cd /tmp; MODE=capture CAM=1 N=10 WM=3 sh /tmp/libcam1/run-libcam.sh 2>&1 | grep -v linker'
$A shell 'export PATH=/tmp/bin:$PATH; cd /tmp; MODE=capture CAM=2 N=10 WM=3 sh /tmp/libcam1/run-libcam.sh 2>&1 | grep -v linker'
#    (equivalent raw command:  LD_LIBRARY_PATH=/tmp/libcam1/lib /tmp/libcam1/bin/cam -c1 --capture=10 --file=/tmp/lc1/f-#.bin)
# 4. optional: smaller output (faster), test pattern, wm=6 comparison
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=1 SIZE=1440x1078 sh /tmp/libcam1/run-libcam.sh 2>&1 | grep A6L_LC'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=bars CAM=1 sh /tmp/libcam1/run-libcam.sh 2>&1 | grep A6L_LC'
# 5. collect
$A shell 'cd /tmp && tar cf lc1.tar lc1'; $A pull /tmp/lc1.tar v75/logs/t37/; tar xf v75/logs/t37/lc1.tar -C v75/logs/t37/
grep -m1 -oE "[0-9]+x[0-9]+-[A-Z0-9]+" v75/logs/t37/lc1/live-c1.txt    # e.g. 2880x2156-ABGR8888
python3 v75/libcam1/libcam_rgb_to_png.py v75/logs/t37/lc1/live-c1-000009.bin 2880 2156 ABGR8888 v75/logs/t37/live-c1.png
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=off sh /tmp/camera15/run-camera.sh'
```
(The `cam --file` names are `live-c1-<seq>.bin`; `ls v75/logs/t37/lc1`. If the frame stride ≠ width×4 the converter derives it from the file size.)

**PASS / how to read it:**
- **env:** `SHA_PASS 12 files`; `NODES media≥1 video≥4 subdev≥8 heaps=linux,cma system (or udmabuf=y)`; `CAM_VERSION v0.7.2…`; then `ENV_PASS`.
  - `ENV_MISSING /system/lib64/libc.so` means the recovery has no bionic userspace. **Fallback:** push bionic `linker64`, `libc.so`, `libm.so` and `libdl.so` from the ROM system image into `/tmp/libcam1/lib`, and run through `linker64` (about 1 h agent work). A static build is not possible because the IPA is `dlopen`ed.
- **list:** `LIST_PASS`, 2-3 cameras. `list.log` shows the simple pipeline matched `qcom-camss` and names the sensor entity.
  - `Mandatory V4L2 control` errors mean a driver gap.
  - "No sensor found" or a path to the PIX node means a graph-walk issue: M2.
- **capture:** `CAPTURE_live-c1_PASS frames=10`, fps printed, `ExposureTime`/`AnalogueGain` changing across frames (AGC running), PNG shows a real, roughly white-balanced image.
  - Orientation: a 90° rotated image is expected (the rotation is applied by Android, not libcamera).
- **Failure signatures to capture:** `DmaBufAllocator` errors (heap nodes), `EXPBUF`/`mmap` failures (vb2 dma-sg export), `IPAManager` signature/isolation messages, VFE timeouts in kmsg.

## 9. Files (no deletes)

**Repo `device/hisense/a6l/camera/libcamera/`:**
- `patches/0001-a6l-imx576-s5k3t1-sensor-support.patch`, `0002-base-thread-android-bionic-affinity.patch`, `0003-apps-common-use-libevent-include-path.patch`
- `data/imx576_a6l.yaml`, `data/s5k3t1.yaml`
- `tools/a6l_sensor_edit.py` (generates 0001), `tools/build-libcamera-a6l.sh` (steps `src openssl libevent libcamera bundle`), `tools/stage-libcam1-laptop.sh`, `tools/libcam_rgb_to_png.py`
- `run-libcam.sh` (phone side), `a6l-android-aarch64.cross.generated`

**Other locations:**
- **Build dir (WSL):** `/home/a6l/libcamera-work` (venv with meson 1.12.1/ninja/cmake, `libcamera-src`, `src-a6l` with the patches as commits, `sysroot` with OpenSSL 3.3.2 and libevent 2.1.12, `build-a6l`, `bundle`).
- **Bundle:** `firmware/extracted/libcamera-20260929/` = laptop `v75/libcam1`.
- **libcam2 bundle (§10):** `firmware/extracted/libcamera-20260929b/` = laptop `v75/libcam2` (SHA256SUMS `03a1392b6e037691e4d62070482e26d7d50cfceeb4c1ba8e3a75a1da8e2960b4`, bin/cam `dd034177…`, lib/libcamera.so `4e371b6f…`, ipa_soft_simple.so `cecc369f…`, run-libcam.sh `8993f6f7…`); build dirs `src-lc2`/`build-lc2`/`stage-lc2`/`bundle-lc2`.

| file | sha256 |
|---|---|
| bin/cam | `3e805463b7b8cc2b9ea9863f47954e92e63023f63968d8e4a1f4d8a66fa10262` |
| lib/libcamera.so | `b418fd163a98e2c316ce70bf81590fa092c61590f44473b03d218ad872180fe5` |
| SHA256SUMS | `3be2565776b9add8557a61065143ea622e367ca5435c7ea216ca82c43ba8b0cf` |

## 10. On-phone results (t37, libcam1) and the libcam2 fixes (29 Sep 2026)

Logs: `logs/t37/lc1.tar` (list, live-c1, live-c2), `logs/t37/kmsg-live2.txt` (A6L_LC markers, cma errors).

### 10.1 What t37 showed

| step | result |
|---|---|
| env | PASS: `SHA_PASS 12 files`, heaps `adsp-region default_cma_region reserved system`, `udmabuf=y`, `cam` v0.7.2+3 runs on the recovery bionic |
| list | PASS, 3 cameras. The simple pipeline walks `sensor → csiphyN → csid0 → ispif0 → vfe0_rdi0 → msm_vfe0_video0` for all three (no graph-walk work needed). IDs `…/i2c-bus@0/camera@1a` imx576 back, `…/i2c-bus@1/camera@2d` s5k3t1 front, `…/i2c-bus@1/camera@20` hi846 back |
| rear 1440x1078 | PASS, 10 frames, 30 fps steady state (90 fps for the first 3 queued), AGC/AWB running (ColourGains, ExposureTime change). Frame dark but properly processed |
| rear / front default size | FAIL: `cma: __cma_alloc_frozen: reserved: alloc failed, req-size: 3875 pages, ret: -16` |
| CAM=2 (second run) | opened **hi846**, not the front camera: no frames (hi846 does not stream yet) |
| hi846 | `Failed to create camera sensor helper for hi846`, `No sensor delays found`, fallback to `uncalibrated.yaml` |

Root causes:
1. **Heap choice.** Upstream `DmaBufAllocator` tries `linux,cma`, then `reserved`, then `system`. On kernel 7.2 the default
   CMA area is exported twice, as `default_cma_region` (the heap name since 6.17) and under its CMA-area name `reserved`
   (`kernel/dma/contiguous.c` names the area "reserved"). There is no `linux,cma` (CONFIG_DMABUF_HEAPS_CMA_LEGACY absent),
   so libcamera picked `reserved`: the **32 MB** default CMA area (`CONFIG_CMA_SIZE_MBYTES=32` in both v67 and r5, no `cma=`
   on the cmdline). One 2296x1728 ABGR8888 buffer is 15.9 MB (3875 pages, the front camera, first run); 2880x2156 is 24.8 MB.
   The manual `linux,cma` alias pointed at the same 32 MB area, so it failed after 3 buffers too.
   The SoftISP output buffers are CPU-written; they need no contiguous memory. The CSI input buffers are vb2 dma-sg (no CMA).
2. **Camera index order is not stable.** `cam --list` indexes follow sensor probe order (run 1: imx576, s5k3t1, hi846;
   run 2: imx576, hi846, s5k3t1). Only the IDs (DT paths) are stable.
3. **SoftISP sizes.** The CPU debayer does **not scale**: the output is at most `(W-8) x H` of the sensor mode (RAW10 CSI2P
   pattern 4x2) and a smaller size is a **centre crop** of the smallest sensor mode that covers it. The 1440x1078 PASS was
   a 2x crop of the 2880x1620 mode. Full-FOV sizes: rear 2872x2156 (4:3 binned mode) or 2872x1620 (16:9 binned),
   front 2296x1728. A downscaled preview needs SoftISP scaling (the GPU debayer, or a 2x2 bin in the CPU debayer): M2 work.

### 10.2 Fixes (libcamera, `device/hisense/a6l/camera/libcamera/`)

| patch / file | change |
|---|---|
| `patches/0006-a6l-hi846-sensor-helper-delays-tuning.patch` | `CameraSensorHelperHi846`: gain = 1 + code/16 (register 0x0077, 0..240 = 1x..16x), black level 64 @ 10 bit; hi846 `sensorDelays` 2/2/2/2 (as imx576/s5k3t1); `hi846.yaml` (uncalibrated: BlackLevel, Awb, Adjust, Agc) in `src/ipa/simple/data` + `data/hi846.yaml` |
| `patches/0007-dma_buf_allocator-a6l-system-heap-first.patch` | provider order **system → linux,cma → default_cma_region → reserved → udmabuf** (callers that ask only for CMA are unaffected); `LIBCAMERA_DMA_HEAP=<heap>|udmabuf|<path>` forces a provider (e.g. `HEAP=default_cma_region` to reproduce t37). Applies on top of 0005 (its EACCES → O_RDONLY retry stays) |
| `tools/a6l_lc2_fixes_edit.py` | generator for 0006/0007 (idempotent) |
| `run-libcam.sh` | no awk (`wc -c`); `CAM=rear|front|wide` (or sensor name, full ID, index) resolved to the **camera ID** from `cam --list`, passed as `cam -c <id>`; file tags by name (`live-rear`, `live-front`); prints the output/input config and the heap used; `HEAP=` passthrough; `MODE=suite` (rear full/16:9/crop, front, wide) ; default prefix `/tmp/libcam2`, output `/tmp/lc2` |
| `tools/build-libcamera-a6l.sh` | `PREFIX`, `SRCD`, `BLD`, `STG`, `BUN` overrides so the recovery bundle builds in its own dirs (`src-lc2`, `build-lc2`, `stage-lc2`, `bundle-lc2`) without touching the HAL build (`src-a6l`, `build-hal`). Defaults unchanged |
| `tools/stage-libcam2-laptop.sh` | laptop staging for libcam2 |

Build: `PREFIX=/tmp/libcam2 SRCD=src-lc2 BLD=build-lc2 STG=stage-lc2 BUN=/home/a6l/libcamera-work/bundle-lc2 build-libcamera-a6l.sh src libcamera bundle`
(v0.7.2 + 7 patches, IPA re-signed and verified). The binaries contain the new heap table and the hi846 helper.

**Phase 2 (HAL/ROM) follow-ups, not done here so as not to race the phase-2 worker:**
- The next `src hal halpkg` picks up 0006/0007 automatically (`git apply $R/patches/*.patch`). Both apply cleanly after 0001-0005.
- After that halpkg, `prebuilt/share/libcamera/ipa/simple/hi846.yaml` exists: add its `PRODUCT_COPY_FILES` line to
  `libcamera-vendor.mk` (not added now: the prebuilt file does not exist yet and the copy would break the build).

### 10.3 ROM side: heaps, ueventd, SELinux, CMA (documentation only, no file changed)

- **Heap used by the HAL:** with 0007 the libcamera HAL (and the SoftISP inside it) uses `/dev/dma_heap/system`.
  - **ueventd:** `system/core/rootdir/ueventd.rc` already has `/dev/dma_heap/system 0444 system system` (and `system-uncached`,
    `/dev/udmabuf 0666`). 0005 opens the heap `O_RDONLY` on EACCES (the allocation ioctl needs no write access). **No vendor
    ueventd line is needed.** `default_cma_region`/`reserved`/`adsp-region` stay `0600 root` on purpose: the camera must not
    drain the 32 MB CMA the display/GPU/remoteprocs share.
  - **SELinux:** `/dev/dma_heap/system` is `dmabuf_system_heap_device`; AOSP `private/hal_camera.te:15` already allows
    `hal_camera dmabuf_system_heap_device:chr_file r_file_perms` (includes ioctl), and `domain.te` allows the
    `dmabuf_heap_device` dir. **No vendor rule is needed.** Only if a CMA heap were ever forced (`LIBCAMERA_DMA_HEAP`), it
    would need a ueventd line + a `dmabuf_heap_device` allow for `hal_camera_default`; do not do that.
  - Android output buffers are gralloc (minigbm, system heap) anyway; only the SoftISP's own buffers (standalone `cam`) and
    the HAL-internal buffers (0005 `generic_frame_buffer_allocator`) come from `DmaBufAllocator`.
- **CMA size:** keep 32 MB (`CONFIG_CMA_SIZE_MBYTES=32`, both kernels, no `cma=`). The camera no longer uses CMA; qcom-camss
  uses vb2 dma-sg. If a CMA heap is ever needed for the camera (e.g. a future contiguous-only consumer), add `cma=128M` to the
  ROM cmdline rather than growing the Kconfig default; 4 full-FOV RGBA buffers need ~100 MB, a 5760x4312 RGBA buffer is 99 MB.
- **Memory note:** 4 SoftISP buffers at 2872x2156 ABGR8888 = 99 MB of system-heap pages (fine on this device). The full
  5760x4312 mode would need ~400 MB: not a preview mode; use it only for single stills later (M3+).

### 10.4 Attended test A1b (libcam2; fresh V75-usb recovery; about 5 min)

```
A="adb -s HLTE730T-PROBE"; cd ~/A6L-usb-20260915
(cd v75/libcam2 && sha256sum -c --quiet SHA256SUMS && echo LIBCAM2_SHA_OK)     # SHA256SUMS = 03a1392b6e037691e4d62070482e26d7d50cfceeb4c1ba8e3a75a1da8e2960b4
$A shell 'rm -rf /tmp/camera15 /tmp/libcam2 /tmp/lc2'; $A push v75/camera15 /tmp/camera15; $A push v75/libcam2 /tmp/libcam2
$A shell 'T=/system/bin/toybox; $T mkdir -p /tmp/bin; for a in $($T); do $T ln -sf $T /tmp/bin/$a; done; chmod -R 755 /tmp/camera15 /tmp/libcam2'
mkdir -p v75/logs/t38; nohup setsid sh -c "adb -s HLTE730T-PROBE shell /tmp/bin/dmesg -w > v75/logs/t38/kmsg-live.txt 2>&1" >/dev/null 2>&1 &
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=probe sh /tmp/camera15/run-camera.sh 2>&1 | grep -v linker | grep -E "PASS|FAIL"'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=env sh /tmp/libcam2/run-libcam.sh 2>&1 | grep A6L_LC'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=list sh /tmp/libcam2/run-libcam.sh 2>&1 | grep -E "A6L_LC|DmaBuf"'      # LIST_ID rear/front/wide = the three IDs
# 1. rear full FOV (4:3 binned mode, 2872x2156), heap must say dma_heap/system
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=rear SIZE=2872x2156 N=10 sh /tmp/libcam2/run-libcam.sh 2>&1 | grep -v linker'
# 2. front (s5k3t1, 2296x1728 = its full FOV)
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=front SIZE=2296x1728 N=10 sh /tmp/libcam2/run-libcam.sh 2>&1 | grep -v linker'
# 3. preview-type sizes: rear 16:9 full FOV, rear crop (t37 reference), front default
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=rear SIZE=2872x1620 sh /tmp/libcam2/run-libcam.sh 2>&1 | grep A6L_LC'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=rear SIZE=1440x1078 sh /tmp/libcam2/run-libcam.sh 2>&1 | grep A6L_LC'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=front sh /tmp/libcam2/run-libcam.sh 2>&1 | grep A6L_LC'
# 4. optional: all of the above + a 3-frame hi846 attempt in one go, and a CMA reproduction of t37
$A shell 'export PATH=/tmp/bin:$PATH; MODE=suite sh /tmp/libcam2/run-libcam.sh 2>&1 | grep A6L_LC'
$A shell 'export PATH=/tmp/bin:$PATH; MODE=capture CAM=rear SIZE=2872x2156 HEAP=default_cma_region TAG=cma-rear sh /tmp/libcam2/run-libcam.sh 2>&1 | grep A6L_LC'
# 5. collect + convert (file names: <tag>-cam0-stream0-<seq>.bin)
$A shell 'cd /tmp && tar cf lc2.tar lc2'; $A pull /tmp/lc2.tar v75/logs/t38/; tar xf v75/logs/t38/lc2.tar -C v75/logs/t38/
for t in live-rear:2872:2156 live-front:2296:1728; do n=${t%%:*}; f=$(ls v75/logs/t38/lc2/$n-*.bin | tail -1); w=$(echo $t | cut -d: -f2); h=$(echo $t | cut -d: -f3); python3 v75/libcam2/libcam_rgb_to_png.py $f $w $h ABGR8888 v75/logs/t38/$n.png; done
$A shell 'export PATH=/tmp/bin:$PATH; D=/tmp/camera15 MODE=off sh /tmp/camera15/run-camera.sh'
```
(If `MODE=suite` ran instead of steps 1-3, the tags are `rear-full`, `rear-169`, `rear-crop`, `front-full`, `front-default`, `wide`.)

**PASS:** `CAPTURE_live-rear_PASS` and `CAPTURE_live-front_PASS` with `out=2872x2156-ABGR8888 in=2880x2156-RGGB-10-CSI2P heap=dma_heap/system`
(front: `out=2296x1728-ABGR8888 in=2304x1728-GRBG-10-CSI2P`), 10 frames, fps printed, no `cma: … alloc failed` line; the
PNGs show the full field of view (rear rotated 90°, front 270°), AGC/AWB converging. `list.log` shows no hi846 helper/delay
warning and `Using tuning file …/hi846.yaml`. The `HEAP=default_cma_region` run is expected to FAIL (t37 reproduction).
hi846 (`wide`) is expected to produce no frames until its stream fix (M6).

## 11. Phase 2 (lc2, 29 Sep 2026): ROM integration of the HAL and the provider, offline only

No phone, no adb, no image build.
- The Lineage tree was idle, but its `out/soong/.temp` is root-owned from an earlier run. `m <module>` therefore stops at once (`chmod out/soong/.temp: operation not permitted`).
- The Soong modules are validated by the compile checks below. The first real Soong pass over them is the next `tools/rom-v2-pipeline.sh <tag> prep build`.
- The camera copy used for the attempt was removed from the tree again.

### 11.1 What is integrated

**libcamera HAL build (`tools/build-libcamera-a6l.sh src hal halpkg`):**
- Built from `src-a6l`, which is v0.7.2 + **all 7 patches**:
  - `0004`: SoftISP NV12 output.
  - `0005`: single processed stream, YUV crop, dma-buf internal allocator with the O_RDONLY retry.
  - `0006`/`0007` from the libcam2 worker: hi846 helper, and the system heap first.
- Build options: meson + NDK r27c, `--prefix=/vendor --libdir=lib64`, `-Dandroid=enabled -Dandroid_platform=generic`. libjpeg-turbo, libexif, libyuv and libc++ are static. `NEEDED` is only libc, libm, libdl and the libcamera libraries.
- `halpkg`:
  - strips the libraries, then **re-signs** the IPA;
  - verifies the signature (`IPA_SIGN_OK`) and checks that the public key is embedded in `libcamera.so` (`IPA_PUBKEY_MATCH`);
  - so the IPA runs **in-process**: no IPA proxy, no isolation, no exec rule, and `/vendor/libexec/libcamera` is not shipped.
- `halpkg` fix: it strips in `/home/a6l/libcamera-work/halpkg` (llvm-strip cannot rewrite files on /mnt/c) and copies the unversioned Android sonames.
- Output in `device/hisense/a6l/camera/libcamera/prebuilt/` (8.6 MB), recorded in `SHA256SUMS`:
  - `camera.libcamera.so` `0d5b2275…` (`HMI` exported)
  - `libcamera.so` `62478e88…`
  - `ipa_soft_simple.so` `3dca7a90…` (`.sign` `bc7fc35a…`)

**Packaging:**
- `camera/libcamera/Android.bp`: `cc_prebuilt_library_shared` modules `libcamera-base.a6l`, `libcamera.a6l`, `ipa_soft_simple.a6l` (`lib64/libcamera/ipa`) and `camera.libcamera.a6l` (`lib64/hw/camera.libcamera.so`).
  - Each has `stem` = the real file name, so no clash with `external/libcamera`.
  - `strip: none`, because the signature covers the exact bytes. `check_elf_files: false`.
- `libcamera-vendor.mk` installs:
  - `ipa_soft_simple.so.sign`
  - the tuning files `/vendor/share/libcamera/ipa/simple/{imx576_a6l,s5k3t1,hi846,uncalibrated}.yaml`
  - `/vendor/etc/libcamera/camera_hal.yaml`
  - `ro.hardware.camera=libcamera`
- **`camera_hal.yaml` lists only imx576 (back, 90) and s5k3t1 (front, 270).** The HAL skips an internal camera that has no entry. hi846 does not stream yet. libcamera IDs follow sensor probe order (t37), so a listed hi846 could become Android camera 0, the default back camera, and the torch owner. Uncomment its entry once it streams.

**Provider (`camera/provider/`):**
- A fork of `hardware/lineage/interfaces/camera/aidl`: `android.hardware.camera.provider-service.a6l` plus `camera.device-impl.a6l`, AIDL `ICameraProvider/internal/0` V1, 64-bit only.
- **A6lTorch** (`device/A6lTorch.{h,cpp}`) drives `/sys/class/leds/white:flash`:
  - Strength levels are 5 mA steps. MAXIMUM_LEVEL = `led-max-microamp` / 5 mA, read from the LED's DT node through `device/of_node`, with a 100 mA fallback.
  - DEFAULT_LEVEL = 100 mA (level 20). Brightness = ceil(level·5 mA·255 / max), so each level lands on its own 5 mA step.
  - The torch belongs to the first BACK camera.
  - Its characteristics gain `FLASH_INFO_AVAILABLE`, `FLASH_INFO_STRENGTH_MAXIMUM_LEVEL` and `FLASH_INFO_STRENGTH_DEFAULT_LEVEL`, also added to the characteristics keys.
  - It implements `setTorchMode`, `turnOnTorchWithStrengthLevel` and `getTorchStrengthLevel`, with AVAILABLE_ON/OFF status callbacks.
  - **The torch works with no camera open.** Opening the torch camera turns it off; while that session is open, calls return CAMERA_IN_USE. Turning the torch off resets the level to the default.
- **DT:** `kernel/a6l-flash-v75.dtso` `led-max-microamp` 100 → **500 mA**, the stock torch maximum, which gives 100 levels. The default tap stays 100 mA.
- **rc:** the service `vendor.camera.provider` (user cameraserver, group camera) is `disabled` and starts `on property:init.svc.a6l_modules_misc=stopped`. libcamera has no hotplug without udev, so the camera modules and the LED must be loaded first. The rc sets `LIBCAMERA_LOG_FILE=syslog` (logcat) and `LIBCAMERA_LOG_LEVELS=*:INFO`.

**Kernel:**
- The qcom-camss rom1 patch defaults to **`a6l_wm=3`** (zero-copy; camera15: wm 3 and wm 0 dequeued 9/9).
- Rebuilt for v67 (`4d1dc227…`) and r5 (`43c5d6ca…`). Both are staged, and `modinfo` shows "default 3".

**Product:**
- `camera/camera.mk` is inherited by `rom/rom.mk`. It adds the packages above plus:
  - `a6l-camera-features.xml`: camera, camera.any, camera.front, camera.flash. **camera.autofocus is not declared.**
  - `media_profiles_V1_0.xml`: minimal, 480p/720p h264 at 15 fps.
- `rom.mk`: **`persist.vendor.a6l.camera=1`** (was 0), so the camera stack loads in the misc group.
- `rom/vendor-etc/a6l-unavailable-features.xml` withdraws only camera.autofocus now.
- `tools/rom-v2-pipeline.sh` syncs `camera/` into the tree and checks the five camera files after a build.

**Device nodes:**
- `rom/vendor-etc/ueventd.rc`: `/dev/video*`, `/dev/media*` and `/dev/v4l-subdev*` are 0660 system:camera. Without these the nodes are root 0600. The heaps need no line (§10.3).

**SELinux (`camera/sepolicy/vendor`, added to `A6L_SEPOLICY_DIRS`):**
- file_contexts: the provider binary → `hal_camera_default_exec`; `/dev/media*` and `/dev/v4l-subdev*` → `video_device`.
- `hal_camera_default` gets read access to sysfs (the libcamera enumerator and the DT node), `sysfs_leds` and the dma_heap dir.
- `rom/sepolicy/vendor/hal_extras.te` adds `sysfs_a6l_flash:lnk_file read`.

### 11.2 Checks (all PASS)

| check | result |
|---|---|
| `camera/provider/tests/check-provider-syntax.sh` (tree clang r584948, bionic/libc++, frozen V1 camera AIDL NDK headers generated with the tree's aidl, `-Wall -Werror`) | A6L_PROVIDER_SYNTAX PASS (6/6) |
| `camera/provider/tests/test_a6l_torch.cpp` (host; level → brightness → kernel current at 100/200/300/500 mA) | A6L_TORCH_TEST PASS |
| `check-a6l-sepolicy.sh user rom/sepolicy/vendor`, and `user`/`userdebug` `+ camera/sepolicy/vendor` | A6L_SEPOLICY_CHECK PASS |
| `rom/tests/test-rom-static.sh` / `rom/r6/tests/test-r6-static.sh` | PASS / PASS 0 |
| `A6L_STAGE_DRYRUN=/tmp/stage-lc2` and `A6L_KERNEL=r5 … /tmp/stage-lc2r5` | STAGE_ROM_V2_PREBUILTS_PASS (qcom-camss default 3) |
| `A6L_KSERIES_DIR=/tmp/kseries-lc2 check-rom-v2-kernel-series.sh` | A6L_KSERIES_PASS |
| `build-rom-v2-dt.sh` (flash led-0 `led-max-microamp = <0x7a120>`) | A6L_V75DT_BUILD_PASS |

### 11.3 First installed-ROM attended tests (A3/A4)

1. **Boot and enumeration.**
   - `getprop init.svc.vendor.camera.provider` → running.
   - `logcat -b all -d | grep -iE "CamPrvdr|A6lTorch|libcamera|HAL|IPAManager"` shows `Loaded "libcamera camera HALv3 module"`, 2 cameras, and `torch LED /sys/class/leds/white:flash: … led-max-microamp 500000 -> 100 levels, default 20`.
   - `dumpsys media.camera` shows camera 0/1 with facing and orientation, and flash + strength on the back camera.
   - Capture `ls -lZ /dev/video* /dev/media* /dev/v4l-subdev*`, `readlink -f /sys/class/leds/white:flash` + `ls -lZ` (the flash genfs path is UNVERIFIED), and `dmesg | grep avc`.
2. **Camera app (Aperture), rear:**
   - preview, then a photo, then open the JPEG in the gallery;
   - note the fps, orientation and colour (the first frames may be dark or green while AGC/AWB settle);
   - fixed focus, no AF.
3. **Camera app, front:** preview and a photo.
4. **Torch QS tile:**
   - on/off with no camera open;
   - long-press strength slider: min, default and max;
   - at max (500 mA), check LED/battery current and **temperature** over 1-2 min;
   - with the camera app open, the torch is expected to be unavailable.
5. **Failure signatures:**
   - `Failed to map mandatory Android format` (0004);
   - `configureStreams` errors (0005 merge path; for comparison, `setenv LIBCAMERA_HAL_MULTI_STREAM 1` in the provider rc restores the upstream path);
   - IPAManager signature/isolation lines;
   - `DmaBufAllocator` errors;
   - VFE timeouts in kmsg;
   - SoftISP fps at full-FOV sizes (CPU debayer, no scaling: §10.1.3).

**Open items:**
- hi846 (commented out of `camera_hal.yaml`).
- Autofocus (M5).
- Still-capture flash: FLASH_MODE is ignored; only the torch works.
- Video (M6; the NV12 path exists but is untested).
- Scaled preview (M2).

