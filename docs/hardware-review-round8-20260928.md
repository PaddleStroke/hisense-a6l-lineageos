# A6L hardware integration review — eighth pass

28 September 2026. **Complete: three new findings, F59–F61.** Two concern an established or establishing mobile-data connection; the third concerns network-time accuracy when delivery is delayed. Production and the phone were not modified.

| Finding | Priority | Reproduced behavior |
|---|---|---|
| F59 | P2 | A connected/reconfiguration-required indication does not refresh IP, DNS or MTU |
| F60 | P2 | A disconnect during settings retrieval is consumed before the listener is installed; setup then reports success |
| F61 | P2 | A delayed NITZ sample is forwarded with its delivery timestamp and age zero |

These are source defects under specified conditions, not three newly observed phone failures. Tests used the preserved source snapshot; Claude's subsequent integrations require comparison against that snapshot. The [seventh-pass report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round7-20260928.md) links the previous rounds. No security or approval check stopped this work.

## F59 — Active data reconfiguration requests are ignored

**Sources:** [packet-status parsing](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/services.cc:1287), [data indication handler](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/datacall.cc:302), [settings retrieval](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/datacall.cc:280).

The parser preserves the QMI `Reconfiguration Required` bit, but the installed callback only acts on DISCONNECTED. A CONNECTED indication carrying that bit is ignored. Current settings are read during setup only. Neither the cached data call nor the installed interface addresses are refreshed when the modem requests reconfiguration. The bit and connection status are separate fields in the [libqmi WDS definition](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-wds.json).

**Reproduction:** establish IPv4 with `10.0.0.5/30`, DNS `8.8.8.8`, and MTU 1430 using the real data manager, QMI client and encoders with fake modem/link operations. Change the fake modem's settings to `10.0.0.9/30`, DNS `9.9.9.9`, MTU 1280, then deliver CONNECTED with reconfiguration required. After dispatch and 300 ms, no additional settings query or address operation occurred; the cached call still contained all three old values. An independent fresh query retrieved the new values, confirming that the fixture made them available. Source inspection finds no later refresh mechanism in this manager.

**Impact:** Android and the interface can retain obsolete connection parameters after a modem-requested change, causing traffic or DNS failures until teardown/reconnection. This is distinct from F14's initial address installation and F15's setup prerequisite checks. Frequency and actual network-triggered reconfiguration on this firmware remain unmeasured.

**Proposed fix:** handle the reconfiguration bit on the data executor, scoped to call generation and affected IP family. Re-query settings, validate them, replace owned addresses and update DNS/gateway/MTU information before publishing a changed data-call list to Android. If safe in-place repair is unavailable, explicitly invalidate and reconnect the affected call instead of retaining stale success. Preserve the other family where possible. Do not perform blocking manager work on the QMI dispatch thread.

**Acceptance:** changes to IPv4/IPv6 addresses, DNS, gateway and MTU; duplicate reconfiguration events; settings temporarily unavailable; event during teardown; delayed event after CID reuse. Verify both actual interface state and Android's data-call list. A fresh query alone is insufficient if Android still receives the old cached result.

## F60 — Setup can miss the disconnect that already ended its session

**Sources:** [settings retrieval before listener installation](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/datacall.cc:280), [listener installation](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/datacall.cc:302), [success publication](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/datacall.cc:440), [client indication dispatch](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/client.cc:190).

`startLeg()` installs packet-status and service-loss callbacks only after START and current-settings retrieval succeed. The QMI dispatcher is already running. It consumes indications without retaining them for handlers registered later. A disconnect received in this window can therefore be lost; an in-flight settings response can subsequently complete setup with the old address. No final liveness check or pending-session state prevents publication.

**Reproduction:** accept START. While Get Current Settings is in progress, deliver DISCONNECTED and wait until the production dispatcher consumes it, then release the in-flight settings response containing the former lease. Setup returns success, one call remains cached, and there are zero loss callbacks. Delivering another disconnect after setup produces a loss callback, proving that the indication encoding and later listener work.

