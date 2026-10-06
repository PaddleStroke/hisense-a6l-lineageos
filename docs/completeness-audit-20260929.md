# A6L completeness audit (29 Sep 2026, offline)

No phone, no adb, and no `m` were used. The r5 build was running the whole time. Three auditors checked the H1–H64
checklist, the Android-side product, and release readiness. This page merges their findings into one list without
duplicates, and records what I prepared offline for **r6**.

I found that several gaps were **already being closed by parallel workers today**: USB (`android-usb-20260929.md`),
Bluetooth audio (`android-bt-audio-20260929.md`), camera (libcamera + provider, `libcamera-plan-20260929.md`), HI846
(`hi846-20260929.md`) and the r5 build. I list those gaps below but did not duplicate the work.

## 0. Status in one line
No H item is proven **in Android** yet, because the ROM has never been installed. 26 items are proven in recovery or
RAM-boot (H1, H4–H10, H12–H18, H20, H26, H27, H30, H33, H34, H37–H40, H45, H48, H50). The first install depends on the
r5 kernel, which is built but not yet booted (ledger B3).

## 1. Merged gap list (priority, owner)

### P1: blocks daily use
| # | Gap | H / ref | State after 29 Sep | Owner |
|---|---|---|---|---|
| 1 | **First install with the r5 kernel**: RAM-boot r5 (module loads, usercopy), then install | B3, H2 | Kernel built, never booted | attended |
| 2 | **Debug defaults**: `persist.vendor.a6l.radio=0`, `persist.sys.usb.config=adb`, `charger=0`, `camera=0` | rom.mk, 0001 patch | `0001-release-defaults.patch` is not applied. New `rom/r6/patches/0002-r6-charger-default.patch` sets charger=1. **Correction:** with charger=0 the PMIC still charges, but only on hardware defaults (no stock limits, no software JEITA, no QC3). | **Pierre** decides |
| 3 | **No PIN or lock screen**: the Gatekeeper HAL is only in `release.mk` | release-prep §0 | **Prepared**: `rom/r6/r6.mk` adds `com.android.hardware.gatekeeper.nonsecure` to every build | r6 merge |
| 4 | **No USB in the installed ROM**: no gadget was ever created, so no adb, MTP, PTP or tethering. There were also no android_usb uevents. | H46 | **Done by android-usb**: `usb/` gadget HAL, configfs rc, UDC overlay and genfs, all merged into rom.mk | attended check |
| 5 | **Suspend and deep sleep never run on the phone**: power key, RTC, SMS/call, charger and proximity (F3) wakes, and overnight drain | H53, H54, H62 | **Prepared**: `rom/r6/tools/a6l-suspend-check.sh` (snapshot / wake-rtc / drain / delta) | attended |
| 6 | **No real thermal data or thermal shutdown**: no thermal HAL in rom.mk (the only one available was the example HAL with fake values) | H51 | **Prepared**: AIDL thermal HAL `rom/r6/thermal`, which reads the tsens zones and the fuel-gauge battery temperature. A parallel worker is extending it with cooling devices and IIO sources. | r6 merge |
| 7 | **Incoming call and ringtone** never run | H21 | HAL code exists | attended |
| 8 | **Emergency calls**: only tested against a mock modem | H23 | Offline flow review still to do (no SIM, locked SIM, no service). Never test live. | offline, open |
| 9 | **Camera and torch in Android** (provider, torch strength, `media_profiles`, feature XMLs) | H35, H36 | Being done: libcamera, provider fork, HI846 | camera workers |
| 10 | **Update path wipes data**: the recovery slot is V74, and every install goes through EDL with a userdata wipe | release-prep S11 | Open: "preserve userdata" mode for the EDL kit, `--backup-only` rehearsal (S15) | offline, open |
| 11 | **Rollback not proven**: kit not on the laptop, EDL from a non-stock state unproven | H63, S14 | Open: stage kit r5, `Verify-RomV1Stage` + `Test-RomV1Flash` against r5 | offline + attended |
| 12 | **All HALs unproven in Android**: radio, Wi-Fi (F5), sensors, audio routing (F7/F62), vibrator, lights, health, GNSS | — | Validation pass after the install | attended |

