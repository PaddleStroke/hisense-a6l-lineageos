# A6L hardware and Android integration review

Review opened 28 September 2026. Scope: all **H1–H64** entries in the master checklist, current device source, module/DT packaging, relevant Android HAL implementations, and available test reports. Companion to [the VoLTE/camera review](volte-camera-review-20260928.md).

**Follow-up:** [the second-pass review](hardware-review-pass2-20260928.md) adds F14–F22, with isolated reproductions for data/radio, call-audio, e-ink input and GNSS failure paths. It does not repeat F1–F13 or certify their fixes.

**Deeper audit:** [F23–F34 and the common regression plan](hardware-review-deep-20260928.md) cover request semantics, asynchronous ownership/order, SMS durability, sensor timing/calibration and GNSS cancellation. Its freshness notes also update the off-mode charging integration status.

**Main conclusion:** several hardware paths now work in recovery, but Android integration and failure recovery still contain blockers. The most important findings are charger policy/state handling, proximity wake, sensor recovery, the selected Wi-Fi HAL, call mute/routing, and the modem's IPA prerequisite. Each finding below includes a proposed fix and a way to validate it.

This was an offline review. No phone, RF, firmware, partition, voltage, active build, or Claude-owned implementation was changed. Added only this report, a cross-reference, and isolated reproduction tests. A source review cannot establish electrical behavior or certify every driver; “no new defect found” below means exactly that, not a hardware PASS.

## Evidence freshness

The checkout is changing concurrently. The integration ledger now contains **r5 / 29 September** results even though this review's session date is 28 September. I use those entries as newer *reported* evidence, preserving their dates; I did not independently repeat those phone tests. Some individual feature documents still say “untested.”

- [Current integration ledger](rom-integration-ledger.md): Wi-Fi WPA2/DHCP/Internet, IPA data, loudspeaker and motion sensors have reported attended progress on 27 September. Its r5 section reports MBHC v2 headset/microphone/button success, visible flash/torch, and microSD detection plus a 64 MiB read at 53 MB/s on 29 September.
- The same r5 section reports **VoLTE5 still failing**: modem closes service 770 after 0x2e/0x34, no IMS PDN. **Camfix6 was tested**, with bus errors and only a few completed frames. The corrected WM=6 sweep still failed (best iccmax 3 frames, burst3 1). Thus the previous review's “not phone-tested yet” statements are superseded; its proposed WM correction is already being used in the reported experiment.
- HVDCP remains idle with the reported Hisense 9 V charger. This is not a fast-charge PASS.
- The September 25 master checklist is useful as an inventory, but **must not remain the authority for current status**. Its “nobody started IMS,” “Wi-Fi scan only,” missing front sensor, wrong vibrator block, and absent ROM integration statements are obsolete. Conversely, a source merge or recovery PASS is not an installed-ROM PASS.

## Actionable findings

Paths below are relative to `device/hisense/a6l/` unless otherwise stated. P1 = resolve before relying on the affected function; P2 = correctness/recovery issue to resolve during integration. These are findings in the reviewed source, not newly observed phone failures.

### F1 — P1: thermal charging policy can increase the USB input limit

**Evidence:** `power/rom/a6l-chg-guard.sh:28–31,52–53`. Normal SDP/unknown-source limit is 500 mA, but cool/warm select 700/750 mA without taking the source limit into account. Running the unchanged guard against fake sysfs gives **SDP at 5 °C → 700 mA**, **SDP at 46 °C → 750 mA**; 25 °C correctly gives 500 mA. This is a requested-current policy violation; it does not prove a real port supplies that current.

**Fix:** compute `effective_icl = min(source_budget, thermal_budget)` in every temperature zone. Keep unknown sources conservative. Preserve any lower negotiated/AICL constraint rather than treating charger classification as permission to exceed it. Longer term, implement proper battery FCC/FV controls: USB input current is only an approximation of battery current and cannot represent the same warm/cool battery limit at both 5 V and 9 V.

**Validation:** SDP/CDP/DCP/unknown × cool/normal/warm/hot, unplug/replug and source changes; no requested ICL exceeds either applicable limit. Validate FCC/FV separately before enabling higher bus voltage. Do not infer full-current ROM validation from the lower-current attended charger tests.

