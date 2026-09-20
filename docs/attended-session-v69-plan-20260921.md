# Attended session plan — V69 (prepared 20 September evening; nothing here has run on the phone)

Installed today: V68. Candidate for tomorrow: **V69** `recovery-v69-candidate-20260921`, SHA-256
`4dc4861ebcbd30b5f38ab236b3bdb37e57140803dca1b993d4749808c2815eec` = V68 (same kernel, same ramdisk) + four DT overlays:
G1 Adreno 512 · E1 TPS65185 e-ink PMIC + rear touch (i2c2/i2c7) · S1 front ALS/proximity (i2c6) · M1 modem + WCN3990 Wi-Fi.
Every new driver is a **module loaded only by the scripts below**; at boot the only new activity is three I²C controllers
probing and RPM regulator nodes being registered (no consumer enabled). Checks passed: 109-property DT allowlist with
value assertions, captured-ABL emulation, transition (accepts only V68) + protocol tests, 39/39 bundle modules load on the
V67 kernel in QEMU. Staged and hash-verified on the laptop (`Stage-V69.py`: 123 pins).

Fallback at any point: reinstall V68 (known good) with `Run-LaptopDiagnosticRestore`-style flow / V68 tools.

## Order (each step = push `v69/bundle/<area>` to `/tmp/<area>`, `D=/tmp/<area> sh run.sh`; create `/tmp/bin` toybox links first)
0. Stock Android: run `cpu-speed.sh` (reference numbers). Install V69, confirm stock still boots.
1. Boot V69 recovery. **Baseline**: ADB, `insmod /sdhci-msm.ko`, recovery partition hash = V69, LCD text, keys. Stop if anything regresses.
2. `cpu-speed.sh` in recovery → ratio vs stock = how slow the fixed CPU clock is.
3. **touch**: expect 2 ft5x06 devices; Pierre taps the *rear* (e-ink side) glass while events are recorded.
4. **eink-pmic**: TPS65185 binds, temperature readable. Rails stay off.
5. **front-als**: raw lux/proximity values change when the sensor is covered.
6. **ADSP + sensors-adsp**: start ADSP (yesterday's PASS), keep it running, load SMGR modules → IIO accel/gyro/mag values.
7. **gpu**: load `msm.ko` with stock firmware → render node + GPU info, no SMMU faults.
8. **Framework run** with V72 payload (input perms, writable backlight, no screen timeout, shell tools, DRM nodes, Mesa libs
   included): first with default ANGLE (regression check, should now be hands-free), then — only if step 7 passed —
   a second fresh boot with `A6L_EGL=mesa` for the first GPU-rendered UI attempt. Each framework run needs a fresh recovery boot.
9. **modem-wifi — only if Pierre explicitly agrees at that moment** (RF-capable; decide SIM in/out). Uses RAM copies of the
   modem storage partitions. Expected: modem `running`, QMI services listed, ath10k reports chip/board id (tells us which
   stock `bdwlan.bXX` board file to use), maybe `wlan0`.
10. Back to stock; host cleanup.

Not prepared (needs design, not just bring-up): audio card DT/codec routing, Bluetooth, charging/thermal policy, cameras,
fingerprint/TEE, e-ink DSI1+bridge transport (format now known: 384×725 XRGB video @85 Hz, see eink-swtcon doc), cpufreq (OSM/CPR port).

## Second stage, same session if V69 went well: V70
`recovery-v70-candidate-20260921`, SHA-256 `b93771e701d677c58e377feae794a25d3a9c4c1f5cd379bb904910e4661a4373`
= V69 + four more overlays (same kernel/ramdisk; transition tool accepts only V69 as predecessor; captured-ABL check,
transition and protocol tests pass; 59/59 bundle modules load in QEMU):
- **D1 native display**: MDSS + DSI0 + generated FT8719 Tianma panel driver. When `msm.ko` loads it replaces the boot
  framebuffer. Success = the LCD stays alive under the real display driver (enables true screen off/on and is the
  prerequisite for e-ink). Failure mode = dark LCD until reboot; ADB unaffected. Known risk: enabling the MMSS SMMU node
  may disturb the bootloader splash/console at boot even before any module loads — if V70 boots with a dead LCD but ADB
  works, that is the cause (then reinstall V69).
- **E2 e-ink transport**: DSI1 → TC358762 (mainline bridge driver with the three stock register values) → 384×725@85 DPI
  panel. Test = binding + connector/mode only. No panel rails, no refresh yet.
- **A1 internal audio**: sound card registration through ADSP/APR/Q6 + LPASS digital + PM660L analog codec. No playback.
- **B1 Bluetooth** (RF-capable, needs `A6L_BT_APPROVED=1`): controller init + version read only.
Bundles: `v70-attended-bundle-20260921/{display,eink-dsi,audio,bluetooth}`. Order: baseline → display → eink-dsi →
(ADSP start) → audio → bluetooth if approved → framework run on the native display if D1 passed.

Framework payload for both stages: `framework-phone-bundle-v71-from-v72r2` (VM: 25/26, only harness teardown failing),
already copied to the laptop under `~/A6L-usb-20260915/v69/fw/`.