The test adds a passive early observer solely as a dispatch barrier. It neither repairs state nor changes production source. It forces a legitimate ordering, not the likelihood of that ordering on the phone. The returned settings deliberately represent a response already in flight when the session ended. This differs from F28's stale callback acting on a reused CID and F29's cache refresh races: here the relevant callback is never registered when the event is consumed.

**Proposed fix:** register lifecycle observers before starting the session. Track generation-scoped pending legs and latch disconnect/service-loss state through setup. Before publishing success, reconcile the pending leg's state and clean up if it has already ended. Simply moving the callback upward is insufficient if an early callback then tries to tear down a CID that has not yet been inserted into `mCalls`; it must invalidate the pending transaction as well. A final status query can supplement, but not replace, correct event ordering.

**Acceptance:** disconnect or service withdrawal before START reply, during settings retries, during link configuration and immediately before success publication. Require either failed setup or prompt ordered loss notification with no lingering active entry. Repeat with dual-stack partial failure and a new call reusing the same CID.

## F61 — Queued network time loses its age before reaching Android

**Sources:** [NITZ receipt and worker-queue handoff](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:755), [HAL forwarding](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioNetworkData.cpp:339), [worker dispatch](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:181), [blocking network refresh on that worker](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.cpp:735).

The core timestamps a network-time sample, queues its delivery, and passes that timestamp to `onNitz`. The HAL discards it and sends the current `elapsedRealtime()` together with age zero. The timestamp representing delivery to Android is appropriate, but age zero falsely says the sample spent no time cached in the RIL. The same worker performs synchronous modem queries, so queue delay need not be negligible.

The local Android AIDL contract defines `receivedTimeMs` as the send time and `ageMs` as the time cached in RIL/modem, measured with a clock that includes sleep. Android's `NitzSignal` derives the sample's reference time by subtracting age from receipt time. The contract is preserved in the source archive; [framework excerpts and hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round8-20260928/framework-time-evidence.txt) show the consumer.

**Reproduction:** compile the exact forwarding method with a recording indication and controlled elapsed clock. A sample timestamped at 10,000 ms and delivered at 14,000 ms produces `received=14000, age=0`; Android's age-adjusted reference is 4,000 ms too late. Immediate delivery is the positive control and has zero error. This is a method-level test with virtual time, not a four-second delay measured on the phone or a full Android time-detector test.

**Impact:** if this NITZ sample is used for time setting, its time estimate lags by the unreported queue delay. Other time sources may mask the defect. No claim is made about a measured wall-clock change, modem-internal age, or suspend duration.

**Proposed fix:** capture the sample's AP receipt time using the same suspend-inclusive boot clock used at delivery. Pass the current delivery time and the nonnegative elapsed age to Android, preserving any verified modem-provided age if available. Do not directly subtract the current `steady_clock` timestamp from `elapsedRealtime`: on Android/Linux these clocks differ in their treatment of suspend. Bound or discard stale/invalid samples rather than assigning them age zero.

**Acceptance:** immediate delivery, delayed worker, simulated suspend-inclusive delay, stale samples, and ordering across multiple samples. Verify the age-adjusted reference time remains the sample's original receipt time and that no Android wall clock is changed by the test harness.

## Evidence and scope

[Reproduction instructions](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round8-20260928/README.md), [results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round8-20260928/results.json), [source hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round8-20260928/source-hashes.json), and [snapshot](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round8-20260928/reviewed-source.zip) are retained. Both harnesses completed under ASan/UBSan without reported sanitizer errors. No snapshotted source changed during the run. Passing assertions mean the defects reproduced, not that those features passed acceptance.

This round also checked radio signal parsing and related SMS/data API candidates. The SMS-memory no-op is already noted under F26 and was not counted again. IPv6 address byte order matches the referenced QMI network-order layout and was not promoted to a finding. Broad questions about single-family loss policy, unsupported features and actual hardware reliability remain separate from these three confirmed cases.
