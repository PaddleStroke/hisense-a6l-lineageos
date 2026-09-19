# Remaining hardware and Android integration — 17 September 2026

Current evidence is a working diagnostic kernel, storage/USB/ADB and LCD presentation. V44 SurfaceFlinger composition passed QEMU and awaits the physical test. Touch, power, radio and audio preparation is offline; none of those preparations establishes a complete booted LineageOS phone.

## Complete bring-up checklist

| Area | Remaining tasks / present evidence |
|---|---|
| Rear e-ink display | Panel power sequencing, bridge/DSI transport, software timing controller, waveform selection, temperature compensation, full/partial updates, ghosting/contrast, display switching and sleep/wake. Stock implementation has been partially reverse-engineered. No new-kernel panel refresh yet. |
| Both touchscreens | Front FT8719 candidate/module prepared; rear touch separate. Android input mapping, active-screen routing, rotation, rejection on inactive face, suspend/wakeup and gestures still require tests. |
| LCD and GPU | Existing LCD tests use preserved boot display state/simpleDRM. Native panel initialization, brightness, display-off/on, Adreno acceleration, GPU power management, GLES/Vulkan and video decode/encode remain distinct work. SwiftShader success is software rendering. |
| Battery/charging/thermal | See power-sensors preparation. Telemetry, charger policy, parallel charger, full/low-battery behavior, thermal protection, powered-off charging and Android Health/Thermal services. |
| Wi-Fi and Bluetooth | Wi-Fi firmware/control/data and HAL; Bluetooth transport/firmware, pairing, reconnect, media/call audio and suspend. WCN3990 evidence exists; working Wi-Fi does not establish Bluetooth. |
| Cellular and GNSS | Modem startup/QMI/services, SIM/network registration, data path, calls/SMS/IMS and Android radio; GNSS firmware/control and Android GNSS service, location fixes and assisted operation. No call, SMS, emergency-call or RF-transmit tests during unattended preparation. |
| Audio | Speakers, earpiece, all microphones, capture/playback, headset plug/buttons/mic, speakerphone, Bluetooth/USB routing and call audio. Amplifier tuning and protection are separate from PCM output. |
| Cameras | Actual front/rear sensor identification, power/clock/reset, CSI/ISP, autofocus and calibration, preview/stills/video, flash/torch, Android camera provider. Stock camera HAL presence is not proof it can run on the new kernel. |
| Fingerprint / secure services | Stock DT has Sunwave fingerprint wiring and vendor HAL. Sensor communication alone does not establish enrollment/authentication; TrustZone, Keymaster/Gatekeeper integration and Android security policy also matter. |
| Motion/environment sensors | BMI160, AK09918, front/rear light/proximity, Hall and capacitive sensing; fusion, calibration, batching, timestamps and wakeup. See power-sensors preparation. |
| Buttons/haptics/indicators | Power/volume/screen-switch inputs; vibration amplitude/patterns and wakeup; LCD brightness and any populated indicator/lighting controls. Stock PM660 haptic and WLED nodes identified. Do not assume a generic DT light is physically populated. |
| Storage and USB | eMMC endurance/filesystem operations, microSD insertion/removal, filesystem permissions/encryption; MTP/ADB, host/OTG and USB role switching, accessories and charger negotiation. USB device ADB success covers only one mode. |
| System power | CPU/GPU frequency/idle policy, deep sleep, alarm wakeup, interrupted suspend, screen-off drain, restart/shutdown and watchdog recovery. Passing a short boot is insufficient for standby behavior. |

NFC is not assumed present: the stock `nfc-nci@28` node is disabled. Generic FM/ANT/NFC HALs or device-tree entries do not establish fitted, supported hardware. Confirm product requirements and actual enumeration before spending porting effort on optional features.

## Android work beyond individual drivers

