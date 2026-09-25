# A6L port checklist

**Updated 23 September 2026** (after the V71 sessions of 21 Sep, the e-ink bridge work of 22–23 Sep and a review of
every attended-session report). This file is the current status index; dated reports are the evidence.

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
