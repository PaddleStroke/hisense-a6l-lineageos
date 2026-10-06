# A6L port checklist

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