- First full Android boot: init/mounts, writable data, SELinux policy, service dependencies, HAL declarations and VINTF compatibility.
- Graphics/input: WindowManager/SystemUI, display enumeration, compositor/allocator synchronization, physical display identities, density and touch association.
- Security: usable lockscreen, file-based encryption, keystore/gatekeeper, permissions and trusted firmware compatibility. Diagnostic permissive/root settings are not production settings.
- Framework integration: Health, Sensors, Audio, Camera, Radio, GNSS, Bluetooth, Wi-Fi, Power and Thermal services. Old Android 9 blobs may depend on old vendor-kernel APIs despite recognizable chip names.
- Recovery/update packaging: reproducible image manifests, signing, recovery/restore verification, OTA behavior and rollback strategy suited to this device's real partition layout.
- Regression evidence: one matrix linking each feature to its image/kernel, test procedure, logs, physical result and known limitations. Preserve good artifacts so failures can be localized without rebuilding or retesting everything.

## E-ink preparation: concrete starting point

Existing records: `docs/eink-port.md`, `docs/kernel-investigation-20260914.md`, `firmware/extracted/eink-elf-analysis.json` and recovered stock framework/kernel disassembly.

- Logical rear display size observed in stock: 720×1440. `eink,ed052tc2` is a stock driver/DT identifier; it is not sufficient proof of a physical panel part number or permission to copy an unrelated panel's timings.
- Stock HWC contains e-ink composition/commit methods. `libtcon_eink.so` exports init/release/contrast/version functions. Its recorded dependencies are libcutils, liblog, libc++, libc, libm and libdl. That relatively small dependency list makes an isolated ABI/algorithm harness worth investigating; signatures, data structures, callbacks and hidden assumptions still need recovery before executing it.
- Stock framework control transactions and rear touch enable sequencing have been recovered. Transaction numbers belong to stock's Binder interface and must not be sent to modern SurfaceFlinger.
- DT evidence includes secondary DSI, bridge-related controls, Toshiba bridge entries, TPS65185 power management, a panel-associated SPI region and `epd_therm`. Precise fitted bridge identity and live sequencing must be established from stock behavior, not just a list of generic compatible strings.
- **Separate SPI panel data is not backed up.** The stock reader exposes a fixed `0x70080`-byte window, but that has not been proved to be the whole flash. Its read also powers the EPD path and has a nonstandard return convention. The prepared exact-buffer reader was denied by stock permissions in the earlier attempt; no successful dump is recorded. Read-path/coverage review and access are needed before capturing and validating this data. Do not treat the eMMC firmware backup as a backup of panel VCOM/waveforms.

The next e-ink preparation steps are:

1. Map and preserve the panel-specific SPI/calibration data, with repeated hashes and known coverage; retain original values.
2. Recover the stock software-TCON ABI and HWC calls offline, identify buffer sizes/ownership, waveform inputs and output transport. Attempt a sandboxed host/emulator test only after those contracts are understood.
3. Recover bridge/power sequencing from stock kernel functions and compare to exact hardware bindings. Build isolated drivers without changing calibration.
4. On the spare, establish one controlled static refresh with USB logging and a verified exit route; then rear touch and screen switching.
5. Add partial updates, mode selection, rotation, lockscreen, temperature behavior and power savings, then tune perceived smoothness.

The public [A6L reverse-engineering project](https://github.com/WanderingArrow/Hisense_A6L_Eink_Display) remains a useful reference. It describes a buggy Android 11 GSI experiment, not a ready modern-kernel implementation. Its Android-version ceiling assertion has not been accepted as a limit on this source port, and its script has not been executed.

## Priority

Continue short UI/touch and battery/temperature milestones, then pursue an early e-ink proof of operation alongside shared ADSP/radio groundwork. E-ink is the user's central requirement and should receive an early feasibility checkpoint before polishing cameras or fingerprint. Keep long endurance and charging tests behind validated power management.

This audit changed documentation and local preparation records only. No phone operation, installation or new physical feature test was performed.
