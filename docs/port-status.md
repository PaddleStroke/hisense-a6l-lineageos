# A6L port checklist

## Current status — 13 Oct 2026 (project paused)

**Builds:**
- **Round 29b, userdebug.** The recommended development ROM. Boot 9f96cd55 (SELinux **enforcing**), dtbo 69251122, vendor 4f7e6654, system 83226f56 (round 23). Laptop kit `rom-r7c-round29b-20261006`.
- **User/release-keys build.** Installed on the spare on 13 Oct. Boot 9f96cd55, vendor 339693b6, system d7ed7fdf. Kit `rom-r7c-user-20261013`. The way back is the wipe kit `rom-r7c-round29b-rewipe-20261013`.
- Builds and audits live in `firmware/extracted/pm-logging-20261005/` (`build-round29-image.py`, `audit-round29-vendor.py`, `stage-round4-kit.py`) and `firmware/extracted/user-build-20261012/` (`build-user.sh`, `wipe-mode/`).
- Full history: `docs/handoff-20261005-claude.md`. Upstreaming notes: `docs/upstreaming.md`.

**Why the user build was made, and the result:** the user build was meant to run La Poste "L'Identité Numérique" (`fr.laposte.idn`). It still refuses the device. Its protection library crashes the app on purpose at launch. Ruled out:
- USB debugging;
- root (none on the user build);
- the build type (user/release-keys);
- the device identity (Hisense/A6L).

The remaining signals are the unlocked bootloader (`ro.boot.verifiedbootstate=orange`), the hardware attestation and the lack of Google certification. They are not addressed on purpose, because addressing them would mean spoofing an integrity check. The bootloader cannot be relocked with non-Hisense-signed images.

Legend: **OK** = verified on the phone (by Pierre where noted) · **PARTIAL** = works with known gaps · **UNTESTED** = built in,
not yet exercised · **OPEN** = not working / not started.

| Area | Status | Evidence / notes | Next |
|---|---|---|---|
| Boot, restart | OK | Restart from Android works under enforcing (13 Oct, back in 45 s). | The first boot after the user wipe landed in the diagnostic recovery once; it was left with a sysrq reboot. The wipe flow should end with a reboot, not a power-off. |
| LCD display, touch, GPU | OK | — | — |
| E-ink mirror, LCD<->e-ink switch, e-ink key wake, lock clock | OK (Pierre, 29b enforcing) | Enforcing needed the mirror rule `gpu_device:dir search` (round 29b) and `hal_graphics_composer_default:fd use` (in the tree and the user build). TPS65185 latched-UVLO fix: tps65185.ko 0cc74f3b (round 29+), see `firmware/extracted/eink-tps-20261013`. | — |
| CPU frequency (CPRh + OSM) | OK | Both clusters reach the top frequency (1843/2208 MHz). | Closed-loop CPR. |
| CPU idle / suspend power | PARTIAL | Overnight drain on 12-13 Oct was 349 mA: the gold cluster busy-looped in s2idle on PSCI states the TZ rejects. Fix F1: `init.a6l.cpuidle.rc` (WFI + CPU power collapse only, `persist.vendor.a6l.cpuidle.pc_only=1`, round 29+). Live test: CLOCK_MONOTONIC frozen 93 % of the time while screen off. See `firmware/extracted/battery-20261012/README.md`. | Measure an unplugged overnight run (`measure/a6l-pm-log.sh`). Phase 2: L2 power collapse via DT (`dt-psci-idle/`). mdss_ahb fix. |
| Hardware video encoder / decoder (Venus) | OK (Pierre) | Gallery freeze fixed (Codec2 0010, round 27). | Video range tag. |
| Camera: 3 sensors, photo/video, AF, tap-to-focus, flash/torch | OK (Pierre, round 28) | Core r14 c78bfe0a, HAL r15 e3744657, tuning V2g. Tap-to-focus, fast AE, no colour dance. HAL NULL-settings bug fixed (0118). | Image quality is still slightly below stock. |
| Speaker, earpiece, wired headset, microphone | OK (Pierre) | In-call volume keys and headset routing fixed (round 27: audio r3, HAL 0006/0007, TFA 0005 health). | — |
| Wi-Fi (WCN3990 / ath10k) | OK | Works under enforcing. Needs the modem up (rmtfs). | `wlan0` MAC is not set ("mac-failed": `ip` is a system binary), so it stays random. |
| Modem / data / calls / SMS | PARTIAL (Pierre) | Data, SMS, calls, in-call speaker, volume and headset work. Enforcing blocker fixed in 29b: rmtfs/tqftpserv AT_SECURE dropped LD_LIBRARY_PATH; fix `allow a6l_radio_ctl ... noatsecure`. | VoLTE: the carrier (Orange) refuses Hisense devices; not a ROM bug. |
| Bluetooth, GNSS, sensors | OK (Pierre) | — | Hall/cover not checked. |
| Charging / battery status | OK (Pierre) | pmi8998_fg fix + guard Q8/Q9; QC3 9 V. | voltage_max for the speed label. |
| Thermal | OK | Skin mitigation in the thermal HAL (round 28). | Charging skin budget in the guard. |
| SELinux | **OK, enforcing** (13 Oct) | Feature pass on 29b enforcing: camera ×3, video, Gallery/YouTube, BT, Wi-Fi, e-ink, restart. Remaining denials are AOSP/GMS noise. | — |
| User build (release keys) | PARTIAL | Builds, signs and audits; the wipe install works. | **Bug:** USB debugging is ON by default (`persist.sys.usb.config=adb` is set at runtime; the images do not set it; source not found yet). First-boot setup with USB plugged in: Google "Copy apps & data" crashes (`UsbD2dMigrateService` NPE) and loops the wizard; unplug USB during setup. |
| Google apps / Play Store | OK | Maps installs (GLES property, round 24). | — |
| **Stability** | **OPEN** | SMPL resets under load on 9 Oct and 12 Oct, with no BCL/LMh current limiting. On 13 Oct, on the user build, two hard resets (`powerup_reason=HRST`, FSCK at next boot) during heavy use: an app crash loop, and the Play Store. The user build keeps no kernel log, so the cause is unknown. Round 29b was not soak-tested. | Soak 29b (userdebug keeps kmsg in /data via a6l_bootlog_data). Ramoops/pstore. BCL register path. |
| USB while asleep | OPEN | adb drops at every suspend (dwc3 not kept in L2). | Keep the link in L2 like stock. |
| /data encryption | OPEN | /data is plain ext4 (no FBE/metadata encryption). | — |