### F2 — P1: charger suspend state is assumed, even when the write fails

**Evidence:** `power/rom/a6l-chg-guard.sh:27,37,54–55`. `susp` starts at zero and is changed regardless of the sysfs write result. A failed hot-zone suspend therefore prevents subsequent attempts while the same condition persists. Fault injection reproduced a failed first write, a recovered node, and **charging still commanded on after the next hot iteration**. A normal-temperature guard restart also issues no resume command when charging was previously suspended.

**Fix:** reconcile desired and applied state; only acknowledge a successful operation, retry failed writes with bounded logging, and publish errors instead of a fictitious applied state. On startup establish the actual USB-input-suspend state, or explicitly apply the desired state after validating telemetry. If necessary expose a dedicated readback of the suspend bit in the driver: the power-supply `status` text is not a faithful readback of a boolean input-suspend control. Keep hardware JEITA enabled independently.

**Validation:** failed suspend/resume writes, missing node then recovery, daemon restart while suspended, invalid/missing telemetry, and warm-voltage/overvoltage hysteresis. The offline harness emulates writes; it does not emulate the charger's electrical response.

### F3 — P1: front proximity is advertised as wake-up, but is shut down in suspend

**Evidence:** `hals/sensors/stk3338/sensors_a6l.c:45–49` advertises `SENSOR_FLAG_WAKE_UP`. `kernel/stk3338/stk3338_a6l.c:801–819` disables proximity interrupts and puts the device in standby at suspend. No wake-IRQ setup was found in that driver. A near/far event cannot be generated by a sensor in standby, irrespective of whether its IRQ line could wake the SoC. This affects screen behavior during a sleeping call, not the already reported awake IIO test.

