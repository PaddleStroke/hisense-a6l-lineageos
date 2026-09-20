# Path to a working (installable) image — plan after the first hardware boot, 20 September 2026

State: framework boots on hardware from RAM (welcome screen + touch), ADSP starts, e-ink NOR captured.
Blocking usability: performance. Blocking installability: everything still runs from a RAM test harness.

## 1. Performance (first)
- **GPU (highest value).** Kernel side = candidate G1 (`a6l-gpu.dtso`, `msm.ko` + stock a512 zap/pm4/pfp firmware).
  Userspace = Mesa freedreno: `tools/build-mesa-freedreno.sh` now cross-builds `libgallium_dri.so`, `libEGL.so`,
  `libGLESv1_CM.so`, `libGLESv2.so` (Android platform, NDK r27c, libdrm static) — **built, untested**.
  Still to do: install as `libEGL_mesa/libGLESv*_mesa` with `ro.hardware.egl=mesa`, minigbm `msm` backend for
  allocation while scanout stays on simpledrm (prime import), `debug.hwui.renderer=skiagl` (no Vulkan yet; turnip
  needs glslang on the build host). Attended test: load msm.ko, check `/dev/dri/renderD128`, run a GLES probe.
- **CPU frequency.** The pinned sdm660-mainline tree has **no CPU clock/cpufreq driver** (SDM660 needs the OSM + CPR3
  work that never went upstream). Cores stay at the bootloader frequency. Next attended session: measure that
  frequency (timed loop vs stock) to size the problem. Porting OSM/CPR is a large, risky (voltage) task: only after GPU.
- Remove debug overhead from the recovery cmdline for framework runs (`initcall_debug`, `loglevel=8`, 2 s probe logger).

## 2. Harness → real boot flow
- Fold today's live fixes into the payload (input node modes, writable backlight, cmd/input/settings, screen timeout,
  simpledrm power-off handling, per-run cleanup so a recovery reboot is not needed).
- Replace the supervisor with real Android `init` + rc files + fstab, SELinux still permissive, data on tmpfs first.
- Then persistent storage. **Decision needed from Pierre before this step:** where the system lives on the spare
  (overwrite stock `system`/`vendor`/`userdata` vs. keep stock and use only `userdata`/`cache`). Until then RAM only.

## 3. Hardware enablement, in dependency order
ADSP (done: starts) → pd-mapper/APR → audio codec routes; SMGR sensors. Modem M1 + rmtfs (RAM copies) → Wi-Fi
(ath10k/WCN3990) → GNSS; Bluetooth. E-ink: real drive frames offline from the captured waveform → E1 (TPS65185 +
rear touch) → DSI1/bridge transport → first refresh. Front ALS S1. Charging/thermal/suspend. Cameras, fingerprint last.

## 4. Release engineering
Device tree proper (HAL manifests, VINTF), enforcing SELinux, signing, recovery/OTA, regression matrix.
