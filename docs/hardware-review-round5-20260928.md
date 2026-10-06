# A6L hardware integration review — fifth pass and final extensions

28 September 2026. Continues F1–F41 in the [initial review](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-20260928.md), [second pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-pass2-20260928.md), [deep review](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-deep-20260928.md), and [fourth pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round4-20260928.md). Includes the two additional review requests received while this pass was running.

**Nine new finding groups, F42–F50.** The radio pass examined inherited HAL behavior, SIM identity and credentials, and the actual bytes sent to QMI. The extensions traced vibrator integration, display recovery, and other hardware startup paths. Every numbered group has an offline reproduction, with the limitations described below. These are defects in the reviewed source under specified conditions, not nine new failures observed on the phone.

Two deserve first attention: **F47 can abort the shared radio service**, and **F49 prevents the packaged Android vibrator HAL from selecting the new GPIO vibrator**. F50 can leave the LCD dark after a failed switch back from e-ink. F43 can associate the wrong SIM identity with the primary subscription.

Only this report and isolated research artifacts were added. Production source and the phone were not modified. No RF operation, real PIN attempt, build installation or partition write occurred. Claude's existing changes remain separate. Source archives and SHA-256 inventories capture what was tested; both harness runs verified that their captured source files stayed unchanged during execution. This is not acceptance of an installed ROM or closure of the earlier findings.

**Final revalidation limitation:** concurrent radio integration changed seven files before the rerun and thirteen by the final comparison. The rerun reproduced F42/F44/F45/F46/F48 again, but its full-ModemCore build encountered an in-progress API mismatch: `DataCallManager::LostFn` took `(int, uint64_t)`, while that snapshot's `ModemCore::start()` still supplied a one-argument callback. A subsequent read showed Claude had already updated that callback. The successful complete run below nevertheless refers to the preserved earlier snapshot; the continuing integration has not had a complete rerun. The transient mismatch is not counted as a tenth defect. Revalidate the completed integration; see [revalidation details](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round5-20260928/concurrent-revalidation/STATUS.md).

## Findings

| ID | Priority | Behavior | Reproduced result |
|---|---|---|---|
| F42 | P2 | Disable SIM / reduce active modem count | Success without a modem operation; live-count readback contradicts the accepted request |
| F43 | P2 | Dual-SIM ICCID | Primary subscription mapped to card 2 reads card 1's identity |
| F44 | P2 | Universal PIN handling | UPIN-aware validation followed by PIN1-targeted QMI commands |
| F45 | P2 | Allowed radio technologies | Empty/unsupported mask enables GSM, UMTS and LTE |
| F46 | P2 | Manual operator selection | Two- and three-digit MNCs collapse to identical requests |
| F47 | P1 | SIM APDU / logical channels | Inherited SIM emulator; nonzero READ BINARY offset reaches a fatal assertion |
| F48 | P2 | Fixed dialling number status | Always reports disabled successfully, without querying the SIM |
| F49 | P1 | Android vibration | Driver name rejected; `on()` succeeds with no effect uploaded |
| F50 | P2 | Return from e-ink to LCD | Failed unblank clears retry state; 100 healthy iterations do not recover |

P1 means fix before relying on that function; P2 means a correctness defect that needs its stated conditions covered. Related symptoms are grouped rather than counted separately.

### F42 — SIM-disable controls acknowledge changes they do not apply

**Sources:** [live-modem configuration](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioSimModemConfig.cpp:154), [inherited UICC enablement](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/minradio/sim/RadioSim.cpp:62), and [SIM class inheritance](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioImpl.h:75).

`setNumOfLiveModems()` validates the requested count but performs no transition. `getNumOfLiveModems()` returns the configured slot count. Separately, `enableUiccApplications(false)` merely changes an inherited Boolean; it sends no provisioning/power operation and does not govern the real card status returned by `A6lRadioSim`.

**Reproduction:** with two configured slots, setting the live count to one succeeds, but the next getter returns two. Disabling UICC applications succeeds and its getter returns false, with zero additional QMI requests. The test executes the actual method bodies with response-recording Binder stubs. It does not claim that every framework SIM-disable flow leaves RF active: the framework may perform additional operations. These particular HAL acknowledgements are false.

**Proposed fix:** implement the modem/UIM transition, reconcile data and registration on the affected subscription, and publish success only after the new state is established. Until implemented, return the appropriate unsupported error. Keep maximum supported slots separate from currently active modems. UICC enablement must reflect the card application's applied state, including restart recovery.