### P2: important
| # | Gap | H / ref | State | Owner |
|---|---|---|---|---|
| 13 | **Bluetooth audio**: no `bluetooth.profile.*` (no profile would start), no SCO path. **Correction:** the audio APEX already ships the BT audio provider. | H31 | **Done by android-bt-audio** (`audio/bluetooth/bt-audio.mk`, software A2DP + HCI SCO) | attended |
| 14 | **Framework overlays**: auto-brightness, `power_profile.xml`, VoLTE/VT/WFC unavailable, notification LED | H1 | **Prepared**: `rom/r6/overlay` (the USB flag is in `usb/overlay`) | r6 merge |
| 15 | **No cpufreq**, so passive trips have no cooling device and there is no DVFS | H52, F2a | Not merged. CPR/OSM voltage work needs Pierre present. | **Pierre**, attended |
| 16 | **SELinux permissive**: `rc-enforcing.patch` not applied, `a6l_logcat` runs as su, `/dev/dri` is 0666 | release-prep §3.2 | The policy passes the offline check (below). Apply the patch for r6+. | merge |
| 17 | **Signing keys**: builds use test-keys, and switching keys later needs a wipe | S6 | Scripts are ready. Subject, password and backup location are for **Pierre** to decide. | **Pierre** |
| 18 | **FBE / metadata encryption**: r5 has FS_ENCRYPTION, but dm-default-key is missing | H59 | FBE trial after the r5 boot | attended |
| 19 | **xt_quota2 missing**: netd data warnings and limits fail | H26, B3 | Port the ACK `xt_quota2.c` as an out-of-tree module (MODVERSIONS) | kernel, open |
| 20 | **Data through netd, MMS, USSD** | H19, H24, H26 | MMS never tried. USSD not implemented. | radio, attended |
| 21 | **Fast charge in the ROM**: `hvdcp_enable=0` | power29 | power29 fixes not merged | merge |
| 22 | **Headset button keycodes in Android** | H14 | IRQs seen in recovery | attended |
| 23 | **DEVMEM=y / a6l_mmio / debug adb** in release | kernel-android-config | Release config fragment still to write | kernel, open |
| 24 | **Blob licensing**: the OTA would ship stock firmware on a public release | S11 | NOTICE + blob list to write. Public or private hosting is **Pierre's** call. | **Pierre** |

### P3: nice to have
| # | Gap | Notes |
|---|---|---|
| 25 | H64 hardware watchdog | r5 has `CONFIG_QCOM_WDT=m`, but `sdm630.dtsi` has **no watchdog node** and the module is not staged. Needs a DT node (APSS WDT), `qcom_wdt.ko` in the base group, and watchdogd. |
| 26 | H61 notification LED | The stock kernel has `qpnp_rgb_set` (leds-qpnp built in), but that is not proof an LED is fitted. Check the stock `/sys/class/leds` or the DT. The overlay keeps the LED setting off. |
| 27 | H41 hall sensor | Test with a magnet. The sysfs value reads stuck high. |
| 28 | H47 OTG, H29 hotspot | Hotspot test in `wifi-hal-20260929.md` §5. The Wi-Fi resource RRO (5 GHz SoftAP) does not exist yet. |
| 29 | H55 Codec2 (venus), H3 Vulkan, H56 Widevine | Software codecs only. No Vulkan on a5xx. No Widevine. |
| 30 | Off-mode charger UI | The `on charger` module chain exists (H49). Lineage charger images are in the tree, but the charger UI binary is not in `system/bin` for r5. |
| 31 | Lineage DeviceSettings for e-ink modes | Only the `A6LDisplaySwitcher` app and tiles exist |
| 32 | OTA updater JSON, user guide (S19), boot time / ANR baseline, H28 Wi-Fi MAC | Documentation and release work |

N/A: H43 fingerprint (dropped), H44 NFC (not fitted), H11 earpiece (physical fault). H25 VoLTE is blocked by Orange
(network side).