---

## History (dated, older first-paragraph summaries)

Latest 4 October evening: r7c logger-only vendor update is installed. All29
runtime pins pass. Both fixed rings accept fresh CRC-checked markers through
38minutes awake with stable2.1MiB metadata headroom. Phone stays awake on LCD
while USB plugged; no unattended sleep or hardware-codec probe is performed.
RAM retention fails across both tested recovery resets, including explicit warm.
Sleep root cause remains unknown; PM_DEBUG kernel scope and module ABI audits pass.
Actual Android DT confirms Venus hardware-codec block disabled. Native software
AVC format probes encode about37-38ms/frame; both formats decode identically,
with no encode-only NV12 speed gain. C2 input candidate remains uninstalled.
Camera colors/framing/speed and e-ink notification artifacts remain open.
See logger-r7c-20261004.md and video-encoder-audit-20261004/README.md.


**Latest, 4 October:** r7c is installed and all 28 runtime pins pass. Saved front
video improves from about 3.7 to 9.3 fps and finalizes normally 2.643 seconds
after Stop. RGB preparation falls from about 250 to 58 ms; software compression
still takes about 57 ms. Native capture keeps pace at 48 kHz and closes normally,
without the prior sustained audio-gap pattern. Recording still falls short of
30 fps. A signed Camera-only update now bypasses the viewport GL pass and
physically delivers YUV to Codec2; its saved front video improves further to
16.463 fps with normal saving and contiguous AAC. This is an app trial on r7c,
not a new image; broader orientation/mode/camera acceptance remains pending.
Camera colors, audible speech/playback/calls, e-ink labels and lock/wake
reliability still need acceptance. No next build has started. Prior paragraphs
below retain their dated evidence. See [r7c report](rom-r7c-20261004.md).

**Current update, 4 October 2026:** LineageOS 24 r6z is installed, boots and
passes all21 runtime payload pins. Hardware graphics and touch work. Photo
previews measure about29/30/28fps; front VIDEO preview now measures29.995fps
after removing RAW resizing. Encoded HD video still measures4.099fps; conversion
versus software compression is being measured. Clean recordings save and close
normally, with about6s finalization delay. The PCM negotiation correction passes
73 hardware-buffer wraps without the former premature XRUN; audible playback,
fresh spoken recording and calls await acceptance. Launcher e-ink labels are
installed, optical result pending. Washed colors and intermittent sleep remain
open. Late recovery USB was partly its gadget watchdog, not proof of a four-minute
Android shutdown. Default-off r7a timing/calibration diagnostics are in preparation.
Current evidence: [r6z report](rom-r6z-20261004.md) and
[master TODO](master-todo-20260925.md). The chart below is the historical
23 September recovery/RAM baseline, not the installed Android acceptance list.

**Historical baseline, 23 September 2026** (after the V71 sessions of 21 Sep, the e-ink bridge work of 22–23 Sep and a review of
every attended-session report). Dated reports are the evidence.

**Phone state:** rooted stock Android (Magisk, boot partition) with the **V71 diagnostic recovery** in the recovery slot.
Everything below runs **from RAM** under V71 (bundles pushed over ADB); nothing of LineageOS is installed on the eMMC.
**Target:** a usable, installable LineageOS 24 image with both displays.

