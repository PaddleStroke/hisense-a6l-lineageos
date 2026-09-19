# A6L port checklist

Updated 19 September 2026, after physical V47 and offline V64. This is the current status index. Dated
reports are historical evidence; the newest entry in `resume-next-session.md`
records the exact installed image and any running operation.

**Current phone:** stock Android running; verified V46 diagnostic recovery installed.
V47 RAM test passed; stock return and host cleanup verified, physical logs archived.
Offline follow-up: V49/V50 pass ART, framework JNI, shared-memory and Binder checks
in QEMU. Genuine Zygote forks SystemServer and enters its initialization. Full
system-service and launcher startup remain incomplete. V50's separate kernel
enables ART's UFFD collector and removes the earlier read-barrier mismatch.
V51 r6 passes all ten runtime/APEX checks: 38 real packages mount read-only,
apexservice becomes ready and SystemServer passes PlatformCompat and reaches
ActivityManager/PowerManager. V52 r2 passes 12 checks, including real flag storage
and SystemSuspend registration; SystemServer completes display discovery and
reaches PackageManager, where missing installer application content stops it.
V53 r1 passes 13 foundation checks with the built system apps and real installd;
PackageManager initializes and SystemServer completes bootstrap services. It
stops in BatteryService because the Health HAL is absent. Full UI is not up.
**19 September update:** see [V57–V64 report](android-full-boot-v57-v64-20260919.md). In the VM, genuine
SystemServer now starts every service, completes boot phase 1000, unlocks user 0 and launches SystemUI,
FallbackHome and the LineageOS setup wizard (V64 r1, 22/23 checks; teardown check fails). Added on the way:
Health HAL, BPF loader, HintManager no-Power-HAL fix, vold, idmap2d, netd on a new Android-networking
runtime kernel (V59), audioserver + example AIDL audio HAL, gatekeeperd, keystore2 + software KeyMint,
init.rc data layout. Nothing of this has run on the phone yet.
**Target:** a usable modern LineageOS phone with both displays.
The full LineageOS interface has not booted on the phone. Linux 7.2.3 is the
working diagnostic kernel; this checklist does not claim it is the latest release.

Legend: **Tested** means the stated bounded operation passed on the spare;
**Partial** means some operations passed; **Prepared** means offline work exists;
**Open** means implementation or evidence is missing. No category is considered
production-complete merely because one test passed.

## Current status chart

