# Android-side config: overlays, carrier/APN, Updater, GApps (29 Sep 2026)

Worker: overlays-carrier-updater. Offline only: no phone, no adb, nothing built with `m` (r5 build running). Lands in the
next build (r6); r5 has none of this except the r6-independent parts listed as "already in r5".

## What is ready

All under `device/hisense/a6l/rom/android/` (copied whole into the tree by `tools/stage-rom-v2-prebuilts.sh`), inherited
by `rom/rom.mk` -> `$(call inherit-product, device/hisense/a6l/rom/android/android.mk)` (last line).

| Area | File | Value / reason |
|---|---|---|
| Navigation | `overlay/.../values/config.xml` | `config_showNavigationBar=true` (no HW keys; AOSP default false). 3-button default, gestural selectable |
| Display cutout | same | stock notch, not "none": stock reported `boundingRect=Rect(420,0-660,75)` on 1080x2340 (stock overlay: 80x25 dp @480). Pixel path `M -120,0 L 120,0 L 120,75 L -120,75 Z` + rect approximation |
| Doze | same | `config_enableAutoPowerModes=true` (AOSP false = no Doze), motion gate off (no significant-motion sensor); no AOD / pickup / double-tap |
| Brightness slider | same | **stock** min 1 / max 255 / default 77 / dim 1 / doze 17 (from stock framework-res via aapt2) |
| Auto-brightness curve | `rom/r6/overlay/.../config.xml` (r6 owns the key) | replaced the r6 first cut by the **stock curve**: 31 lux levels 2..9534, 32 backlight values 3..255 |
| Battery 3800 mAh | `rom/r6/overlay/.../power_profile.xml` | already there (r6); checked |
| Density / size | `rom/rom.mk` | `ro.sf.lcd_density=400` (unchanged; physical 397 dpi, stock used 480) |
| Wi-Fi dual band | RRO `A6LWifiOverlay` (com.android.wifi.resources, APEX: static overlays cannot reach it) | `config_wifi5ghzSupport=true` (AOSP false), SoftAP 2.4+5 GHz, no 6 GHz/bridged, ACS off, connected-MAC randomization off (both unvalidated on ath10k) |
| Hotspot | overlay comment | Lineage `net.tethering.noprovisioning=true`, default tether regexes match wlan/rndis/ncm; no provisioning app |
| Fast-charge label | RROs `A6LSystemUIOverlay` + `A6LSettingsOverlay` (SettingsLib ints) | slowly < 5 W, fast > 9 W (v1 and v2): 5 V/1.5 A DCP = regular, HVDCP 9 V or >= 5 V/2 A = fast |
| Carrier config | RRO `A6LCarrierConfigOverlay` (CarrierConfig `res/xml/vendor.xml`, applied after the carrier asset) | all SIMs: VoLTE/VT/WFC unavailable, 4G-calling toggle hidden, SS over CS (no Ut/XCAP), IMS APN hidden. Orange France = carrier id 32 asset, used unmodified |
| APN list | Lineage `vendor/apn` world list (already in r5: /product/etc/apns-conf.xml) + tree patch `patches/vendor/apn/0001-FR-Orange-World-IPV4V6.patch` | Lineage has Orange World `orange` + `ia` (208-00/01/02) **IPv6-only, no roaming protocol**. Only an IPv4 call is proven (data3/ipa4) and 464XLAT is untested -> IPV4V6 home+roaming (stock used IPv4). Applied by `tools/rom-v2-pipeline.sh` prep (new idempotent loop over `rom/android/patches/<project>/`). Not a second module: two modules installing the same path collide in Kati (Soong emits install rules for every module; `overrides` only drops it from the product list). Generator: `apn/a6l_apn_fixup.py` |
| Emergency numbers | none needed | radio HAL reports 112/911 (SOURCE_MODEM_CONFIG); the framework ECC database (TeleService eccdata) has FR 112/15/17/18/115/119/... |
| Updater | `android.mk` | non-release builds: `lineage.updater.uri=.../updater/unpublished/{device}.json` (deliberately never published: no release-key OTA offered to test-key images, no polling of download.lineageos.org). Release (`A6L_RELEASE=1`): unchanged real URI from `rom/release/release.mk` |
| GApps | `rom/rom.mk` | see below; new knob `A6L_NO_GAPPS=1` |

## GApps plan

- Already in r5: MindTheGapps `cinnamonbun` (Android 17; gitlab.com/MindTheGapps/vendor_gapps, commit bfa45ac3, 30 Jun 2026)
  is moved into the tree as `vendor/gapps` by the pipeline and inherited from `rom/rom.mk` when present (Pierre: required).
- Licensing: the `proprietary/` APKs are Google's closed-source files (MTG LICENSE: "I do not own them"). A build that
  contains them is fine for Pierre's own phone; do **not** publish such an image or OTA publicly. For a public release,
  build with `A6L_NO_GAPPS=1` (new, `rom/rom.mk`) and let users install MindTheGapps themselves (Lineage recovery
  sideload in release builds). Nothing was downloaded by this worker.

## Checks (all PASS)

- `bash rom/android/tests/check-android-overlays.sh /home/a6l/android/a6l-lineage24` -> 95/95 ANDROID_OVERLAYS_PASS
  (XML, one owner per framework key across rom/android + rom/r6 + audio + usb, values, product wiring lineage_gsi_a6l.mk ->
  rom.mk -> android.mk, PRODUCT_PACKAGES == Android.bp RROs, resource keys exist in the tree (framework, Wi-Fi overlayable,
  SettingsLib, CarrierConfigManager), aapt2 compile+link of the 4 RROs against android.jar, APN patch applies to
  vendor/apn, make-apns on the patched sources == fix-up of the r5 generated list, XSD valid, entry count unchanged, bpfmt).
- `rom/tests/test-rom-static.sh` PASS, `rom/r6/tests/test-r6-static.sh` PASS 0. No sepolicy change.
- NOT done: Soong/Kati evaluation of the new Android.bp/mk (no `m` while r5 builds): first r6 build is the proof.

## Attended tests after the first install

1. `cmd overlay list | grep -i a6l` : 4 RROs + framework-res auto RRO enabled; `dumpsys display | grep -i cutout` = 420..660 x 0..75.
2. Navigation bar visible, gestural switch works; status bar icons clear of the notch.
3. Brightness slider low end readable (min 1), auto-brightness walk (dark / office / window / sunlight) vs the stock curve; compare `dumpsys sensorservice` lux with a reference.
4. Wi-Fi: 5 GHz AP visible and joins; hotspot on 2.4 and 5 GHz (channel non-DFS); `dumpsys wifi | grep -i 5ghz`.
5. Charging label: SDP (PC) "slowly", 5 V charger "charging", QC charger "fast" (`dumpsys battery`: max charging current/voltage).
6. Orange SIM: `content query --uri content://telephony/carriers/current --where "apn='orange'"` shows protocol/roaming_protocol IPV4V6; data up, `ip addr` on rmnet_data*; IPv4 and IPv6 reachability; MMS send/receive.
7. `dumpsys carrier_config | grep -E "volte_available|ss_over_ut|hide_ims_apn"` = false/false/true; call forwarding menu works (CS).
8. Emergency: `dumpsys phone | grep -A5 EmergencyNumber` lists 112/15/17/18 (do not dial).
9. Settings > System > Updates: check fails/no update (placeholder URI), no crash.
10. Doze: `dumpsys deviceidle force-idle` then `unforce`; overnight drain with screen off.
