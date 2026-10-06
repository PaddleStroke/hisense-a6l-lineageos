# A6L hardware integration review — eleventh pass

28 September 2026. **Complete: one new finding, F65, and an incomplete recovery case in F17.** Both reproduced against the current integration source. The network-time reproduction also runs the actual local Android `NitzData` parser. Production source and the phone were not modified.

| Finding | Priority | Confirmed behavior |
|---|---|---|
| F17 follow-up | P1 | Failed VOICE/WMS registration is not retried at startup or after isolated service restoration, while the core reports ready |
| F65 | P2 | Missing network timezone/DST metadata becomes an explicit UTC offset of zero / no DST in Android |

Previous findings: [tenth pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round10-20260928.md) and [original F17](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-pass2-20260928.md). The preserved snapshot identifies the reviewed revision; Claude's later changes and previously built images may differ. No approval or security check interrupted this review.

## F17 follow-up — Registration recovery is abandoned after a transient rejection

**Sources:** [startup registrations](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:330), [ready state](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:310), [connect-loop ready branch](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:284), and [service restoration](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:685).

The F17 changes correctly notice isolated VOICE/WMS withdrawal and attempt initialization on return. However, `restoreService()` removes the service from `mSvcLost` before checking availability, binding, or the initialization results. A failed registration is only logged. There is no retry deadline or pending registration state. While `mNeedsInit` remains false, the connection supervisor simply sleeps and repeats.

Startup has the same underlying gap: `initModem()` logs and ignores VOICE indication-registration and WMS event-report failures. If the remaining initialization succeeds, the core enters ready state. WMS route configuration has a store-mode fallback, but that does not repair a failed event-report registration. The secondary-subscription rebind path also returns after failure without retaining the recovery marker; that variant was inspected, not exercised by this single-slot test.

**Reproduction:** compile production `ModemCore.cpp` and the QMI implementation against the repository's fake transport and Android property/logging fixtures. Reject VOICE registration and WMS event reporting with a QMI error, while other initialization succeeds. The core becomes ready after exactly one attempt at each registration. Clear the fault and wait 2.5 seconds, spanning multiple supervisor iterations: neither request is retried, and accepted-registration counts remain zero.

Then withdraw and republish only VOICE/WMS. With a healthy fake modem, both registrations succeed. Repeat that service cycle with registration failures, clear the fault again, and wait another 2.5 seconds: there are again zero retries while the core reports ready. A further healthy service cycle restores both registrations, demonstrating that the transport and restoration path work when explicitly triggered.

**Impact:** startup or an isolated service restart can leave notification setup incomplete until another service cycle or full reinitialization. Where firmware requires the rejected registration, Android can miss incoming-message or call-related notifications despite an otherwise usable modem. The harness establishes the failed setup and missing retry; it does not emulate firmware's notification filtering or demonstrate a missed real call/SMS. The absence of a longer-term retry follows from the source, not just the finite observation window.

**Proposed fix:** track service presence, subscription binding, and required registrations separately. Keep failed work pending until acknowledged, and reconcile it on the worker using bounded exponential backoff. Clear recovery state only after the required steps succeed. Invalidate pending work on service-generation changes, and avoid re-registering healthy services unnecessarily. Share this mechanism between startup and isolated restoration. Classify unsupported optional registrations separately from transient failures; keep affected functionality explicitly unavailable when essential setup cannot succeed.

**Acceptance:** inject individual failures in VOICE registration, WMS routes, WMS event reporting and secondary-subscription binding, both at startup and after isolated return. Recover without another service withdrawal. Include repeated failure, withdrawal during backoff, modem restart during a request, and healthy-service controls. Assert truthful readiness, bounded retries and restored notifications with a fixture that respects registrations, then perform attended call/SMS tests. This extends F17 rather than creating a second identifier for the same recovery requirement.

## F65 — Unknown timezone information is converted into affirmative zero values

**Sources:** [NAS network-time handler](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:815) and [NITZ forwarding](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioNetworkData.cpp:342).

The handler initializes `tz` and `dst` to zero, updates them only when their TLVs exist, and always formats both into the outgoing NITZ string. A missing timezone therefore becomes `+0`; missing daylight-saving information becomes `,0`. These are factual assertions, not representations of missing information.

The [libqmi NAS schema](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-nas.json) defines universal time, timezone offset and daylight-saving adjustment in separate fields. Android's [NitzData parser](https://android.googlesource.com/platform/frameworks/opt/telephony/+/refs/heads/main/src/java/com/android/internal/telephony/NitzData.java) preserves an omitted DST field as unknown, but interprets a supplied zero as a known zero adjustment. The current bridge loses that distinction.

**Reproduction:** inject three network-time indications through the production QMI dispatcher and core, capture the actual listener strings, and feed those strings to the unchanged local Android parser:

| Modem metadata accompanying UTC 12:00 | Emitted suffix | Android interpretation |
|---|---|---|
| Neither timezone nor DST supplied | `+0,0` | UTC offset 0; DST adjustment 0 |
| Offset +8 quarter-hours; DST absent | `+8,0` | UTC offset +2 hours; DST adjustment 0 |
| Offset +8 quarter-hours; DST 1 hour | `+8,1` | UTC offset +2 hours; DST adjustment 1 hour |

The fully specified case is a positive control. Feeding `26/09/28,12:00:00+8` directly to the same Android parser yields an unknown (`null`) DST adjustment, as required when it was not supplied. UTC epoch time remains identical across the cases: **there is no demonstrated UTC-versus-local-time conversion error here.**

**Impact:** Android receives misleading inputs for automatic timezone selection when network metadata is incomplete. The chosen timezone also depends on country, other signals and framework policy; this test does not claim a particular timezone change on the phone. F61 concerned elapsed age while forwarding network time. Its fix does not address this separate loss of optional metadata.

**Proposed fix:** retain presence flags and validate values. When the timezone is known but DST is missing, omit the DST component rather than inserting zero. When timezone itself is missing, do not manufacture a NITZ offset: obtain current valid metadata through a supported query or suppress the unsupported combined indication with a diagnostic. If preserving time-only assistance is important, use an explicitly supported time-only path. Do not borrow an unvalidated offset from an earlier network/subscription. Keep the F61 boot-clock receipt/age handling intact.

**Acceptance:** missing timezone, missing DST, both missing, explicitly supplied zero, positive/negative and fractional-hour offsets, valid DST values, malformed fields, and network/subscription transitions. Verify the parsed Android values and timezone-suggestion behavior. No test should change the host or phone clock merely to exercise parsing.

## Evidence and limits

[Reproduction instructions](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/README.md), [results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/results.json), [radio log](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/radio.log), [Android parser log](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/java.log), [source hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/source-hashes.json), and [snapshot](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round11-20260928/reviewed-source.zip) are preserved.

The radio harness passed 31 defect/control assertions under ASan/UBSan with no reported sanitizer diagnostics. The Java parser checks passed. No snapshotted source changed during the run. As in the existing ModemCore tests, the radio harness exits with `_Exit` because the production singleton has process-lifetime threads and no shutdown API; teardown and leak checking are not validated. Passing means these defects reproduced, not that fixes passed acceptance.

The pass also inspected data teardown, SIM I/O/PIN dispatch, network registration mapping and vibrator lifecycle code. No additional confirmed finding is claimed for those paths. In particular, a rejected WDS STOP alone does not prove a leaked data session because closing the QMI client may release it. Hardware acceptance remains necessary after these source issues are fixed.