Legend: **Works** = shown on the phone by the user or by read-back; **Partial** = some parts shown, a known blocker left;
**Prepared** = offline work only; **Open** = nothing started.

## Status chart

| Area | Status | Shown on the phone | Next missing checkpoint | Evidence |
|---|---|---|---|---|
| Kernel / boot | Works (diagnostic) | Linux 7.2.3 phone kernel, V71 recovery, stable | production config, watchdog, reboot/suspend | attended-session-results-20260921-v71.md |
| USB / ADB | Partial | authenticated ADB, fast transfers | recovery USB sometimes not enumerating; MTP, OTG, charging negotiation | recovery notes |
| Internal storage | Partial | eMMC read, firmware hashes, read-only mounts | persistent install location (**Pierre's decision**), writable data, encryption | roadmap-to-working-image-20260920.md |
| **LineageOS UI** | **Works from RAM** | welcome screen → setup wizard played through, usable, boot ~2 min | real Android `init` + vendor image instead of the test supervisor; persistent install | phone-framework-first-boot-20260920.md, …-v71.md |
| LCD | Works | native DPU + DSI0 + FT8719 driver, colours, brightness | blank/unblank + resume | …-v71.md |
| **GPU** | **Works** | Adreno 512, Mesa freedreno GLES 3.1 drives SurfaceFlinger, no faults/underruns | Vulkan (turnip), GPU power management, HW video codecs | attended-session-results-20260921.md |
| CPU frequency | Open (known gap) | cores stay at the bootloader frequency (measured: not slow) | CPR3/OSM port for SDM660 (multi-day, voltage — attended only) | cpufreq-assessment-20260920.md |
| Front touch | Works | multi-touch in LineageOS | edges/rotation, suspend | android-input-v47 |
| Buttons | Works (events) | power, volume, e-ink key (code 616) | Android actions/wakeup | controls-v46 |
| Vibration | Partial | commands succeed, nothing felt | stock PM660 haptics config review | controls-followup |
| **Rear e-ink** | **Works** | kernel bring-up (V73 panel driver) + on-phone service `a6l_epdd` (stock TCON + panel waveform): 16 dithered greys, quality/partial/fast/fastest/clear modes, no ghosting after clear, stock-order power | Android integration (what goes on the rear screen), on-demand full clear, repeat on fresh boots | claude/fresh-eye-eink-bridge doc; attended-session-results-20260922-eink-bridge.md |
| Rear touch | Works (events) | ft5x06 3-0038, taps counted | face switching, inactive-face rejection | …20260921.md |
| Battery readings | Works | voltage, capacity, current, temperature | Health HAL accuracy | — |
| Charging / thermal / sleep | Open | — | charger policy, thermal, deep sleep | — |
| ADSP | Works | boots, QMI services on node 5 | — | …20260921.md |
| Motion sensors | Partial | SMGR up → IIO accel/gyro/mag | Android sensors HAL (IIO) | …-v71.md |
| Light/proximity | Partial | TMD3702 answers at 0x49 (front STK3338 never answers) | TMD3702 driver (none upstream) | …-v71.md |
| Audio | Partial | APR/q6 stack, pm660l codec, sound card "Hisense A6L" registers (0 route failures) | first playback (earpiece/headset); speaker needs a TFA9894 amplifier driver | …-v71.md |
| Modem | Partial | boots, all QMI services register (NAS, UIM, voice, WMS, WDS…) | crash after ~16 s: `dog_hb` diag task starvation → needs a diag router | radio-first-boot-20260921.md |
| Wi-Fi | Partial | ath10k_snoc probes | blocked by the modem crash + tqftpserv paths / `wlanmdsp.mbn` | radio-first-boot-20260921.md |
| Bluetooth | Partial | hci_uart + QCA load | `msm_serial c1af000` probe −22 (DT fix) | radio-first-boot-20260921.md |
| Cellular calls/data, GNSS | Open | — | after the modem is stable | — |
| Cameras | Open | — | — | — |
| Fingerprint | Open | — | needs TEE path | — |
| Installable release | Open | — | real init + vendor image, install location, enforcing SELinux, signing, recovery/OTA | real-init-container-design-20260921.md |

## Next steps, in order
1. Modem: diag router so the modem stays up → Wi-Fi (tqftpserv paths, `wlanmdsp.mbn`) → GNSS.
2. Bluetooth UART DT fix → `hci0`.
3. Audio: first playback on earpiece/headset through the pm660l codec; TFA9894 speaker driver.
4. E-ink into Android (service + display policy), rear touch routing, e-ink key.
5. Real Android `init` from RAM (container design) → then persistent install once Pierre picks the location.
6. Sensors HAL, TMD3702 driver, vibration, charging/thermal/suspend, cpufreq.
7. Cameras, fingerprint, release engineering.

After each meaningful test: update this chart, link the report, and separate "command succeeded" from "user saw it".