**Acceptance:** two → one → two live modems; disable/re-enable each SIM independently; failed transition; modem restart; getters and unsolicited status agree with applied state. Disabled applications must not silently remain usable through this interface.

### F43 — the primary subscription's ICCID ignores its physical-card mapping

**Sources:** [ModemCore::iccid](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:744) and [default ICCID reader](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/services.cc:361).

For `mSub == 0`, ICCID retrieval uses the legacy helper hardwired to physical card session 1. The secondary subscription correctly derives its session from `slotSim()`. Thus the primary application's provisioned physical slot and its reported ICCID can disagree.

**Reproduction:** the complete production `ModemCore` and QMI client run against a fake two-card modem. Primary provisioning points to physical card 2, secondary to card 1. Card 1 contains dummy ICCID `1111`; card 2 contains `2222`. Both subscriptions return `1111`, even though the primary's own slot view correctly identifies card 2. No real subscriber identifiers were used.

**Proposed fix:** select the physical card session from the current provisioning view for every subscription, including subscription zero. Permit a legacy DMS fallback only when its identity is unambiguously tied to that same card. Invalidate identity caches when card/provisioning mappings change.

**Acceptance:** normal and reversed provisioning; one missing card; physical-card replacement; mapping change with cached ICCID; failed reads. Each Android subscription must retain the correct card identity and associated settings.

### F44 — UPIN-aware validation sends operations to PIN1

**Sources:** [UPIN preflight selection](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:351), [PIN operations](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioSimModemConfig.cpp:411), and [PIN identifiers](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/include/a6lqmi/services.h:95).