## 2. Prepared offline for r6 (`device/hisense/a6l/rom/r6`, inert until applied)
- `r6.mk` adds: Gatekeeper (nonsecure), thermal HAL, overlay, and the suspend tool.
- `apply-r6.sh [--apply] [--defaults]` is idempotent and does a dry run by default. It does three things:
  - adds `inherit r6.mk` to rom.mk;
  - adds `rom/r6/sepolicy/vendor` to `A6L_SEPOLICY_DIRS`;
  - with `--defaults` only (**Pierre's decision**), sets charger=1.
- `thermal/`: an AIDL IThermal V3 service.
  - Config: `/vendor/etc/thermal-a6l.conf`, whose 9 zone types are checked against the r5 `sdm630.dtsi`.
  - Battery: `qcom-battery/temp`, with SEVERE at 45 °C and SHUTDOWN at 60 °C. The virtual SKIN sensor is the battery temperature.
  - A 5 s poller runs with 2 °C hysteresis and calls callbacks.
  - `soong_namespace`, so the module is inert until r6.mk includes it. `bpfmt` is clean. `clang -fsyntax-only` passed against the tree's V3 NDK headers (first version).
  - A parallel worker then added cooling and IIO support. The host test passes again after those changes.
- `sepolicy/vendor`: exec label, plus sysfs_thermal / batteryinfo read access.
- `overlay/`:
  - auto-brightness: 9 lux levels, curve still to be tuned;
  - VoLTE/VT/WFC set to false;
  - LED off;
  - `power_profile.xml` with 3800 mAh; the currents are estimates.
  - `aapt2 compile` passed and every resource name exists in `framework-res`.
- `tools/a6l-suspend-check.sh`: an on-phone script that reads suspend_stats, the top wakeup sources, qcom/rpm stats and the battery, with snapshot, wake-rtc, drain and delta modes.
- `patches/0002-r6-charger-default.patch` (not applied).
- `superseded/`: a pure-init alternative for USB (FFS MTP/PTP/rndis/ncm/midi with no gadget HAL). It is renamed `.txt` and kept only as a fallback if the android-usb gadget HAL fails.
- `tools/release/check-a6l-sepolicy.sh` is fixed so it no longer adds an extra dir twice. The r5 conf already contains `rom/sepolicy/vendor`, which made the mandated check fail with "Duplicate declaration of type".

Checks:
- `rom/tests/test-rom-static.sh`: PASS
- `rom/r6/tests/test-r6-static.sh`: PASS, 0 failures
- thermal host test: PASS (47 checks)
- `check-a6l-sepolicy.sh user rom/sepolicy/vendor`: PASS
- the same check with `rom/r6/sepolicy/vendor` added: PASS

## 3. Attended tests (need the phone; after the r5 or r6 install)
1. RAM-boot the r5 kernel, then do the first install. Confirm `getprop ro.boot` and module loads.
2. USB (android-usb): run `readlink -f /sys/class/udc/a800000.usb` to check the genfs path. Then test adb, MTP on Windows/Linux, PTP, and USB tethering (rndis/ncm).
3. Suspend: run `a6l-suspend-check.sh snapshot` → 10 min screen-off unplugged → `snapshot` → `delta`, then `wake-rtc 60`. Test wakes from the power key, an incoming SMS/call, a charger plug and proximity during a call. Run `drain` overnight and expect less than 1 %/h.
4. Thermal: `dumpsys thermalservice` should show real values for cpu, gpu, battery and skin. Heat the battery with a charge plus load, and check the status changes.
5. Lock screen: set a PIN, reboot, unlock.
6. Incoming call, ringtone, in-call routing (F7/F62), and the headset button keycodes (`getevent`).
7. Auto-brightness walk: dark, office, window, sun. Tune the curve.
8. Bluetooth: A2DP headphones, and an HFP call over HCI SCO.
9. Charger: install with charger=1, then test off-mode charging, QC3, and JEITA via the guard log.
10. Emergency-call **flow review only**. No live call.
11. H41 magnet, H61 `/sys/class/leds`, H47 OTG, H29 hotspot, GNSS outdoor fix (H32).

## 4. Needs Pierre
- Release defaults: radio=1, adb off, charger=1 (0001 + 0002). Camera stays 0 until the provider exists.
- Signing keys: subject, password, backup location. Also blob hosting: public or private.
- The cpufreq/CPR attended session (H52).
- Consent for the first ROM install and the rollback rehearsal. Keep V74 in the kit.
