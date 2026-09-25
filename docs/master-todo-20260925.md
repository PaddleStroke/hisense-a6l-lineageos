# Master TODO: a daily-drivable LineageOS 24 on the Hisense A6L (25 Sep 2026)

Agent `todo`, offline review only: nobody touched the phone. Sources: every project doc (claude/*.md up to
attended-20260925), the repo docs (port-status, all docs/*-2026092[345].md, flash/hals/rest/realinit/roadmap/hardware
readiness), /home/claude/overnight-status.md, and three read-only checks made today: (a) `device/hisense/a6l/rom/`
and `lineage_gsi_a6l.mk`, to see what is actually integrated in the ROM; (b) a read-only listing of the laptop
`~/A6L-usb-20260915/` (relay `todo-02`); (c) `git status` of the repo.

**Goal definition ("daily driver").** The phone is installed on the eMMC and boots by itself. It charges and sleeps
safely and lasts at least a day. It makes and receives calls with audible audio, and the screen goes off at the ear.
SMS works, Wi-Fi works (with WPA), and Bluetooth audio works. GPS gets a fix. Mobile data works, or Pierre explicitly
accepts Wi-Fi-only. At least the main camera works. Rotation and auto-brightness work. The e-ink is usable from Android.
The phone can be updated and rolled back to stock. SELinux runs enforcing.

Status key: **P** = proven on the phone (Pierre saw it, or a read-back proved it). **P~** = partly proven, with a known
gap left. **B** = built and checked offline, but never run on the phone. **D** = designed or documented only, nothing
built. **N** = not started by anyone. "DD" is whether the item is needed for a daily driver: **Y** = required,
**R** = strongly recommended, **O** = optional.

---

## 0. Headline findings (read these first)

1. **The installable ROM (rom-v1 kit-r8) contains almost none of the 24 to 25 Sep work.** `rom.mk` inherits only
   `gnss.mk` and the dalvik heap config. The `full` variant (`rom/full/full.mk`) adds audio3, the Wi-Fi/BT/lights/power/
   thermal HALs and nothing else. The following are **not integrated anywhere**: the radio HAL (`radio.mk`),
   eink3 (`eink.mk` and the composer patch), the sensors multihal, the USB gadget/MTP HAL, the vibrator HAL, q6voice
   and a6l-q6voiced, the audio4 q6routing fix, the speaker, the camera patches, haptics, hall, the charger, flash LED,
   cpufreq, IPA and the USB watchdog. The kernel/DT is still the **V74 base DT without the speaker, charger, camera,
   haptics or hall overlays**. Nobody owns a "rom-v2 integration" (one V75 kernel + DT + vendor). That integration is
   the first item on the critical path.
2. **The rom-v1 kit is NOT on the laptop.** Checked today: `~/A6L-usb-20260915/rom-v1` does not exist. kit-r8 exists only
   in WSL at `/home/a6l/rom-v1/kit-r8/rom-v1`.
3. **There is no proven rollback path from an installed ROM.** Once system is replaced, stock cannot boot, so there is no
   `adb reboot edl`. Our kernel has no `reboot edl`, and the ABL has no known EDL command. The flash doc's "Plan B"
   (dd from the V74 recovery over adb) is untested, and the hardware EDL entry method is undocumented. **Before the first
   install, prove one way back into EDL, or test Plan B.**
4. **Charging under rom-v1 is unverified.** The charger overlay (`a6l-charger-v75`) is not in the rom-v1 DT. An
   installed phone might not charge, or might charge only at the PMIC hardware default. This must be checked in the
   first installed boot, and the phone must not be left on the ROM for long until it is proven.
5. **The in-call proximity sensor is missing.** The FRONT sensor is the STK3338 (`i2c@c1b6000` 0x47), and it has
   **never answered on I2C**. The TMD3702 that works is the REAR sensor. Without the front sensor, the screen stays on at
   the ear during calls, and front-side auto-brightness has no input. Nobody is working on this.
6. **The earpiece is physically broken on the test unit.** Handset-mode calls cannot be validated on this phone.
   Calls need the headset, or the speaker once the TFA9894 driver works.
7. **No VoLTE/IMS, no USSD, no call waiting/forwarding, no MMS.** Nobody has started any of these (the RIL doc lists them
   as unsupported). Calls rely on CSFB to 2G/3G, which is a long-term risk as French operators switch off 2G/3G.
8. **Nobody has started these:** Vulkan (turnip), hardware video codecs (venus + Codec2), Widevine, OTA/updater, a
   LineageOS recovery for installs, release-key signing, a GApps/Play Integrity plan, SELinux enforcing policy
   compilation, USB OTG, the charger (off-mode) boot, the notification LED check, a production kernel config (watchdog,
   `panic=` reboot, debug options off), and a user guide for Pierre.
9. **Repo hygiene.** The last git commit is from **21 Sep**. `git status` shows 153 changed or untracked entries, which is
   all the work of 22 to 25 Sep (eink, hals, radio, gnss, audio, camera, rom, …). One disk loss would erase four days.
   Commit it soon (Pierre, or the main agent from the sandbox shell with `-c user.name=Pierre`). Note: my read-only
   `git status` left an empty `.git/index.lock`, because the sandbox cannot delete files. I removed it through relay
   `todo-01-unlock` (REMOVED_EMPTY_LOCK). **Other agents: do not run git in the sandbox shell.**
10. `docs/port-status.md` is stale (23 Sep). It is owned by the main agent and should be refreshed from this file.

---

## 1. Critical path to a daily driver

```
[A] rom-v2 integration (flash agent owns it: V75 kernel + DT + vendor with all deliveries)  ── 3–5 days
      │
[B] Prove rollback (EDL entry or V74 Plan-B dd) + stage kit on laptop                    ── 0.5 day attended
      │
[C] First install on eMMC: boot, /data, USB, CHARGING, battery %, suspend/resume, e-ink    ── 1–2 attended sessions
      │
[D] Power basics: charger overlay + SW JEITA, s2idle + wake sources, thermal HAL,
      cpufreq (CPR/OSM open-loop, attended)                                                ── 1–2 weeks
      │
[E] Telephony in the ROM: RIL HAL on the real modem → SIM/SMS/calls; q6voice + q6voiced
      + in-call routing; FRONT proximity (STK3338); speaker (TFA9894) for handsfree        ── 2–3 weeks
      │
[F] Wi-Fi WPA association + MAC; Bluetooth HAL + bdaddr + A2DP; GNSS fix                  ── 1 week (parallel to E)
      │
[G] Mobile data (ipa2_lite → rmnet/QMAP → RIL data call → netd)                           ── 2–4 weeks, may fail
      │
[H] Cameras: frames to DDR → libcamera (SoftISP) Android HAL → CameraX apps              ── 3–6 weeks
      │
[I] Release: SELinux enforcing, release keys, recovery + OTA path, user guide              ── 2–3 weeks
```
The longest and most uncertain items are G (IPA v2.6L) and H (camera HAL). The rest is integration and validation.
Realistic total if everything goes well: **6–10 weeks** of combined agent and attended time. A usable "Wi-Fi + calls +
SMS" build is possible after about A–F (**3–4 weeks**).

---

## 2. Hardware

| # | Item | Status | Evidence | Next concrete step | Estimate | Risk | DD |
|---|---|---|---|---|---|---|---|
| H1 | LCD (FT8719, DPU/DSI0) | **P** | attended-session-results-20260921-v71.md | In the installed ROM: blank/unblank, resume after suspend, find the backlight sysfs name for the lights HAL (`ls /sys/class/backlight`) | 1 day | low | Y |
| H2 | GPU GLES (Adreno 512, Mesa freedreno) | **P** from RAM (21 Sep) | attended-session-results-20260921.md | Confirm in the installed ROM (`persist.graphics.egl=mesa`); GPU devfreq/power | 1–2 days | low | Y |
| H3 | Vulkan (turnip) | **N** | (none) | Cross-build Mesa turnip for Android (NDK), `ro.hardware.vulkan=freedreno`; check that HWUI/SF stay on GL if it is unstable | 3–5 days | med | R (many apps, and ANGLE-on-Vulkan paths) |
| H4 | Rear e-ink panel (TC358767 bridge, TPS65185, a6l_epdd) | **P** (kernel + v2 clear, 23 Sep) | claude/fresh-eye-eink-bridge, eink-clear-prep | none at the panel level; see S5 for Android | — | low | Y (it's the point of the phone) |
| H5 | E-ink frontlight | **D**/unknown | attended-session-results-20260922-eink-bridge.md (candidate `a6l-eink-frontlight.dtso`, LPG ch4); hals doc: "none known" | **Ask Pierre whether the stock rear screen has a frontlight.** If it does: V75 DT + `leds-qcom-lpg.ko` + a sysfs control in the e-ink settings | 1–2 days | low | O/Y depending on the hardware |
| H6 | Front touch | **P** | android-input-v47 | Check it in the installed ROM; wake/suspend behaviour | 0.5 day | low | Y |
| H7 | Rear touch | **P** (events) / **B** mapping | eink3-20260924.md | E8/R5 in eink3 §7B: orientation property, uinput → InputReader | 1 day | med | Y |
| H8 | Keys: power, vol +/-, e-ink key 616 | **P** (evdev) / **B** (Android) | controls-v46, eink3 | ROM: volume UI, power-key wake from suspend, e-ink key handled by a6l_eink_mirror (short = mirror, long = clear) | 0.5 day | low | Y |
| H9 | Vibration (PM660 LRA) | **P~** failed: nothing felt (24 Sep) → **B** rest2 | misc-20260924.md §3, rest-20260924.md §2 | Attended rest2 MODE=haptics (ilim 800, SC debounce, auto-res, sweep). Then add the VibratorOL HAL (`vendor/qcom/opensource/vibrator`) to the ROM | 1 day + HAL 0.5 day | med (actuator fault possible) | Y |
| H10 | Loudspeaker (TFA9894 amp) | **B** (driver, stub DAI, v75 overlays); never played | audio3-20260924.md | V75 DT with the speaker overlay → the amp probes → the first tone at low level. **Calibration/profile (the stock tfa container) must be reviewed to avoid damaging the speaker** | 3–5 days | **high** (speaker damage, card-wide probe dependency) | Y (ringtone, handsfree) |
| H11 | Earpiece | **broken on this unit** | attended 23 Sep | Codec EAR path in mixer_paths only (untestable here); verify on another unit, or leave it | — | n/a | Y on a healthy unit |
| H12 | Headset out + jack detection | **P** (detect; call downlink) / **P~** media (ADM 0x10325 error) | attended-20260924, audfix | audio4 MODE=tone/media (q6routing per-direction patch) | attended 1 h | med | Y |
| H13 | Microphones (main, secondary, headset) | **P~** uplink proven in a call (probably the headset mic, 24 Sep) | attended-20260924 | audio4 MODE=media capture; test the main mic without a headset; the secondary mic/ECNS has no ACDB | 1–2 days | med | Y |
| H14 | Headset button (hook/media keys) | **N** | (none) | Check the codec MBHC button kcontrols/input events; map KEY_MEDIA | 0.5–1 day | low | R |
| H15 | Call audio (q6voice / CVD) | **P** in recovery (legacy CVD=0, headset, both ways, 24 Sep) | attended-20260924, kvoice-20260924.md | audio4 MODE=call GAIN=0/3/6; then **ROM integration**: q6voice patch in the ROM kernel, voice DT, `a6l-q6voiced`, RIL sets `vendor.a6l.voice.active`, audio HAL in-call routing. No EC/NS (no ACDB) | 1 week | med | Y |
| H16 | Modem (MPSS boot, rmtfs, diag-router over QRTR) | **P** (stable, 24 Sep) | morning-20260924, attended-20260924 | ROM `a6l-radio.sh` on the real eMMC (EFS read-only by default) → 30 min stable; then decide rmtfs_rw | 1 attended session | med | Y |
| H17 | SIM / registration | **P** (CLI: PIN, online, LTE Orange F) | attended-20260924 | Radio HAL V4 on the real modem in the ROM (never talked to a modem): SIM status, PIN UI, signal bars, NITZ time | 3–5 days attended | med | Y |
| H18 | SMS | **P** (CLI send + receive) / **B** decode fix | attended-20260924, misc §1 | ril2 sms-listen (bare TPDU decode); then SMS in the Messaging app through the HAL | 2 days | low | Y |
| H19 | MMS | **N** | (none) | Needs mobile data (or a Wi-Fi-calling-free path: no) → after G | after G | high (depends on IPA) | R |
| H20 | Voice calls: outgoing | **P** (CLI dial to voicemail) | attended-20260924 | ril2 dial-dtmf; then calls from the Dialer through the HAL | 2–3 days | med | Y |
| H21 | Voice calls: **incoming/ringing** | **N** on the phone (HAL code exists, callRing) | ril-20260924.md | Attended: CLI `call-wait`/answer, then the HAL (ring, answer, reject). Needs a ringtone → speaker (H10) | 2 days | med | Y |
| H22 | DTMF | **B** (ril2) | misc §1 | ril2 dial-dtmf `1234#` | attended 10 min | low | Y (IVRs, voicemail) |
| H23 | Emergency calls | **B** in the HAL (emergency dial + 112/911 list); **cannot be tested live** | ril-20260924.md | Offline review of the emergency flow (no SIM / locked SIM / no service); never dial 112 for a test. Check what the modem does with an emergency number without a SIM | 1 day review | **high** (safety) | Y |
| H24 | USSD, call waiting/forwarding, CLIR, network scan, cell broadcast | **N** ("not supported" in ril doc) | ril-20260924.md §limits | Implement VOICE USSD (orig_ussd / ussd ind) first (prepaid balance), then SUPS | 3–5 days | med | R (USSD), O (the rest) |
| H25 | VoLTE / IMS / VoWiFi | **N** (nobody) | ril doc: "IMS/VoLTE not supported" | Research only: the stock IMS stack is proprietary (QTI ims + DPL). An open IMS client (e.g. the Doubango-based ones) is a multi-month project. Plan: rely on CSFB and **check the Orange F 2G/3G switch-off dates** | months | **high** (network sunset) | R (long-term), not for v1 |
| H26 | Mobile data (IPA v2.6L + rmnet + WDS) | **B** (ipa2_lite resets the phone at load → ipa2b step-logged) | ipa-20260924, ipa2fix-20260924, ipa-sdm660-plan | ipa2b STOP_AT walk with `dmesg -w` streamed → fix → MODE=status/data → RIL setupDataCall → netd routes/DNS | 2–4 weeks | **high** (upstream v2.6L never passed packets) | Y (or Pierre accepts Wi-Fi-only) |
| H27 | Wi-Fi (WCN3990/ath10k) | **P~** scan only (4 networks, 23 Sep). **Association/WPA never tried** | prep-20260923, hals §Wi-Fi | ROM full variant: Wi-Fi HAL (the fallback-legacy-HAL question is open; plan B goldfish `libwifi-hal-emu`) + wpa_supplicant → connect to Pierre's Livebox (WPA2), DHCP, DNS, sustained throughput, reconnect after suspend | 2–4 days | med | Y |
| H28 | Wi-Fi MAC | **B** (a6l_macs; the real MAC 7c:b3:7b:99:40:46 is in persist) | hals §Wi-Fi | Attended `dmesg | grep A6L_MACS`; the permanent MAC stays random (framework randomisation is fine) | 0.5 day | low | R |
| H29 | Wi-Fi hotspot/tethering, P2P | **N** | hals §3 | hostapd packaged; test after H27 | 1–2 days | med | O |
| H30 | Bluetooth (hci0, WCN3990 UART) | **P** hci0 (23 Sep, serial1 DT) / **B** HAL + bdaddr | hals §Bluetooth | ROM full variant: `a6l_macs bt` (hci0 UNCONFIGURED finding) → HAL `hci interface 0 found` → pair a device | 1–2 days | med | Y |
| H31 | BT audio: A2DP / HFP (SCO) | **N** | hals: "A2DP/SCO offload not wired" | A2DP: AOSP software encoding through the audio HAL's `/bluetooth` module (the AIDL example HAL registers it) → test with headphones. HFP/SCO: WCN3990 SCO is routed to the SoC PCM/SLIMbus on stock; on mainline you have to **check whether SCO-over-HCI (UART) works** (a vendor command may be needed) | A2DP 2–3 days; HFP 1–2 weeks | med / **high** (HFP) | Y (A2DP), R (HFP) |
| H32 | GNSS | **P~** (LOC engine on, 7 SVs, max 32 dB-Hz, no fix in 600 s) / **B** gnss2 XTRA + HAL | attended-20260924, gnss-20260924, misc §2 | gnss2 XTRA + coarse position outside with open sky; compare C/N0 with Pierre's everyday A6L; then the HAL in the ROM (a map app) | 1–3 days | med (antenna/LNA config unknown) | Y |
| H33 | Main camera IMX576 + GT9769 VCM | **P~** (chip-ID, sensor streams at frame counter 6→157, no frame in DDR; stream-off oops) / **B** camera3 | attended-20260925, camfix2-20260925 | camera3 bars (oops fix + ICC vote + VFE diagnostics) → first raw frame → AF sweep | 1–2 weeks to stable frames | high | Y (at least the main camera) |
| H34 | Front camera S5K3T1 | **P~** chip-ID / **B** own driver (585-reg init) | cam-20260924, camfix | camera3 bars SENSOR=s5k3t1 after IMX576 works | 1 week | high | R |
| H35 | Aux camera Hi-846 | **P~** chip-ID / **B** (upstream drv, 4-lane patch) | camfix | camera3 bars SENSOR=hi846 | 3–5 days | med | O |
| H36 | Camera Android HAL (libcamera + SoftISP) | **D** (camera_hal.yaml, package list; libcamera ≥ 0.7 **not built**) | cam-20260924 | Cross-build upstream libcamera with the Android HAL + simple pipeline + SoftISP; tuning (no chromatix CCM/LSC decoded); CameraX/Camera2 app test | 2–4 weeks | **high** (quality, performance without cpufreq) | Y |
| H37 | Flash / torch LED | **B** (`a6l-flash-v75.dtso`, leds-qcom-flash) | rest-20260924 | V75 DT → `echo 1 > …/brightness` → torch HAL (Lineage flashlight via camera HAL or `torch` sysfs) | 1 day | low | R |
| H38 | Motion sensors (accel/gyro/mag via SMGR) | **P** (IIO devices) | attended-session-results-20260921-v71 | Sensors multihal + trout IIO sub-HAL (in no variant yet): verify the IIO names, the XML schema, axis signs, mag support → auto-rotate | 3–5 days | med | Y |
| H39 | Rear ALS/prox TMD3702 | **P** (ALS + prox with stock regs, 23 Sep) | prep-20260923 (als2) | Buffered IIO/hrtimer trigger (not done) or its own sub-HAL; useful for e-ink policy | 2 days | low | O |
| H40 | **Front ALS/prox STK3338** | **P~ FAILED**: never answers at 0x47 | hardware-readiness-20260920, port-status | **Nobody is working on it.** Check the stock DT for the supply/reset/IRQ (vdd, vio, any enable GPIO), probe with the rails on, and try `stk3310.ko` (ID acceptance). Needed for in-call screen-off and auto-brightness | 2–4 days | med | **Y** |
| H41 | Hall sensor (gpio75, flip cover) | **P~** stuck high / **B** rest2 hall | misc §4 | rest2 MODE=hall with a strong magnet / the flip cover; the Android lid RRO is optional | 0.5 day | low | O |
| H42 | Step counter/significant motion (SSC virtual sensors) | **N** | hardware-readiness (stock fusion from the ADSP) | Skip; Android derives what it needs; SMGR fusion ports are out of scope | — | low | O |
| H43 | Fingerprint (Sunwave, QSEE TA) | **dropped** by Pierre | rest-20260924 §4 | none | — | — | no |
| H44 | NFC | **not fitted** | rest §7 | none | — | — | no |
| H45 | USB device: ADB | **P** (recovery; enumeration flaky after warm reboot) | usbfix-20260925 | V75-usb watchdog (optional); ROM `init.a6l.usb.rc` `soft_connect` gate on the real eMMC | 1 attended session | med | Y |
| H46 | USB MTP / file transfer | **B** (gadget HAL fork, **not in any variant**: it conflicts with the a6l_manual_usb gate) | hals §USB, full.mk comment | Merge the gadget HAL with the soft_connect gate (or drop `a6l_manual_usb=1` in the ROM); test "File transfer" | 1–2 days | med | Y |
| H47 | USB OTG / host | **N** | rest §7 | PM660 Type-C detection + OTG VBUS + dwc3 role switch driver | 2–3 days | med | O |
| H48 | Charging (PM660 SMB2) | **B** (`a6l-charger-v75`, 1.95 A, no JEITA); **not in the rom-v1 DT** | rest §6 | Attended C2 (current-affecting). Then **software JEITA** (thermal trip or qcom_smbx patch) before unattended charging | 2–3 days | **high** (battery safety) | **Y** |
| H49 | Off-mode charging (plugged in while off) | **N** | (none) | Check what the ABL does (`androidboot.mode=charger`?) with our boot image; Lineage charger or just boot fully | 1–2 days | med | Y |
| H50 | Fuel gauge / battery % | **P** (readings, V45 and 21 Sep) | port-status | Health HAL in the ROM: % matches sysfs; the capacity-learning accuracy of pmi8998_fg on PM660 | 1 day | low | Y |
| H51 | Thermal | **B** (tsens zones present; the thermal HAL reports fake values) | rest §6 | The thermal HAL reads the real zones; battery-temperature trip; CPU cooling needs cpufreq | 1–2 days | med | Y |
| H52 | CPU frequency (CPR3/CPRh + OSM) | **B** (ported, Image.gz built, open-loop, capped 1536/1747 MHz) | rest §1, cpufreq-assessment-20260920 | **Do not use MODE=fuses** (it reset the phone on 24 Sep). Take the CPR fuse values from stock dumps (root on stock: `/sys/kernel/debug` or the stock kernel's CPR logs) → attended RAM boot, Pierre only | 3–5 days attended | **high** (voltage) | R (performance, thermal, battery) |
| H53 | Suspend (s2idle) + wake sources | **N** on the phone (QEMU only) | rest §6, flash §9 | Installed ROM: screen off → `cat /sys/power/suspend_stats`, wake by power key, alarm (pm8xxx RTC), incoming SMS/call (modem SMP2P/GLINK), charger plug | 1 week | high | **Y** |
| H54 | RPM deep sleep (XO/CX shutdown), battery life | **N** | rest §6 | After H53: measure the drain (overnight) and find the votes that block XO shutdown | 1–2 weeks | high | Y |
| H55 | Video codecs (venus) + Codec2 HAL | **N** | (none) | Check mainline venus support for SDM660 (HFI 4xx), the firmware from stock `venus.mbn`; an Android Codec2 v4l2 HAL (the ChromeOS v4l2_codec2 approach). Until then the software codecs work (CPU heavy) | 2–3 weeks | high | R |
| H56 | DRM / Widevine | **N** | (none) | Lineage has ClearKey only. Widevine L3 needs a prebuilt `drm-service.widevine` blob (a legal/licensing question); L1 is impossible without QSEE | 1 day (L3 blob) | med | O (streaming apps) |
| H57 | Keymint / Gatekeeper | **B** (software keymint "nonsecure" in the vendor; QEMU) | flash §3, realinit | Installed ROM: set a PIN, reboot, unlock; the Keystore works. No hardware-backed keys (no QSEE) | 1 day | low | Y |
| H58 | Storage /data (ext4, formattable) + /metadata | **B** (QEMU r8) | flash §9 | First install: 107 GiB format, fstrim, performance | 0.5 day | low | Y |
| H59 | Encryption (FBE / metadata encryption) | **N** | flash §2 alt D | fscrypt v2 + metadata encryption with software keymint (`fileencryption=aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized` without inlinecrypt) | 2–3 days | med | R (a lost phone = readable data) |
| H60 | microSD slot | **unknown** (stock DT enables sdhc_2, cd-gpio54; `ro.build.characteristics=nosdcard`) | rest §7 | **Pierre: look in the SIM tray.** If present: overlay + vold adoptable/portable | 0.5 day | low | O |
| H61 | Notification LED | **N** (nobody checked) | (none) | Check the stock DT (qcom,leds / pm660l RGB) and the stock `/sys/class/leds`; if present: lights HAL notification | 0.5 day | low | O |
| H62 | RTC / alarms, NITZ time | **N** | (none) | pm8xxx RTC wakealarm in the installed ROM; NITZ from the RIL | 0.5 day | low | Y |
| H63 | Reboot / power-off / reboot-to-recovery/bootloader/EDL | **P~** (`reboot bootloader` works from the kernel) / `reboot edl` **N** | flash §5 | PON reason for EDL (qcom,pon `reboot-mode` edl = 0x01 on PM660?) → also closes finding 3 | 1 day | med | Y |
| H64 | Hardware watchdog | **N** | port-status ("production config, watchdog") | Enable the APSS WDT (`qcom-wdt`) + Android watchdogd | 0.5 day | low | R |

## 3. Software, system and release

| # | Item | Status | Evidence | Next concrete step | Estimate | Risk | DD |
|---|---|---|---|---|---|---|---|
| S1 | Real Android init + vendor image, boot from the eMMC | **B** (kit-r8: QEMU boot_completed + 11 min stable; captured-ABL emulation PASS) | flash-20260924 §9 | Stage kit-r8 on the laptop → `Verify-RomV1Stage.py` → attended install (after S14) → H1–H10 of flash §7.3 | 1 attended session | med | Y |
| S2 | **rom-v2 integration** (a single V75 kernel + DT + vendor with all the deliveries) | **N** (nobody owns it) | this review §0.1 | Flash agent: V75 kernel = 7.2.3 + q6asm xlate + q6routing per-direction + q6voice + camss patches + haptics + ipa2_lite (if it passes); V75 DT = base + speaker/charger/haptics/hall/flash/camera/voice(/cpufreq off); vendor = radio.mk, eink.mk + composer patch, sensors multihal, gadget HAL, VibratorOL, q6voiced, full variant; QEMU r-run + ABL emulation | 3–5 days | med (every added module can break the boot) | **Y** |
| S3 | Production kernel config / cmdline | **N** | flash §3 (cmdline keeps `a6l_probe=1`, `panic=0`, `loglevel=6`, earlycon, `*_ignore_unused`) | `panic=5`, `loglevel=4`, drop `clk/pd/regulator_ignore_unused` once the drivers vote properly (it matters for sleep!), watchdog, drop `a6l_manual_usb` once USB is reliable | 2–3 days (the ignore_unused removal is iterative) | med | Y |
| S4 | SELinux enforcing | **N** (every fragment is written for permissive; hals sepolicy never compiled; eink sepolicy compiles monolithically) | hals §sepolicy, eink3 | `m selinux_policy` with all the fragments → boot permissive → collect avc denials in the installed ROM → fix → `androidboot.selinux=enforcing` | 1–2 weeks | med | Y (security, some banking apps) |
| S5 | E-ink Android integration (eink3: lease from drm_hwcomposer, a6l_epdd v4, mirror v2, key 616, rear-touch uinput) | **B** (unit 50/50, e2e 14/14; never on msm) | eink3-20260924 | Include it in rom-v2; attended eink3 §7A (RAM) and §7B E1–E14 (installed) | 1 week | med (the lease under a live LCD is unproven) | Y |
| S6 | Dual-screen switching + e-ink settings UI | **in progress** (dualux agent; no doc seen yet) | (dualux) | Deliver: a mode switch (LCD / e-ink / mirror), a Settings tile/page, per-app e-ink, a reading mode; rear-touch face switching and inactive-face rejection | 1–2 weeks | med | Y |
| S7 | HAL inventory (see §3a) | mixed | hals-20260924 | Put every HAL in one variant; `lshal`/`dumpsys` check on the phone | — | — | Y |
| S8 | Audio HAL (AIDL example + a6l-audio-route + mixer paths + policy) | **B** (full variant) | audio3-20260924 | Installed ROM: media playback on the headset, then the speaker; in-call routing hooks (`vendor.a6l.voice.active`) | 3–5 days | med | Y |
| S9 | Telephony framework (the radio HAL declared, carrier config, APNs) | **B** (HAL m rc=0, 105+131 host tests) — **not in the ROM** | ril-20260924, misc | Add radio.mk + BoardConfig-radio.mk to rom-v2; `telephony.*` features; the Orange F APN/carrier config | with S2 + H17 | med | Y |
| S10 | OTA / updater | **N** | (none) | Choose a path: (a) LineageOS non-A/B OTA installed by a recovery that has our kernel, or (b) a laptop EDL "update" tool (the current installer with `--allow-nonstock` + preserve userdata). Lineage Updater needs signed builds + a server; (b) is realistic first | 1–2 weeks | med | R |
| S11 | Recovery able to install (sideload, wipe, OTA) | **N** (the recovery slot holds the V74 diagnostic recovery) | flash §2/§7.4 | Build a Lineage recovery (`recoveryimage`) with the V74 kernel + sdhci-msm + touch/display; keep V74 as a debug image elsewhere | 1 week | med | R |
| S12 | Build signing (release keys, `user`/`userdebug`) | **N** (test-keys userdebug) | (none) | Generate the keys, `sign_target_files_apks`; decide user vs userdebug (adb root is needed during bring-up) | 1–2 days | low | R |
| S13 | GApps / Play Integrity | **N** (no plan) | (none) | Decide: none / MicroG / MindTheGapps (Android 16 arm64). Expectation: with an unlocked bootloader, test-keys and software keymint, Play Integrity "device"/"strong" **fails**; "basic" at best. Banking/wallet apps may refuse. Pierre should know this before choosing the ROM as a daily driver | 1–2 days to integrate | med | Pierre's choice |
| S14 | Backup / restore to stock (EDL, byte-exact) | **B** (Test-RomV1Flash 8/8 on a virtual eMMC; Verify-RomV1Stage PASS) — **kit not on the laptop** | flash §5, §9.4 | Stage kit-r8 → Verify on the laptop; **decide alternative A (full userdata backup, ~1.5 h)**; **prove EDL entry from a non-stock state** (finding 3) | 0.5 day + attended | **high** if the rollback path is untested | Y |
| S15 | Installer + rollback UX (one command each) | **B** (Launch-RomV1Install/Restore) | flash §7 | A dry-run on the real phone = backup-only mode? (not implemented: consider `--backup-only` to rehearse EDL without writes) | 1 day | med | Y |
| S16 | V75-usb recovery (USB enumeration watchdog) | **B** (image sha 8ecb8e9e…; captured-ABL PASS; mock 7/7) — **install tools NOT generated** | usbfix-20260925 | Generate the Prepare/Stage/Run install tools (the V74C pattern) before it can be installed; then sysrq-reboot 3–5× | 1 day + attended | low | O (it makes the bring-up faster) |
| S17 | Persist / modem EFS policy | **B** (persist ro; rmtfs read-only EFS by default) | flash §4 | Decide `rmtfs_rw` after a stable modem in the ROM (the EFS holds the NV/IMS/carrier state) | decision | med | Y |
| S18 | Performance tuning (dex2oat, zram, lmkd props, schedutil) | **N** | (none) | After cpufreq: zram + lmkd props for 4 GB, the dex2oat thread count | 1–2 days | low | R |
| S19 | Documentation for Pierre (install, rollback, enable the radio, e-ink key, known issues, recovery from a hang) | **N** as a user guide (only agent docs) | (none) | Write `docs/user-guide.md` once rom-v2 exists | 1 day | low | Y |
| S20 | Regression matrix / test automation on the phone | **N** | roadmap §4 | A script that checks every A6L_* marker after the boot (a `bugreport`-like dump) | 1–2 days | low | R |
| S21 | Repo: commit 22–25 Sep work; refresh port-status.md | **N** (last commit 21 Sep, 153 entries pending) | `git status` (today) | Pierre / main agent: commit per area; update port-status from this doc | 1 h | **high** (data loss) | Y |
| S22 | Kernel maintenance (7.2.x updates, out-of-tree patches rebased, Lineage monthly merges) | **N** (no process) | (none) | Keep the patches as a series in `device/hisense/a6l/kernel/`, with one rebuild script | ongoing | med | R |

### 3a. HAL list (target rom-v2) and where each one stands

| HAL / service | Source | In the ROM today? | Proven? |
|---|---|---|---|
| composer (hwc3 drm + e-ink ignore/lease patch) | realinit + eink3 patch | hwc3 yes; patch **no** | UI on the phone from RAM (21 Sep), not from the eMMC |
| graphics allocator/mapper (minigbm msm) | realinit | yes | from RAM |
| EGL/GLES Mesa freedreno | V71 payload | yes | from RAM |
| Vulkan | none | no | **N** |
| audio (AIDL example + a6l-audio-route) | audio3 | full variant only | no |
| a6l-q6voiced (voice session) | kvoice | no | CLI equivalent in recovery |
| radio (android.hardware.radio-service.a6l, V4) | ril/misc | **no** | no (the CLI is proven) |
| GNSS (AIDL V7 over QMI LOC) | gnss | yes (base) | QEMU registration only |
| Wi-Fi (android.hardware.wifi-service + wpa_supplicant) | hals | full only | no (legacy HAL fallback question open) |
| Bluetooth (bluetooth-service.default, HCI_CHANNEL_USER) + a6l_macs | hals | full only. The rc starts `/vendor/bin/a6l_macs`, but `full.mk` does not list it in PRODUCT_PACKAGES: check that the binary is really in the vendor image | no |
| Bluetooth audio (A2DP software) | AOSP | unclear | **N** |
| sensors (multihal + trout IIO sub-HAL) | hals | **no** | no |
| lights (lineage) | hals | full only | no |
| vibrator (QTI VibratorOL) | rest | **no** | no |
| health | example | yes | no (battery readings proven in sysfs) |
| power, thermal | example | full only | no (thermal = fake values) |
| USB (usb-service.basic) + USB gadget (a6l fork) | hals | **no** (it conflicts with the manual USB gate) | ADB only |
| keymint / gatekeeper (software) | realinit | yes | QEMU |
| camera (libcamera Android HAL) | cam | **no** (not built) | no |
| drm (clearkey), Widevine | AOSP / none | clearkey only | — |
| memtrack, PowerStats | none | no | noise only |
| e-ink: vendor.a6l_epdd, vendor.a6l_eink | eink3 | **no** (only the manual `/vendor/a6l/epd` payload) | v2/v3 from RAM |

---

## 4. Things nobody has started, or that are missing from every plan

Nobody has started these, and no plan even mentions them:
- In-call **front proximity** (STK3338 never answers) → screen-off at the ear, auto-brightness (H40).
- **Rollback from an installed ROM** (EDL entry without stock, `reboot edl`, a rehearsal of Plan B) (S14, H63).
- **Off-mode charging** behaviour (H49).
- **Headset button** (H14), **main-mic-without-headset** test (H13), **incoming call/ringtone** (H21).
- **Emergency call** review (H23).
- **A2DP / HFP** Bluetooth audio (H31).
- **Vulkan**, **venus/Codec2**, **Widevine** (H3, H55, H56).
- **FBE encryption** (H59), **notification LED** check (H61), **hardware watchdog** (H64), **RTC alarms** (H62).
- **Production kernel config/cmdline** (S3); the removal of `*_ignore_unused` matters for battery life.
- **OTA/updater, installable recovery, release signing, GApps/Play Integrity decision** (S10–S13).
- **User guide** (S19), a **regression matrix** (S20), **kernel maintenance** (S22).
- **rom-v2 integration** (S2): each agent delivered "for flash", but nobody merged it.

Known but not started: VoLTE/IMS (H25), USSD/SUPS (H24), MMS (H19), OTG (H47), hotspot (H29), SMS in the Messaging app
through the HAL, a microSD overlay (waiting for Pierre).

Questions for Pierre (they block choices): (1) Does the rear screen have a frontlight? (2) Is there a microSD slot in
the SIM tray? (3) Install layout: default, or alternative A (a full userdata backup, ~1.5 h)? (4) Is Wi-Fi-only
acceptable as a first daily-driver milestone, if IPA takes weeks? (5) GApps: none, MicroG or MindTheGapps? (6) Does he
know the hardware EDL entry (key combo or test point) used on 14 Sep? (7) Can the modem EFS be read-write in the ROM?

---

## 5. Attended test queue for tomorrow (25/26 Sep), in order, with exact bundles

Base: laptop `~/A6L-usb-20260915`. Phone: V74 recovery, **fresh boot** (long-press Power if USB does not enumerate). Setup:
`.relay/prep.sh` as usual (toybox links in `/tmp/bin`, sdhci-msm). Every phone line starts with `export PATH=/tmp/bin:$PATH;`.
Pierre types the RF lines himself and appends `2>&1 | grep -v linker`. Before long kernel-log phases, tell Pierre, and
wait for his "ready" before any step that needs him.

**0. (Optional, Pierre decides) V75-usb recovery** (`v75usb/image`, sha 8ecb8e9e…). **Not ready:** the install tools are
not generated (usbfix doc §"Not generated"). Skip it tomorrow, unless an agent generates and offline-tests the
Prepare/Stage/Run tools first. The V74 workaround is to replug at the phone, or long-press Power.

**Boot 1 (fresh V74):**
1. **audio4, no RF, before the ADSP.** `adb -s HLTE730T-PROBE push v75/audio4 /tmp/audio4`
   - `D=/tmp/audio4 MODE=ovl sh /tmp/audio4/run.sh` → `A6L_AU4_OVL_PASS` (it must run BEFORE the ADSP starts)
   - ADSP bundle (v68, modules + `echo start`, as in the audio2/audio3/kvoice sessions)
   - `D=/tmp/audio4 MODE=load sh /tmp/audio4/run.sh` → `A6L_AU4_LOAD_PASS`, `A6L_Q6ROUTING_PATCHED yes`
   - `D=/tmp/audio4 MODE=tone sh /tmp/audio4/run.sh` (headset NOT worn, −30 dB) → `A6L_AU4_TONE_PASS`, no `0x10325`
   - `D=/tmp/audio4 MODE=media sh /tmp/audio4/run.sh` → `A6L_AU4_MEDIA_PASS`
2. **rest2 haptics + hall (no RF).** `adb -s HLTE730T-PROBE push v75/rest2 /tmp/rest2`
   - `export D=/tmp/rest2 MODE=haptics; sh /tmp/rest2/run-rest2.sh`: Pierre reports after each pulse whether he felt it; read the 0x0A/0B/0C snapshots
   - `export D=/tmp/rest2 MODE=hall; sh /tmp/rest2/run-rest2.sh`: a strong magnet or the flip cover over both halves; pass = level toggles + IRQ count rises
3. **radio2 KEEP (RF, Pierre's go, Pierre types it).** `push v74/radio2 /tmp/radio2`, `push v75/ril2 /tmp/ril`, then
   `export A6L_RF_APPROVED=1; export D=/tmp/radio2; export A6L_WATCH=40; export A6L_KEEP=1; export A6L_WIFI_SCAN=0; sh /tmp/radio2/run.sh`
   → `crashes=0`, `A6L_KEEP=1: modem and daemons left running`. Then the PIN/online steps from the 24 Sep ril-test flow if needed.
4. **ril2 SMS decode.** `sh /tmp/ril/ril-test.sh sms-listen 180` → Pierre sends an SMS → `A6L_QMI_SMS_RX … form=bare-tpdu … text='…'`
5. **ril2 DTMF + audio4 call gains (RF, own number/voicemail only).**
   `export A6L_RF_APPROVED=1 A6L_DIAL_TO=<num>; sh /tmp/ril/ril-test.sh dial-dtmf <num> 1234# 40` → `A6L_QMI_DTMF_PASS digits=5`.
   During an active call, in a 2nd shell: `D=/tmp/audio4 MODE=call CVD=0 GAIN=0 SECS=60 sh /tmp/audio4/run.sh`, then
   `GAIN=3` and `GAIN=6` only if Pierre wants louder (the cap is raw 90). Then `sh /tmp/ril/ril-test.sh call-list`.
   **New, not in any doc yet:** if time allows, an **incoming call** from Pierre's other phone with the CLI's call-wait/answer (H21).
6. **gnss2 XTRA (modem still up; phone outside, open sky).** `push v75/gnss2 /tmp/gnss2`;
   `adb shell "date -u $(date -u +%m%d%H%M%Y.%S)"`; `sh /tmp/gnss2/gnss2-test.sh query`;
   `sh /tmp/gnss2/gnss2-test.sh xtra <lat>,<lon>,5000 600` → `A6L_GNSS XTRA_OK parts=32`, then a FIX. The XTRA files expire
   ~7 days after 24 Sep. Put Pierre's everyday A6L next to it for a C/N0 comparison.
7. **camera3 LAST in this boot (it may still oops).** `rm -rf /tmp/camera3`; `push v75/camera3 /tmp/camera3`;
   `D=/tmp/camera3 MODE=probe sh /tmp/camera3/run-camera.sh` → `CAMSS_CAMFIX2_PASS`;
   `D=/tmp/camera3 MODE=bars SENSOR=imx576 sh /tmp/camera3/run-camera.sh`; `pull /tmp/cam-imx576-bars.dmesg v75/logs/`.
   Pass = no `KERNEL_OOPS_FAIL`; ideally `CAPTURE_imx576_bars_PASS`. If that works, repeat with s5k3t1 and hi846, then `MODE=off`.
   After an oops: stop and reboot (run-camera refuses to continue anyway).
8. `pull /tmp/audio4-out v75/logs/audio4-out` (and the ril/gnss logs) **before** the reboot.

**Boot 2 (fresh V74, its own boot; it may reset the phone):**
9. **ipa2b step walk.** `push v75/ipa2b /tmp/ipa2b`; **first** `adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; dmesg -w' > v75/logs/ipa-klog.txt &`;
   then for N in **1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0**:
   `export D=/tmp/ipa2b; export MODE=load; export STOP_AT=N; sh /tmp/ipa2b/run.sh` → `A6L_IPA2_STEP_PASS`. On a reset, the last
   `A6L_IPA_STEP` in ipa-klog.txt is the culprit. Retries: `NOIOMMU=1 STOP_AT=13` (SMMU attach), `IDENTITY=1` (first DMA at step 13).
   After `A6L_IPA2_LOAD_PASS`: radio2 KEEP (RF), then MODE=status / MODE=data per ipa-20260924.

**Not tomorrow, but the next attended block:** stage kit-r8 and prove the rollback path (S14/H63) → first eMMC install (S1)
→ charging check (H48) → C2 → cpufreq (H52, Pierre only, no MODE=fuses).

---

## 6. Evidence index
Project: claude/attended-20260925, fixes-20260924, attended-20260924, fullphone-20260924, camfix/cam-ipa-20260924, morning-20260924,
prep-20260923, session-notes. Repo docs: port-status (stale), flash-20260924 (§1–§9), hals-20260924, rest-20260924, ril-20260924,
misc-20260924, kvoice-20260924, audfix-20260924, audio3-20260924, gnss-20260924, ipa/ipa2fix-20260924, cam/camfix-20260924,
camfix2-20260925, usbfix-20260925, eink3-20260924, realinit-progress-20260923, roadmap-to-working-image-20260920,
hardware-readiness-20260920. Checks made today: `device/hisense/a6l/rom/{rom.mk,full/full.mk}`, the laptop listing (relay todo-02:
v75/{audio4,rest2,ril2,gnss2,camera3,ipa2b} and v75usb/image present, rom-v1 absent), `git status` (153 entries, last
commit 21 Sep).