Preflight and the SIM-lock getter honor `upinReplacesPin1`, using the card's universal PIN state and retry count. Verify, unblock, change-PIN and `SC` lock-setting operations nevertheless pass `kPin1` unconditionally. PIN1 and UPIN are distinct protocol identifiers, also documented in [libqmi's UIM enum definitions](https://chromium.googlesource.com/chromiumos/third_party/libqmi/+/master/src/libqmi-glib/qmi-enums-uim.h).

**Reproduction:** a synthetic card says UPIN replaces PIN1, UPIN needs verification with three attempts, and application PIN1 is disabled. The real preflight returns `Send` using UPIN's count. All four real QMI builders receive/send PIN identifier **1**, whereas UPIN is **3**. Fake responses accept the operations; this establishes the wrong selected credential, not the physical modem's eventual error or retry-counter behavior. No PIN attempts were spent.

**Proposed fix:** resolve the requested application and effective PIN identifier once from fresh card state; use that same selection for validation, command construction and returned counters. Handle UPIN unblock/change/protection consistently. Do not fix only the verify method.

**Acceptance:** PIN1 and UPIN replacement fixtures for all four operations, wrong/unknown AID, blocked state and card replacement. Validate command identifiers with mocks before considering any attended credential test.

### F45 — an empty supported-network intersection enables all available families

**Source:** [allowed-network setter and getter](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioNetworkData.cpp:255).

When no requested bits map to GSM/UMTS/LTE, the setter replaces the zero result with all three families. On success it caches the original requested bitmap, not what it applied. This contradicts the local AIDL contract's restriction to the requested technology set (`/home/a6l/android/a6l-lineage24/hardware/interfaces/radio/aidl/android/hardware/radio/network/IRadioNetwork.aidl:231`).

**Reproduction:** both a zero bitmap and NR-only bitmap `0x100000` generate QMI mode preference `0x1c` = GSM + UMTS + LTE, with a successful response. The following getter reports NR-only from the cache. This matters when the framework or another legitimate caller supplies an empty/unsupported intersection; it does not establish that ordinary LTE-only settings fail or that A6L supports NR.

**Proposed fix:** reject an unrepresentable request without changing modem state, or implement the contract's defined handling of an empty set. Never broaden it to all technologies. Report confirmed applied capability/state and invalidate the cache across modem generations. Preserve the separate emergency exception defined by AIDL.

**Acceptance:** zero, unsupported-only, LTE-only, UMTS-only and mixed masks; rejected requests leave the previous applied mask unchanged; getter matches the actual state after both success and restart.

### F46 — manual network selection loses MNC width

**Sources:** [HAL operator parsing](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioNetworkData.cpp:238) and [NAS request construction](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/services.cc:650).

The HAL converts the MNC substring with `atoi()` and discards its length. The QMI helper sends MCC/MNC integers but omits the separate MNC-width indicator. NAS Initiate Network Register supports this as TLV `0x12`; see the [libqmi NAS schema](https://chromium.googlesource.com/chromiumos/third_party/libqmi/+/bdc8c50a91c9c323eccb9132b5c51b72ca93f370/data/qmi-service-nas.json).

**Reproduction:** test PLMNs `00101` and `001001` produce identical QMI TLVs, both without `0x12`. The requested networks are distinct; the request cannot distinguish them. No real registration was attempted.

**Proposed fix:** validate exactly five or six decimal digits, carry two-/three-digit MNC width through the helper API, and encode the protocol indicator. Preserve width through scan results and selection as well.

**Acceptance:** round-trip two-/three-digit MNCs with leading zeroes, malformed identifiers, each supported RAT, and modem rejection. Different valid PLMNs must remain distinguishable on the wire.

### F47 — APDU calls still enter the inherited SIM emulator, with fatal assertions

**Sources:** [base SIM construction and APDU methods](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/minradio/sim/RadioSim.cpp:35), [application/channel dispatch](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/minradio/sim/AppManager.cpp:61), and [fatal READ BINARY check](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/minradio/sim/apps/FilesystemApp.cpp:131). `A6lRadioSim` overrides file I/O but not the logical-channel/basic/logical APDU methods.

Those inherited methods use an in-memory filesystem initialized with dummy language/forbidden-PLMN data. Only its empty-AID filesystem application is registered; normal real-card application AIDs are not forwarded to UIM. Opening those logical channels therefore fails locally. Basic-channel READ BINARY reaches `CHECK(offsetHi == 0 && offsetLo == 0)`. A nonzero byte offset is normal APDU input, yet causes a fatal assertion. UPDATE BINARY and several record arguments contain similar assertions.

**Reproduction:** the exact production READ BINARY method returns the dummy file for offset zero. Offset one terminates the isolated child with **SIGABRT**. The harness substitutes Android's fatal `CHECK` with an aborting equivalent and stubs the filesystem; the Binder-to-method route was traced in source. It is not a crash test of an installed Android radio process. Because these SIM objects run in the shared radio service, reaching the fatal branch there would interrupt the other radio interfaces too.

**Proposed fix:** implement UIM-backed channel open/close and APDU transport with correct physical-card/session ownership. Until then return an honest unsupported result, rather than emulator data. Replace request-dependent fatal assertions with validated errors/status words. Remove the production dependency on the simulated SIM application layer where real card behavior is required.

**Acceptance:** unknown/real AID, basic and logical channels, nonzero offsets, partial reads, invalid lengths, channel close/reuse, card removal and modem restart. None may abort the service. Later verify real card applications and carrier-rule access separately from ordinary ICC file reads. The unused `addCtsCertificate()` helper has no discovered caller; this review does **not** claim a CTS certificate was installed or carrier privileges granted.

### F48 — fixed dialling number status is always reported as disabled

**Source:** [getFacilityLockForApp](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioSimModemConfig.cpp:462).

`SC` queries real PIN state, other unknown facilities return unsupported, but `FD` falls through with `locked = 0` and success. It never checks FDN status or even card availability. The corresponding setter correctly returns unsupported for `FD`; the getter should not invent a disabled state.

**Reproduction:** the actual getter returns success/disabled with zero QMI requests. The result is independent of the card's real FDN setting, which the method never reads. This is false reporting, not evidence that modem/SIM FDN enforcement can be bypassed.

**Proposed fix:** query the effective application's FDN service status, or return unsupported until that query exists. Distinguish absent card, unavailable state and disabled FDN.

**Acceptance:** enabled/disabled FDN, absent card, query failure and each physical slot. The Android status must match the card or explicitly fail.

### F49 — the selected Android vibrator HAL rejects the new driver's name

**Sources:** [packaged QTI vibrator selection](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/rom/rom.mk:144), [driver input name](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/kernel/a6l-gpio-vibrator/a6l_gpio_vib.c:176), and local platform `/home/a6l/android/a6l-lineage24/vendor/qcom/opensource/vibrator/aidl/VibratorOL/Vibrator.cpp:138,237`.

`InputFFDevice` accepts a fixed list of device names before checking FF capabilities. `a6l_gpio_vibrator` is absent. After rejecting it, `play()` deliberately returns success when no device was selected. Thus the GPIO driver/recovery FF test can work while Android vibration silently does nothing. No A6L patch updating this filter was found in the reviewed integration tools or platform source.

**Reproduction:** the unchanged constructor, `isPresent()` and `play()` run against fake evdev. With `a6l_gpio_vibrator`, presence is false, `on(400 ms)` succeeds, and there are **zero capability queries, effect uploads or event writes**. A positive-control device named `qti-haptics` is selected and receives one upload and one write.

**Proposed fix:** explicitly support the A6L driver in the packaged HAL, or provide an A6L implementation for its binary GPIO actuator. Also resolve startup ordering: the HAL enumerates once in its constructor, while the vibrator module is loaded later through the ADSP → misc chain. Gate startup on real device readiness or implement reopen/retry, not merely a name change. Propagate missing-device errors.

When completing that integration, do not advertise real amplitude control merely because ff-memless exposes `FF_GAIN`: the driver turns every nonzero combined effect into the same GPIO-on state. Audit effect capabilities and duration conversion as well; the cited 15-second clamp in the QTI source is conditional on its LED/LDO backend, not this FF backend. These are required follow-up checks within the vibrator integration finding, not additional counted findings.

**Acceptance:** actual Android vibration after cold boot and late module load; short/long pulse, cancellation, service restart and suspend; confirmed GPIO-off after completion; truthful amplitude/effect capabilities. Direct recovery `a6l_vib` success alone cannot close H9.

### F50 — a failed LCD unblank suppresses all further recovery attempts

**Source:** [enforce_backlight](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/eink/switcher/native/a6l_dualux.c:216).

When switching back to LCD, `blanked_by_us` is cleared whether writing `bl_power=0` succeeds or fails. Subsequent iterations skip unblanking because that flag is now zero. A transient sysfs failure can therefore leave the primary screen dark even after writes become possible again, unless some other component or another state transition repairs it.

**Reproduction:** the unchanged function runs with Android awake, LCD selected and `bl_power=4`. Its first unblank write fails. After restoring successful writes, **100 iterations** still leave `bl_power=4`, with only one total write attempt. Retaining the retry flag in a positive control allows the next operation to restore `bl_power=0`.

**Proposed fix:** clear ownership/retry state only after a successful unblank or reliable readback proving it already happened. Retain pending restoration while asleep and retry when awake, without overriding a deliberate Android sleep state. Use bounded error logging and re-discover a lost backlight device when appropriate.

**Acceptance:** failed write followed by recovery without another face switch; unreadable/missing node; switch while asleep then wake; daemon restart after e-ink; normal Android blanking. Verify visible LCD recovery on the phone after the host cases pass.

## Coverage and candidates not promoted

The added findings affect inventory rows H17/H24/H26 (SIM/radio), H9 (vibrator), and H1/H4/H5 (display switching). SIM toolkit, carrier applications, FDN and logical channels should be explicit checklist subitems; “SIM ready” is not evidence for all of them. Also split “vibrator driver works” from “Android HAL selects and controls it.”

The extensions inspected current Wi-Fi lifecycle/capability code, USB init/configfs integration, charger-guard fixes, frontlight control, vibrator force-feedback behavior, storage configuration, battery discovery, and release recovery assembly. No additional independent finding is claimed for those other paths. Earlier F35–F41 and the remaining hardware acceptance gaps still apply; this pass does not certify thermal behavior, suspend, OTG, cameras or IMS.

Three tempting conclusions were rejected: (1) replacing the boot image does not invalidate a recovery delta in the reviewed release configuration, because it explicitly selects a full recovery image; (2) Health's current `BatteryMonitor::updateValues()` rescans and initializes late battery devices, so startup-before-fuel-gauge alone is not proof of permanent battery loss; (3) the SIM test-certificate helper exists but has no discovered production caller. Touch cancellation across face changes and physical suspend/thermal behavior remain acceptance work, not extra numbered defects without stronger evidence.

The common unresolved pattern is a mismatch between a successful API response and applied hardware state. Prioritize cross-layer acceptance tests: framework request → selected HAL → driver/modem operation → observable result, followed by a failure/recovery case with the requested state unchanged. Repeating only happy-path CLI tests will miss these defects.

## Reproduction artifacts

[Evidence directory](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round5-20260928/README.md) contains the source snapshots, hashes, harnesses, build logs and results. Both runners use ASan/UBSan; all assertions passed, including the intentionally aborting APDU child. “PASS” here means the defect was reproduced, not that the affected feature works.

```text
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round5-20260928/reproduce.py
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round5-20260928/extended.py
```

The first runner uses exact extracted HAL methods with Binder stubs and real QMI builders/client, plus the full `ModemCore` for F43. F47 isolates the actual APDU method with a faithful fatal-assertion substitute. The second runner uses the full relevant vibrator methods with fake evdev and the exact LCD enforcement method with fake reads/writes. Neither invokes adb or accesses a real modem, SIM, display or vibrator.