| Area | Status | Demonstrated | Next missing checkpoint |
|---|---|---|---|
| Backup and recovery | Partial | Firmware/calibration backup; verified recovery writes and stock return | Separate e-ink SPI data; final recovery/update package |
| Kernel and boot | Tested baseline | Linux 7.2.3, RAM Android init, stable diagnostic boot | Production configuration, watchdog/restart and suspend |
| USB and ADB | Partial | Authenticated ADB and stable transfer | MTP, OTG/host, roles, reconnect and charging negotiation |
| Internal storage | Partial | eMMC enumeration, repeated firmware hashes, read-only ext4 | Writable Android data, encryption, sustained I/O, microSD |
| LCD output | Partial | Correct colours, scanout, real SurfaceFlinger rendering, brightness | Native panel initialization, screen-off/on, rotation, sleep/wake |
| GPU/video | Prepared | Software graphics rendering works | Adreno acceleration, GPU power, hardware video codecs |
| Front touch | Partial | V47 real Android input dispatch: 227 events, two fingers, four quadrant taps and visible finger-following marker | Screen edges, rotation, suspend and framework integration |
| Power/volume keys | Tested events | Correct press/release codes | Android actions and wakeup |
| E-ink side key | Tested events | V46: 52 presses/releases, code 616, IRQs advance with explicit stock pinctrl | Android action, wakeup and display switching |
| Vibration | Partial | Corrected brake parsing and bounded commands succeed; V46 pulse still not felt; stock PWM/current/amplitude discrepancies identified offline | Guarded register observations, reviewed PM660 configuration and perceptible output |
| Battery readings | Partial | Voltage, capacity, current, temperature, design capacity | Accuracy checks and Android Health service |
| Charging/thermal/sleep | Prepared | Stock policies/configuration identified; modules prepared | Charger/parallel-charger policy, limits, thermal handling, deep sleep |
| Rear e-ink display | Prepared | Stock reverse engineering; TCON version ABI; fixed waveform/VCOM read interface mapped | Privileged SPI window backup (not whole chip), power/transport, first static refresh |
| Rear touch/screen switching | Prepared | Stock controller and dimensions identified | New-kernel input, active-face routing and display switching |
| Wi-Fi | Prepared | Firmware and module/dependency bundle checked offline | Modem services, correct board data, link/data and Android Wi-Fi |
| Cellular/SIM | Prepared | Own modem firmware and memory map collected | MSS/RMTFS/QMI, registration, calls/SMS/IMS; IPA data path unresolved |
| Bluetooth | Prepared | UART and firmware identified; modules checked | Resolve IO supply, controller init, pairing/audio/suspend |
| GNSS | Prepared | Firmware/config and QMI LOC dependency identified | Modem LOC service, position fix and Android GNSS |
| Speaker/earpiece/headset/mics | Prepared | Firmware, routes and codec/amp identities; modules checked | ADSP/APR, codec routes, amplifier protection, playback/record/call audio |
| Motion/light/proximity sensors | Prepared | Stock inventory and module bundle | ADSP/SMGR, real readings, calibration, Android Sensors |
| Cameras/flash | Open | Stock artifacts available | Exact active sensors, power/ISP/calibration, preview and Android Camera |
| Fingerprint/security | Open | Stock wiring/HAL evidence | Sensor/TEE, enrollment, keystore, lockscreen and encryption |
| Android interface | Partial foundation | Phone native input/rendering; VM ART/APEX/native services, display discovery, PackageManager and SystemServer bootstrap | VM: WebView zygote, launcher idle, teardown; then assemble the same service set for a phone RAM boot |
| Installable release | Open | Reproducible diagnostic images with manifests | Full device build, enforcing SELinux, signing, recovery/OTA, regression |

## Next checkpoints, in order

- [ ] Repair missing diagnostic input nodes and printf alias in the reusable RAM setup.
- [ ] Fix touch capture initialization: lift/reapply after recording starts, or snapshot all slots; preserve raw events.
- [x] Observe stock e-ink key events: 13 complete press/release pairs, code 616.
- [x] Independent review and V46 explicit pinctrl image: scoped DT diff and captured bootloader checks passed.
- [x] Compare/correct PMIC pin configuration and retest the e-ink key: V46 52 complete pairs, IRQ count 103→211.
- [x] Identify and build a fix for the haptic brake-pattern cell/byte mismatch; 5/5 emulator ABI checks passed.
- [x] Physically test corrected haptic candidate: commands pass, user still feels no vibration.
- [ ] Review stock PM660 startup/resonance; physical vibration remains unresolved.
- [x] Connect front touch to an interactive Android rendering/input test: V47 software and user visual checks passed.
- [x] VM: full framework boot to SystemUI/setup wizard (V64, offline only).
- [ ] Phone: boot the same framework/service set from RAM on Linux 7.2.3 with the V59 networking configuration.
- [ ] Preserve the separate e-ink SPI data and pursue a controlled first refresh early.
- [ ] Bring up shared ADSP/modem services to unblock audio, sensors, Wi-Fi and GNSS.

## Detailed completion criteria

### Boot, storage and USB

- [x] Spare identified separately from everyday phone; bootloader unlocked.
- [x] Firmware/calibration partitions backed up; stock restore route exercised.
- [x] Modern kernel boots; eMMC permissions and USB failures resolved for diagnostic use.
- [x] Android first/second-stage init, SELinux loading, authenticated ADB and Binder tested.
- [x] Read-only firmware hashes and sampled system/vendor files match backups.
- [ ] Writable data and filesystem recovery; encryption and storage stress.
- [ ] SD card, MTP, USB host/OTG, cable reconnect and role switching.
- [ ] Reliable software restart/shutdown and production watchdog behavior.