**Fix:** implement a wake-capable proximity suspend path: establish device wake capability, retain PS sensing and its rail when required, configure the IRQ for wake, disable ALS separately, and restore/report pending state on resume. Tie sensor enable/disable to HAL clients. Do not simply remove the wake-up flag and call proximity complete. Kernel wake-IRQ handling is separate from ordinary interrupt delivery; see [Linux suspend and device interrupts](https://docs.kernel.org/power/suspend-and-interrupts.html).

**Validation:** near → screen off → actual suspend → far must wake/report correctly, with repeated cycles and measured idle current. Test calls and standalone proximity. An awake hand-over-sensor test is insufficient.

### F4 — P1: sensor recovery can wait forever after a read failure

**Evidence:** `hals/sensors/stk3338/sensors_a6l.c:293–324` computes `retry_ms` **before** `a6l_motion_service()`. A read error in `a6l_motion.c:747–763` then closes the device and schedules a retry. If there are no light deadlines, other sensor FDs, queued events, or control writes, the actual poll loop has only its control pipe and a timeout of **-1**, so the new retry never runs. The harness enters the real poll implementation, injects a read error, and observes `retry_pending=1 poll_nfds=1 poll_timeout=-1`.

**Related readiness defects:** `a6l_motion_activate():360` returns `-ENODEV` before remembering the requested activation if the IIO node is late. The advertised two-second retry never starts in that case (`enabled=0 retry_ms=-1`, reproduced). Front proximity's `open_events():126` returns no error to `activate()` when opening its event FD fails; the HAL reports activation success and never retries that FD in its poll loop.

**Fix:** derive the next retry deadline after all draining/error handling, using a dedicated deadline helper or a second reconciliation pass. Define a consistent late-device policy: retain a pending requested activation and retry asynchronously, or provide a supported availability/retry mechanism to the framework. Propagate or recover event-FD failures. Retain the initial proximity event until a valid reading is available.

**Validation:** HAL starts before ADSP; transient permissions; one active motion sensor loses its FD with ALS/proximity disabled; ADSP restart; proximity event-ioctl failure followed by recovery. Assert automatic recovery without a second Android activate request. The read fault in the harness is EISDIR, which follows the same handling branch as EIO/ENODEV; no actual DSP restart was performed.

### F5 — P1: the packaged Android Wi-Fi HAL selects an unsupported fallback

**Evidence:** `rom/BoardConfig-rom.mk` and `hals/BoardConfig-hals.mk` intentionally leave `BOARD_WLAN_DEVICE` unset. The actual local Lineage tree selects `libwifi-hal-fallback`; its `frameworks/opt/net/wifi/libwifi_hal/wifi_hal_fallback.cpp:19` returns `WIFI_ERROR_NOT_SUPPORTED` from `init_wifi_vendor_hal_func_table()`. `hardware/interfaces/wifi/aidl/default/wifi_legacy_hal_factory.cpp:111` rejects that result. This is stronger evidence than the old checklist's open question. Recovery's standalone supplicant/ath10k success does not exercise this path.

**Fix:** provide a working vendor-HAL implementation for the mainline nl80211 device, or deliberately implement the platform's supported path without a vendor HAL. Evaluate an existing generic implementation against this branch; do not assume selecting a Qualcomm proprietary HAL or the emulator library will work with ath10k. Implement initialization/interface lifecycle and accurately report supported modes/capabilities first. Check the final linked product, not just package presence.

**Validation:** real Android `IWifi.start`, STA creation, Settings toggle, WPA2 association, DHCP/Internet, off/on and suspend reconnect. Then validate hotspot/P2P and concurrency against `iw phy` interface combinations; the current dual-interface build flag is not proof of chipset/firmware support.

### F6 — P1: call mute reports success without applying it

**Evidence:** `radio/hal/RadioMessagingVoice.cpp:369–375` merely stores `mMute` and responds successfully. The comment still says q6voice is missing although it is now integrated. No corresponding mute operation was found in q6voiced or the route daemon. The selected local AOSP audio module's `Module::setMicMute()` also only updates its member variable; it does not establish DSP voice mute for this port.

**Fix:** implement mute on the actual q6voice/CVD TX path, or a verified route control that silences that uplink. Connect the Android audio/radio control paths to one applied state and return failures accurately. Restore state across call changes and DSP recovery; do not mute only the multimedia recording PCM.

**Validation:** during a consented ordinary call, remote-side audio must become silent on mute and return on unmute, on each supported route. Test repeated calls and DSP recovery. A successful response or changed mute icon is not a pass.

### F7 — P1/P2: Android route choices are not wired through, and failed routes are cached

**Evidence:** `audio/route/a6l_audio_route.c:108–121` chooses routes solely from jack state and `vendor.a6l.voice.active`. Calls use headset or earpiece; there is no requested speakerphone/Bluetooth route input. The file explicitly documents absent speakerphone support. Separately, `apply():59–72` logs mixer failures but returns void; the caller updates `last_hp/last_mic/last_voice` anyway, suppressing retries until a state change.

**Fix:** connect Android's chosen output/input device to the appropriate codec/DSP route. Add the TFA voice route before exposing working speakerphone; plan HFP/SCO separately. Make route application return success, commit cached state only after success, and retry/reopen the card after DSP/card loss. Preserve routing consistency during call start/stop and headset removal.

**Validation:** speakerphone button actually changes the remote/local audio route, no-headset calls on a healthy earpiece, headset hotplug, notification/ringtone routing, BT routing, and injected mixer failure with unchanged jack state. The broken earpiece on this test unit remains a hardware limitation, not a new software regression.

### F8 — P1: failed IPA readiness does not prevent modem startup

**Evidence:** `rom/bin/a6l-radio.sh:54–64` waits for the IPA binding and `rmnet_ipa0`, prints `NOT-BOUND` on timeout, then still loads the modem stack. Module insertion errors are also only logged. This violates the now-established prerequisite documented in the same script: IMS startup without AP IPA can assert the modem.

**Fix:** when IPA is required, make binding/netdev readiness a hard prerequisite **before loading any auto-starting MSS driver**. Propagate module errors and report a failed dependency state to init. Check rmtfs/tqftpserv readiness as well as diag-router. Keep any intentional IPA-disabled diagnostic mode explicit; do not treat unexpected failure as that mode. Use bounded retries that do not start multiple modem instances.

**Validation:** fake missing module, deferred IPA probe, missing netdev, and failed daemon. In every failure case assert the MSS driver/start operation is not reached; then verify normal ordered startup on the phone.

### F9 — P1: data setup ignores the caller's roaming prohibition

**Evidence:** `radio/hal/RadioNetworkData.cpp:357–386` computes whether the serving network is roaming, selects the roaming protocol, and logs `roamingAllowed`, but never checks that boolean before creating a WDS connection. The async request does not retain it. The local `IRadioData.aidl` defines it as the user's data-roaming permission.

**Fix:** retain the permission through asynchronous execution, recheck serving state before setup, and reject normal data setup when roaming is prohibited, using the appropriate AIDL result/failure cause. Reconcile existing calls and framework policy on roaming transitions. Handle any explicitly defined emergency exception separately.

**Validation:** mocked roaming/home/unknown transitions with permission true/false; prohibited roaming must issue no WDS start. This is a missing HAL safeguard; the review does not show that the framework has actually caused unwanted roaming traffic.

### F10 — P2: GNSS blocklisting changes the report, not the position solution

**Evidence:** `gnss/hal/Gnss.cpp:49–55,166,334–336` advertises satellite-blocklist support, stores a local list, and clears `USED_IN_FIX` in callbacks. It never tells the modem to exclude those satellites from calculating location. The local `IGnssConfiguration.aidl:103–123` requires exclusion from the solution, not just from reporting.

**Fix:** implement and acknowledge modem-side satellite exclusion if supported, or withdraw the capability and return the appropriate unsupported result. Report `USED_IN_FIX` faithfully. Also audit scheduling/recurrence, aiding-data deletion, and other advertised capabilities against actual QMI operations before marking Android GNSS complete.

**Validation:** mocked blocklist request must either reach an exclusion operation or be rejected without claiming support. Later verify known blocklisted satellites are excluded from actual fixes; do not validate solely by their callback flags disappearing.

### F11 — P2: the logged graphics fallback does not clear an earlier Mesa selection

**Evidence:** `rom/bin/a6l-modules.sh:28–35` sets `persist.graphics.egl=mesa` when the render node is present, but only logs “EGL angle” when absent. A pre-existing Mesa property can remain selected; the claimed fallback is not implemented in that branch. Persistent-property loading order is another reason not to use a previous boot's selector as the readiness decision.

**Fix:** choose the actual renderer explicitly before graphics startup, accounting for persistent-property loading and the available software EGL/Vulkan stack. Prefer a boot-scoped readiness signal plus a deterministic selection policy. Do not advertise hardware Vulkan for Adreno 512 merely because Mesa is present.

**Validation:** success boot followed by missing/late render node; verify the effective EGL implementation and visible output. Test the fallback with the real staged software libraries, not only its log line.

### F12 — P2: telephony settings can be acknowledged without being applied

**Evidence:** `radio/hal/RadioNetworkData.cpp:408–413` accepts initial-attach APN without programming it. `radio/hal/RadioMessagingVoice.cpp:131–148` acknowledges cell-broadcast activation even if the QMI operation fails, and accepts configuration ranges without programming them. These are retained integration gaps, not evidence that all supplementary services are broken.

**Fix:** implement and verify the relevant modem operations and return their real errors; otherwise explicitly report unsupported behavior. Treat cell broadcast as a required capability decision, including public-warning reception, rather than an automatically optional feature. Keep supplementary-service support accurately listed per operation.

**Validation:** mocked modem refusal and configuration round-trip; attach APN survives/reapplies across modem restart as designed. Use a lab/synthetic broadcast flow for reception tests, not live emergency services.

### F13 — P2: flash permission setup runs before the driver creates the node

**Evidence:** `rom/init/init.qcom.rc:48–54` chowns the flash brightness node in `on boot`, then starts the ADSP group. Flash modules are loaded later by the misc group after ADSP exits. `rom/vendor-etc/ueventd.rc` has backlight/frontlight rules but no flash equivalent; the misc loader only logs the discovered LED. The early chown can therefore fail on an ordinary cold boot. Current root-operated recovery flash tests do not cover non-root access.

**Fix:** add an event-driven ueventd rule for the actual flash device sysfs path, or apply ownership after successful probe. Include all controls required by the eventual torch/camera service, with its intended uid and SELinux domain; avoid globally writable sysfs. Confirm the actual node name rather than assuming `led:flash_0`.

**Validation:** cold boot with delayed flash probe, inspect ownership/context, then operate torch from the intended service uid. Complete CameraService torch support and the requested adjustable strength levels separately.

## Hardware checklist, one item at a time

“Reported” refers to the existing reports/ledger, not a test performed by this audit. “Pending” means no sufficient end-to-end evidence was located in the reviewed material. F-numbers refer to findings above. Where no specific code defect was established, the next step is a validation task, not a speculative patch.

| ID | Hardware / function | Review result and proposed next action |
|---|---|---|
| H1 | Front LCD / brightness | Prior visible Android output reported. No new panel defect established. Test cold boot, full brightness range and repeated blank/resume in the installed image; check fallback F11 and actual lights-HAL sysfs selection. |
| H2 | Adreno 512 GLES | Freedreno rendering previously reported from RAM. Fix F11. Validate GPU recovery, sustained load, devfreq and idle power in the installed ROM. |
| H3 | Vulkan | Old checklist's Turnip plan is inappropriate for this Adreno 5xx device; [Mesa documents no a5xx Turnip support](https://docs.mesa3d.org/drivers/freedreno.html#turnip). The current product selects software Vulkan. Record software capability honestly; do not make Turnip hardware support a release assumption. Test actual Vulkan enumeration and representative software workloads. |
| H4 | Rear e-ink | Panel/clear/mirroring work exists, with composer lease integration. No additional proven driver defect found. Validate installed-ROM switching, refresh after suspend, daemon restart and full clear/ghosting behavior. |
| H5 | E-ink frontlight | Current LPG → PWM LED overlay/module/control exists; old “does it exist?” row is obsolete. Installed-ROM brightness, off-at-idle/resume and visible output remain a separate acceptance test. |
| H6 | Front touch | Existing multitouch evidence. Validate edges, rotation, suspend/wake, and a finger held down while switching faces; ensure Android receives cancellation/release rather than retaining a gesture across grabs. This last item is a test risk, not a reproduced defect. |
| H7 | Rear touch | Events and uinput remapping implemented. Test all edges/orientations, multitouch release, SYN_DROPPED, face switching and mirror crash/restart; verify raw and virtual devices cannot both deliver active touches. |
| H8 | Physical keys | Evdev support and e-ink actions exist. Verify Android power/volume/long-press behavior, no duplicate delivery, and actual wake from suspend. |
| H9 | Vibrator | Correct current actuator is GPIO79, **not PM660 LRA**. Driver + VibratorOL are packaged. No new confirmed functional defect found. Test short/long pulses, cancel/off, process death and suspend; verify any advertised amplitude/effect support reflects this on/off actuator. |
| H10 | Loudspeaker | TFA output reportedly heard on 27 September. Keep media/ringtone success separate from speakerphone, which is absent (F7). Test routing and sustained playback with the selected profile. |
| H11 | Earpiece | Known physical fault on this unit. Keep “unvalidated on healthy hardware” status; do not close from successful headset calls. |
| H12 | Wired headset / jack | r5 reports MBHC v2 headset+mic success. Test 3-pole versus 4-pole distinction: the documented fallback can label an uncertain plug as a headset. Repeat hotplug in the ROM; fix route error recovery F7. |
| H13 | Main / secondary / headset mics | Audio6 routes and capture evidence exist; r5 reports headset capture. Retest main mic after MBHC merge, secondary mic selection and full duplex. Add real mute F6. EC/NS/calibration remains distinct from receiving samples. |
| H14 | Headset button | r5 reports 7 press/7 release IRQs without false removal. Verify Android keycodes/media and call controls; IRQ counts alone do not prove framework delivery. |
| H15 | Call audio | Recovery two-way headset audio reported. Fix F6/F7; validate volume, route switching, hangup cleanup, incoming call path, DSP recovery, and later IMS session selection. |
| H16 | Modem boot / stability | IPA-first recovery sequence works; ROM dependency handling is defective (F8). Validate actual installed startup, service loss/recovery and documented EFS persistence policy. |
| H17 | SIM / registration / dual SIM | PIN/provisioning improvements exist, including retry guard and dual-slot code. Do not repeat the old “single SIM only” assumption. Validate Android SIM UI, slot selection, hotplug/restart and data/voice subscription separation without consuming PIN attempts. |
| H18 | SMS | CLI send/receive and newer IMS SMS path exist. Test Android incoming/outgoing multipart, Unicode, status reports and modem restart; IMS SMS requires actual IMS registration, not merely an implemented method. |
| H19 | MMS | Now has a viable underlying mobile-data path, but MMS is not proven. Test APN selection, separate/secondary data connection, DNS/routes and messaging-app send/receive. |
| H20 | Outgoing calls | CLI signaling and audio evidence exist. Validate Dialer state transitions, cancellation, route/mute and ordinary-call failure cleanup. |
| H21 | Incoming calls | Keep installed-ROM acceptance open: ringing while idle/asleep, answer/reject, lockscreen, speaker ringtone and caller display. Modem IRQ wake and audio routing must both work. |
| H22 | DTMF | Code/tests exist. Verify remote recognition of short/long digits during an ordinary call; successful QMI response alone is insufficient. |
| H23 | Emergency calling | Not certified by this work. Review no-SIM/locked-SIM/no-service routing, emergency mode and callback handling with mocks or an authorized lab. Do not place live emergency test calls. |
| H24 | USSD / supplementary services / cell broadcast | Inventory each operation; avoid a blanket supported label. Initial-attach/broadcast false successes are F12. Implement missing operations and test response/error propagation; prioritize public-warning reception explicitly. |
| H25 | VoLTE / IMS / VoWiFi | Still blocked per newer r5 report. Use the companion review, but incorporate the new 770 session closure/no-PDN result. Successful binds are no longer a sufficient next milestone; capture the exact close reason/protocol sequence. VoWiFi remains separate and unproven. |
| H26 | Mobile data | IPA/WDS Internet traffic reportedly passed. Fix F8/F9; validate Android netd/DNS, reconnect, IPv4/v6, multi-APN and selected SIM. Never read IPA registers while runtime-suspended merely to diagnose it. |
| H27 | Wi-Fi STA | WPA2/DHCP/Internet reported in recovery; Android HAL blocked by F5. Fix HAL selection/implementation, then test Settings and suspend reconnect. |
| H28 | Wi-Fi identity / regulatory | Persist-derived MAC and regdb integration exist. Validate ordering before HAL interface creation and stable identity across reboot; distinguish permanent MAC from intentional per-network randomization. |
| H29 | Hotspot / tethering / P2P | Packaged hostapd and build flags are not a pass. After F5, test supported interface combinations, downstream DHCP/NAT and upstream changes; advertise only working combinations. |
| H30 | Bluetooth controller / BLE | Discovery reported; no-MSFT fix staged. Default Android HAL really has a Linux HCI path, so it is not merely a serial stub. Validate address initialization before HAL open, pairing, reconnect and controller restart. |
| H31 | A2DP / HFP-SCO | Still separate integration work. Test A2DP software transport and call SCO routing, both directions and transitions; F7 applies. Controller discovery is not audio support. |
| H32 | GNSS | QMI LOC/HAL/XTRA work exists; do not infer usable navigation from satellite visibility or injected assistance. Fix F10; validate outdoor first fix, timestamps, accuracy, repeat sessions and suspend behavior. |
| H33 | Main IMX576 + GT9769 autofocus | Newer camfix6/WM6 tests still fail sustained frames. Continue companion review's isolated CAMSS tests. Then prove focus motion, exposure/gain and full-height RAW; sensor ID is insufficient. |
| H34 | Front S5K3T1 | Enumerated/probed is not a capture pass. Validate sustained RAW independently after transport repair, correct orientation/Bayer order and stream restart. |
| H35 | Auxiliary HI846 | Keep explicit auxiliary-camera row. Prove sensor role, RAW capture and simultaneous/use-case requirements; do not omit it once the main camera works. |
| H36 | Android camera HAL / ISP | Still a distinct missing layer after RAW capture. Build/integrate libcamera/SoftISP/provider and metadata; test preview/still/video, autofocus, orientation, permissions, reopen and Camera2/CameraX. No existing driver probe establishes this. |
| H37 | Flash / torch | r5 reports visible torch at 23/49/100 mA and a short strobe. Fix permission ordering F13. Add CameraService torch, adjustable strength requested by Pierre, flash synchronization and shutdown-on-error. Higher-current limits are not validated by these lower-current observations. |
| H38 | Accel / gyro / compass | IIO measurements reported; Android HAL host tests exist. Fix F4, then test physical axes/units, timestamps, rotation/fusion and calibration persistence through the framework. |
| H39 | Rear TMD3702 | Working rear sensor must not substitute for front call proximity. No new defect established. Verify its intended e-ink use, power lifecycle and whether/where readings are exposed. |
| H40 | Front STK3338 light / proximity | Awake sensor evidence exists. Fix F3/F4; calibrate lux/near-far thresholds, verify auto-brightness and call suspend/wake end-to-end. |
| H41 | Hall / cover | GPIO75/L13 vote and wake-capable gpio-keys overlay exist; polarity was an assumption. Verify magnet/cover open and close, Android lid policy and wake behavior; do not interpret a constant high GPIO as a pass. |
| H42 | Steps / significant motion / virtual sensors | Deliberately not declared by current sensor package. Keep optional missing features explicit. Validate framework rotation-vector/fusion support separately; do not advertise unsupported low-power step detectors. |
| H43 | Fingerprint | Intentionally dropped by Pierre. Record as excluded, not a forgotten driver or a mandatory fix. |
| H44 | NFC | Reported not fitted. Keep excluded; do not expose a feature/HAL for absent hardware. |
| H45 | USB ADB / speed | Recovery ADB works with a warm-reboot reliability history. r5 identifies USB2/high-speed stock configuration. Test repeated cold/warm reconnect and installed adbd; do not pursue SuperSpeed as missing support for this configuration. |
| H46 | USB MTP / function switching | Dedicated `hals/usb-gadget` code is unfinished and explicitly excluded by the pipeline. Its success-returning stubs must not be re-enabled as a fix. Test the actual init/configfs path for adb↔mtp, disconnect/reset and host transfers; implement a real gadget HAL if needed. |
| H47 | USB OTG / role switching | No complete validated host/role implementation found. Identify controller/Type-C/CC wiring, role switch and VBUS supply; then test host enumeration/power and return to device mode. Do not equate QC charging with USB-PD/OTG support. |
| H48 | Charger | Binding/current telemetry and lower-current attended progress exist. F1/F2 block relying on the software policy. Full ROM FCC, warm/cool behavior and fast charging remain separate milestones; HVDCP is not yet working per r5. |
| H49 | Off-mode charging | **Missing integration gate:** driver/misc bring-up is tied to normal `on boot`, while charger mode needs its own event/service path. A Health `--charger` executable alone does not load these drivers/policy. Add a minimal `on charger` dependency path, then test power-off cable insertion, display, unplug and boot-to-system. |
| H50 | Fuel gauge / Health | FG telemetry and a real sysfs-backed AOSP Health implementation exist; “example” does not automatically mean fake here. Validate actual capacity/current sign/temp/status/full/low-battery callbacks and unplug transitions. F2 means the guard's own property must not be mistaken for hardware truth. |
| H51 | Thermal | **Still a gap:** main rom.mk does not package a real device thermal HAL; optional full variant's example is not a tuned implementation. Map actual zones/trips/cooling devices and provide real Android severity/headroom callbacks. Kernel protection and CPU cooling must be checked independently of HAL presence. |
| H52 | CPU DVFS / CPR / OSM | Prepared CPR work is not the active stock-clock kernel path. Retain staged fuse/voltage validation plan and module compatibility checks. Do not claim working scaling or thermal CPU mitigation from offline tables alone. |
| H53 | Suspend / wake sources | No complete phone acceptance matrix found. Test power key, RTC, incoming call/SMS, charger, cover and proximity; fix F3. QEMU suspend is insufficient. |
| H54 | Deep idle / battery life | IPA is deliberately runtime-active; diagnostic clock/domain holds remain relevant. Measure residency and screen-off drain with each subsystem enabled. Remove holds only as dependencies gain proper PM votes, checking resume after each change. |
| H55 | Venus / hardware video codecs | No complete Codec2 hardware path established. Track decode and encode separately, firmware/driver/HAL integration, formats, resolution, and thermal behavior. Explicitly retain software fallback limitations. |
| H56 | DRM / Widevine | No verified Widevine integration/security level. Report actual DRM capabilities; ClearKey or successful playback of unprotected video does not establish Widevine. |
| H57 | KeyMint / Gatekeeper | Current product uses nonsecure KeyMint; release Gatekeeper is optional preparation. Validate keystore/lockscreen operations and accurately report software security. Hardware-backed key storage/attestation is not established. |
| H58 | eMMC / filesystems / RAM pressure | Existing boot/storage evidence; no new block-driver defect found. Validate installed mounts, repeated I/O, zram/memory pressure, restart and error recovery. Firmware/persist read-only mounts and 6 GiB memory profile are now present. |
| H59 | Data encryption | Current default fstab is plain ext4; FBE is opt-in and requires the prepared kernel changes. Keep “not encrypted” explicit. Validate install/unlock/reboot/recovery/update with the actual FBE build before changing status. |
| H60 | microSD | r5 reports the L5 voltage-grid correction and successful read at SDR104. ROM overlay/fstab are present. Still test vold discovery, supported filesystem mount, eject/reinsert and write/readback on disposable media; a recovery read does not prove Android storage UI or adoptable storage. |
| H61 | Notification LED | Hardware presence/wiring and mapping remain unconfirmed in reviewed evidence. Inventory physical LEDs first, then implement only real channels; flash and e-ink frontlight are not evidence of a notification LED. |
| H62 | RTC / alarms / network time | Verify RTC advances across reboot, wakealarm from real suspend, and Android wall-clock/NITZ handling independently. GNSS time injection and working system time in a VM do not prove RTC wake. |
| H63 | Power-off / reboot / recovery / rollback | Tools and rollback preparation have advanced beyond old checklist. Keep installed-ROM reboot/charger/EDL/recovery transitions and an independently proven restore route as acceptance items. Do not derive EDL support from the existence of a proposed PON value. |
| H64 | Hardware watchdog | No complete APSS watchdog acceptance found. `ro.hw_timeout_multiplier` concerns Android timeouts, not proof of an enabled hardware watchdog. Integrate the actual driver/watchdogd policy and test expiry/recovery in a controlled development setup. |

## Checklist changes and suggested order

1. Replace one-dimensional “works” labels with **driver/recovery**, **Android integration**, **installed-ROM**, and **suspend/recovery** evidence, each tied to a build/module hash. This prevents Wi-Fi, sensors, flash and data CLI passes from concealing missing Android behavior.
2. Add explicit subitems for call mute, speakerphone, HFP, public warnings, roaming policy, charger write failures, sensor/ADSP restart, torch strength, and per-sensor camera streaming. These are currently easy to lose inside broad hardware rows.
3. Fix F1–F6 and F8 first. F7 speakerphone is also a daily-use blocker for this unit because its earpiece is broken; headset-only operation should be an explicit interim limitation.
4. Keep VoLTE and camera experiments isolated. The newer failures mean IMS binding/enable and CAMSS bandwidth/burst knobs have not solved their respective problems. Use the companion report's instrumentation and pass criteria, amended by the r5 results above.
5. Before closing any hardware row, run the relevant Android test under the intended SELinux mode. Offline policy compilation, module staging and QEMU boot are useful checks, but do not exercise the phone's real device paths or all service permissions.

## Reproductions and review limits

Files: [reproduce.py](../research/hardware-review-20260928/reproduce.py), [sensor_review.c](../research/hardware-review-20260928/sensor_review.c), [results.txt](../research/hardware-review-20260928/results.txt), [source-hashes.json](../research/hardware-review-20260928/source-hashes.json).

Run from WSL: `python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-20260928/reproduce.py`.

The guard is executed unchanged except for line-ending normalization into a temporary copy, using fake power-supply files. A shell sleep boundary injects recovery after a failed write. The sensor harness compiles the actual HAL/motion code and intercepts only its final `poll()` to inspect the deadline after a real read error. It uses the existing host-test path abstraction. **All expected defect reproductions completed**, including a normal-temperature control; sensor execution completed under ASan/UBSan without sanitizer errors. Existing `snprintf` path-length compiler warnings were emitted and are not claimed fixed. Tests assert the current broken behavior deliberately; they must be converted to corrected expectations when fixes land.

Other findings are source/control-flow reviews, not fault-injected hardware tests. Local platform inspection used `/home/a6l/android/a6l-lineage24`, including Wi-Fi HAL selection/factory/fallback, Bluetooth Linux HCI transport, Health sysfs integration, audio mute, and the GNSS/radio AIDL contracts. Source hashes identify the tested sensor/charger versions; recheck affected lines and final build contents before applying fixes to the evolving checkout.
