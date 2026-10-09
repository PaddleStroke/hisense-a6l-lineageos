# A6L port checklist

## Current status — 9 Oct 2026 00:05 (installed: round 23)

Installed: boot 10affa76 (force-normal cmdline, Venus CX DTB, cpufreq OSM nodes) + vendor 475d6519 + system 83226f56
(round 23: round 20 + ath10k 0005 Wi-Fi-off fix + SELinux passes 2-4 + e-ink 0055 idle back-off + 0056 reader-sleep allowlist). Kits: `/home/pierrelouis/A6L-usb-20260915/rom-r7c-round*-20261006` on the laptop; builds and audits in
`firmware/extracted/pm-logging-20261005/round*-{vendor,system}-*` (build-round12-image.py, audit-round*-*.py,
stage-round4-kit.py). Handoff detail: `docs/handoff-20261005-claude.md`. SELinux: permissive (prep rules, pass 1).

Legend: **OK** = verified on the phone (by Pierre where noted) · **PARTIAL** = works with known gaps · **UNTESTED** = built in,
not yet exercised · **OPEN** = not working / not started.

| Area | Status | Evidence / notes | Next |
|---|---|---|---|
| Boot, restart with USB (warm-boot backlit black) | OK | charger-mode trap fixed by `androidboot.mode=normal` (fa8ff3c); every reboot on 7-8 Oct normal | — |
| LCD display, touch, GPU | OK | — | — |
| E-ink mirror, LCD<->e-ink switch | OK (Pierre) | switch fast (0042/0045/0048 + hwc 0006 GPU compose); tears fixed (0048/0051); sleep-screen clock OK (lock_clock was 0, now 1; 0054 logs who changes it); carrier marquee off on e-ink (0053) | per-app reader sleep + mirror idle back-off (eink-round9) |
| E-ink reader sleep (Android sleeps between pages) | UNTESTED | 0044 phase 1 + 0056 allowlist (Reader apps default / All / custom), Settings > E-ink, needs Screen lock None; vol keys wake + turn page; 0055 mirror idle back-off (100->1000 ms on static pages) | attended test plan eink-round9-20261008/README Part 3 (~45 min) |
| CPU frequency (CPRh + OSM DVFS) | OK | rev 13b: L2 SAW AVS init was the missing piece; both clusters to top (1843/2208 MHz) on fused open-loop voltages (silver 612-900 mV, gold 692-940 mV); sweep, 3 min all-core + 1080p encode stress OK; loaded at boot (round 19+); opt-out `persist.vendor.a6l.cpufreq=0` | closed loop CPR later; one unexplained freeze (cpufreq + live venus module swap), watch |
| Hardware video encoder (Venus H.264) | OK (Pierre) | Aperture 720p/1080p, High@4.1, 30 fps; cold power cycle (prod3+), sleep OK | — |
| Hardware video decoder (H.264/HEVC/VP8/VP9) | OK | prod5e + Codec2 0007-0009, b11d ACCEPT=1 (Surface 1080p ~140 fps, bit-exact); codecs listed at boot (round 20: venus before cpufreq + media restart) | Pierre: Gallery playback smoothness |
| Camera: 3 sensors, photo/video, 1080p (two-stream HAL fix) | PARTIAL | works (HAL 0a203bd4). Image-quality root causes found (camera-iq-20261008): hw-ISP black point = red channel value applied to all channels before WB (yellow/green tint growing into shadows, main+front); per-camera control cache leaks CCM/tone curve between cameras; tone curve reloaded only at stream start; videos full-range BT.601 tagged limited BT.709 (clipped highlights, +15% contrast) | 9 Oct live: V2b tuning + core r8 (0105-0107) neutral colour; AF patch 0108 (first scan sample read before the lens moved -> wrong locks; settle discards, 2-sample median, parabola fit, sustained-drop rescan) signed IPA 719abc4b + V2b-af0108: "focus is better" (Pierre). Sharpen 250 % doubles detail without extra noise but still ~1/2 of stock (stock B/G 0.79 warm, ours 0.94). Open: tap-to-focus never implemented (patches 0109/0110 in progress), stock-like detail/tone (0111 + V2c in progress; is stock 12 MP full-res remosaic?), ship into tree, video range tag |
| Speaker / audio playback | OK (Pierre) | after-sleep silence fixed (round 16: LPI pad restore on resume + sm8250 BE wakeup source) | — |
| Microphone / recording audio | OK | tinycap + Aperture audio track | — |
| Wi-Fi (WCN3990 / ath10k) | OK | connects at boot (cfg80211 early); WoWLAN at suspend (no teardown crash); Wi-Fi-off crash root-caused (TXBF reset after VDEV_DOWN) and fixed in ath10k_core 0005 quirks=1 (live A/B: 4 passes, control crashes) | verified on round 21: Wi-Fi off/on, no fatal |
| Modem / mobile data / calls / VoLTE | PARTIAL | 9 Oct with Pierre's SIM: hot-insert detected, mobile data worked (Play Store browsing). Calls, SMS, VoLTE, GPS not yet tested (SIM is Pierre's main one: needs a planned 10-15 min session with the radio logger) | SIM session: SMS, calls (earpiece/speaker/mic), VoLTE, data with Wi-Fi off, GPS |
| Bluetooth | UNTESTED | stack present | pair + audio test |
| GNSS, sensors (accel, gyro, prox/light, hall) | UNTESTED (partly seen working earlier) | — | quick check |
| Suspend / battery | PARTIAL | s2idle works; "hard LOCKUP" watchdog reports in s2idle are false positives; mdss_ahb_clk stuck-on warning at suspend (cosmetic); backlit-black panel on timer-only wakes (bug, power) | power measurement; mdss fix (mdss-ahb-20261007) |
| Charging / battery status (BMS) | OK (live, 9 Oct; in tree for next round) | "Not charging" root cause: pmi8998_fg loads before qcom_smbx, never links the charger, and its current-sign fallback was inverted (positive = charging) -> DISCHARGING -> "Not charging". Fixed module e0587abf (live charger status by name, sign fixed; bms-20261009). QC 9 V never ran when moving the cable from the laptop: the UDC stays "configured", the guard kept the D+ pull-up, APSD saw OCP. Guard Q8 trusts APSD OCP/FLOAT over the stale UDC state -> pull-up off, rerun, QC3 8.8 V, battery 2.5 A (47 -> 50 % in 2 min); back on the laptop the pull-up reconnects (adb OK). Speed label: no voltage_max, Android assumes 5 V; 0.5 A at plug-in = "Charging slowly" until the 2 A limit is read | speed label confirmed "Charging rapidly" ~1 min after plug-in (Pierre, 9 Oct); ship in next round; optional voltage_max in qcom_smbx so the label is right from the first seconds |
| SELinux enforcing | OPEN | pass 1 shipped (permissive); pass 2 in round 21 (module groups OK under vendor.a6l_modules_*); passes 2-4 (round 23): kernel avc 905 -> 17, all harmless (radio helpers probing linker dirs at exec, shell dmesg); tqftpserv firmware access fixed (would have broken the modem under enforcing) | attended enforcing test boot (boot-only flash of boot-10affa76-enforcing.img 9f96cd55; fallback = permissive 10affa76) |
| Google apps / Play Protect | PARTIAL | device registered (GSF id), message gone. Play Store showed Google Maps "not compatible": ro.opengles.version unset -> reqGlEsVersion=0x0; fixed in rom.mk (196609 = GLES 3.1, Mesa FD512), round 24 | flash round 24, install Maps |

| Stability | OPEN | 9 Oct 11:32: one spontaneous reset during the first cellular-data session (Play Store, gold cluster at 70 C, on battery): screen black ~2 s then boot logo, no button press (Pierre); no panic or log (rings just stop), pstore empty. powerup_reason=HRST is set on almost every boot and proves nothing; the hw watchdog is not armed. Suspects: PMIC power fault (no LMh/BCL current limiting yet), modem/IPA under data (ipa2_lite, anoc2 SMMU), CPU DVFS. Kit: firmware/extracted/crash-20261009 (pon-reason.sh, krec session, warm-reset module for ramoops, repro scripts) | after any reset: run pon-reason.sh BEFORE rebooting; kernel.panic=5 + a6l_diag 0400 next round |
| USB while asleep | OPEN | adb drops at every system suspend on a PC port (dwc3 "HS-PHY not in L2", QSCRATCH restore + re-enumeration at each resume); hidden before by stay-awake. Charging continues | keep the link in L2 across suspend like stock |

### Known caveats for daily use
- Round 21: Wi-Fi off/airplane no longer crash the modem.
- Playback/recording of heavy video while the CPU is pinned at max is untested beyond the 3 min stress.

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