### LCD, graphics and input

- [x] Visible colour bars/gradient and correct channel ordering.
- [x] Android buffer allocation/import, DRM presentation and real SurfaceFlinger composition.
- [x] User-confirmed graphics pattern and brightness changes/restoration.
- [x] Front touch binds; coordinate stream and two-slot updates observed.
- [x] Power and Volume Up/Down press/release events.
- [ ] Fully initialized multitouch tracking and all screen edges/rotation.
- [ ] Native panel reset/initialization, display blank/unblank and resume.
- [ ] Adreno GPU acceleration and memory/power management; hardware video.
- [x] Android input dispatch and touch-driven rendering: V47 real EventHub/InputReader/InputDispatcher/InputChannel.
- [ ] Density/orientation, SystemUI and launcher.
- [ ] E-ink key and reliable vibration; Android keylayout/haptic service.

### E-ink and second face

- [x] Stock library/kernel/framework evidence archived; basic TCON ABI emulated.
- [ ] Read and verify panel-specific SPI/VCOM/waveform data with known coverage.
- [ ] Establish exact TCON initialization/conversion ABI and buffer contracts.
- [ ] Power sequencing, bridge/DSI transport, first static panel refresh.
- [ ] Rear touchscreen, screen selection and inactive-face input rejection.
- [ ] Full/partial updates, ghosting control, temperature compensation, sleep/wake.
- [ ] Android display integration, app compatibility and refresh-mode UX.

### Power, radios and audio

- [x] Battery readings obtained under the new kernel.
- [x] Own firmware/configuration collected and module ABI bundles checked offline.
- [ ] Health/Thermal services, safe charging/parallel charger and powered-off charging.
- [ ] CPU/GPU idle/frequency, deep sleep, alarm wake and standby drain.
- [ ] ADSP/APR and modem/RMTFS/QRTR shared services.
- [ ] Wi-Fi scan/connect/data, Bluetooth init/pair/audio, GNSS fix.
- [ ] SIM/registration, voice/SMS/IMS, data path and airplane-mode recovery.
- [ ] Earpiece/headphones/microphones, speaker protection, routing and call audio.

### Remaining hardware and release

- [ ] Motion/light/proximity/Hall/capacitive sensors and Android sensor fusion.
- [ ] Cameras, autofocus/calibration, video, flash/torch.
- [ ] Fingerprint enrollment/authentication, TEE and trusted keystore.
- [ ] Full Lineage device build, HAL/VINTF integration and enforcing SELinux.
- [ ] Signing, installable recovery/package, OTA and rollback procedures.
- [ ] Reboot/suspend/thermal/network regression and daily-use acceptance.
- [ ] Publish reviewed sources and assess upstream LineageOS submission requirements.

## Evidence map and maintenance

- Latest physical results: [V45 report](combined-controls-v45-results-20260918.md).
- Current control fixes: [follow-up investigation](controls-followup-20260918.md).
- Every detailed dated report: this `docs` directory. Older statements such as
  “awaiting test” describe their date, not today's status.
- Raw phone/host evidence: `captures/`; V45 includes `analysis.json` and a SHA256 index.
- Original firmware, verified images, packages and emulator reports: `firmware/`.
- Stock investigation and online references: `research/` and the relevant dated report.
- Device sources: `device/hisense/a6l/`; repeatable build/test/install helpers: `tools/`.
- Operational state: [resume notes](resume-next-session.md). These are detailed working
  notes, not the user-facing completion checklist.

After each meaningful test, update this checklist, link the new report, record
the image/kernel and raw evidence, and distinguish command success from physical
confirmation. Do not erase failed tests or mark an entire component done from a
single successful operation. The separate e-ink SPI backup gap remains explicit.
