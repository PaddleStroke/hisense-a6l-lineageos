# A6L hardware integration review — second pass

Opened 28 September 2026. Companion to the [first hardware review](hardware-review-20260928.md) and [VoLTE/camera review](volte-camera-review-20260928.md).

Follow-up: [the deeper audit](hardware-review-deep-20260928.md) adds F23–F34 and a shared plan for testing lifecycle, ordering, failure and consumer-visible behavior.

**Nine additional findings, F14–F22.** The most consequential are missing cellular interface address configuration, false data-setup success, radio recovery/state inconsistencies, and exhausted call-audio retries. GNSS and e-ink input also have reproducible failure paths. These findings are separate from F1–F13, whose fixes Claude is integrating.

This pass concentrates on the boundary between a successful driver/recovery test and usable Android behavior, plus failures after initial startup. It does not repeat the H1–H64 inventory or claim nine newly observed phone failures. No phone access, RF operation, ROM build, firmware/partition change, or production-source edit was performed. Added only review documentation and isolated host tests.

## Evidence and freshness

- Five host harnesses compile the actual implementation with fake modem, device, or link operations. All defect assertions completed under ASan/UBSan without sanitizer diagnostics. These assertions deliberately describe the **broken behavior**, not passing feature acceptance tests.
- [Source hashes](../research/hardware-review-pass2-20260928/source-hashes.json) identify the copied C/C++ sources. [The concurrent-change check](../research/hardware-review-pass2-20260928/changed-during-run.json) found **zero changes to those inputs during the final run**. Claude may change them afterward; verify the current code before applying a fix. The hashes are not a ROM/build identity or a hash of every platform file inspected.
- At report close, four hashed files had subsequently changed: `radio/qmi/src/services.cc`, `radio/qmi/include/a6lqmi/services.h`, `radio/hal/RadioImpl.h`, and `radio/hal/RadioNetworkData.cpp`. The current files contain F12 broadcast/attach-APN integration changes; the data-result address mapping was rechecked and remains as described below. The host results apply to the recorded snapshot, not a claimed retest of those later edits. The main implementation files containing F15–F22 were unchanged at that check.
- The reviewed q6voiced already contains the first review's F6/F7 integration work. F19 below remains present in that version; it is not a repeat of the missing-mute or mixer-retry findings.
- The newer [volte6 report](volte6-20260929.md) supersedes earlier IMS-handshake interpretations: instance 1 in the destroy message is GLOBAL, and the new experiment reasserts IMS after SIM readiness and inspects/sets NAS voice-domain preference. Its root-cause section still says unproven. Nothing in this second pass establishes successful IMS registration or fixes the reported camera streaming failure.

Paths in findings are relative to `device/hisense/a6l/` unless stated otherwise. P1 means resolve before relying on the affected function; P2 means a narrower correctness/recovery problem. Severity describes the impact **if the documented trigger occurs**, not its measured frequency on this phone.

## F14 — P1: Android cellular data never assigns the modem's addresses to the interface

**Evidence:** `radio/qmi/src/datacall.cc:294–307` creates the rmnet interface and requests link-up/MTU. `radio/qmi/src/rmnet.cc` implements those operations but no address assignment. `radio/hal/RadioNetworkData.cpp:327–334` exports the modem's addresses in `SetupDataCallResult`; `radio/minradio/data/RadioData.cpp:39–46` caches/notifies the result. Reporting addresses to Android does not itself install them on the Linux interface.

I checked the local platform tree, `/home/a6l/android/a6l-lineage24`: `frameworks/opt/telephony/src/java/com/android/internal/telephony/data/DataNetwork.java:2898–2939` constructs `LinkProperties` and adds the reported addresses; `packages/modules/Connectivity/service/src/com/android/server/ConnectivityService.java:11048–11099` updates network membership, routes, DNS and related state, without assigning the modem's addresses. No matching address-configuration operation was found in the reviewed ROM path.

By comparison, `kernel/ipa/bundle/run.sh:159–161` explicitly runs `a6l-net addr`, `ip addr add`, or `ifconfig`. Thus the recovery data test performs an essential step missing from the Android path. An IPv4 WDS session can succeed and appear in Android's link properties while the interface lacks its usable local address. IPv6 needs deliberate handling too; do not assume its modem settings and any kernel autoconfiguration are interchangeable.

**Proposed fix:** give a defined, privileged component ownership of interface address configuration, using rtnetlink or the appropriate platform service. Apply valid family-specific addresses/prefixes before reporting usable data; track and remove owned addresses on teardown/reconfiguration. Leave Android network policy, routing and DNS ownership with the framework. Check capabilities and SELinux access in the installed ROM.

**Validation:** on Android, compare WDS settings, actual `ip address` output, and `dumpsys connectivity` for IPv4, IPv6 and dual-stack APNs. Exercise traffic through that Android network, teardown, reconnect and address changes, with no recovery helper having configured the interface. The host trace proves which link operations run; the missing-address conclusion also depends on the static framework/caller review, not a real networking experiment.

## F15 — P1: data setup reports success after failed prerequisites

**Evidence:** `radio/qmi/src/datacall.cc:207–210` continues after mux binding or IP-family selection fails. At `233–251`, failed current-settings retrieval still produces a successful leg. At `296–307`, link-up/MTU results are ignored before `out.ok = true`.

The real data manager with a fake modem/link layer reproduced all three cases:

- Current-settings failure: `ok=1`, **zero addresses**.
- Mux-bind rejection: a START request is still sent and setup returns `ok=1`.
- Link-up failure: setup still returns `ok=1`.

The normal control case returned one address and followed the expected create/up/MTU/up sequence. IP-family and MTU failures are additional source-confirmed unchecked results, not separately injected cases. This finding concerns the Android data manager; the previous VoLTE review identified a related false-success pattern in the IMS data path.

**Proposed fix:** make setup transactional. Require successful mux/family configuration, a valid session handle, usable family settings and successful interface configuration. Where settings legitimately arrive later, use bounded retries and an explicit pending state. On failure, stop the WDS sessions and clean up only resources owned by this setup. Preserve legitimate single-family fallback for dual-stack requests rather than demanding both families indiscriminately.

**Validation:** fault each prerequisite independently, including address installation from F14. Assert accurate failure reporting and no leaked sessions/interfaces; then clear the fault and establish data successfully. Test dual-stack partial success separately.

## F16 — P1: a fatal QMI receive error leaves a permanently stale client

**Evidence:** `radio/qmi/src/client.cc:133–141` exits the reader on a negative transport result, but leaves `mRunning` true, cached services present, and pending requests waiting. `start():97–104` then returns success without reopening. `radio/hal/ModemCore.cpp:191–239` does not supervise this failed reader as a new connection attempt.

Injected fatal receive failure produced `cached_service=1`, `down_events=0`, a request timeout, and `start_again=1` with **only one transport open**. This is specifically the fatal-error branch: the QRTR transport already treats EINTR, EAGAIN and ENETRESET as transient. It is not evidence that every ordinary modem restart takes this path.

**Proposed fix:** introduce an explicit failed-transport state, immediately fail outstanding requests, invalidate service/endpoints and notify dependents. A supervisor should close/reopen with backoff, rediscover services, and restore registrations. Preserve correct thread ownership: simply setting `mRunning=false` is insufficient because `stop():115` currently returns early in that state and would skip joining/closing. Do not join the reader from itself.

**Validation:** fatal read while idle and while requests are outstanding; transport-open failure followed by recovery; repeated start/stop; shutdown during backoff. Assert prompt failure, exactly one active reader/dispatcher pair and restored service indications.

## F17 — P1: isolated VOICE/WMS/WDS service loss does not trigger reinitialization

**Evidence:** `radio/hal/ModemCore.cpp:501–507` has an IMSA-specific path, otherwise ignores service-up and only treats UIM/NAS/DMS withdrawal as requiring reinitialization. VOICE and WMS are required at startup, but their independent disappearance does not invalidate the ready state or restore their initialization on return. WDS loss likewise lacks this core recovery trigger.

The core harness established an active call, withdrew **only VOICE**, and observed `ready=1`, `voice_active=1`, and one cached call. When VOICE returned at a new endpoint, the indication-registration count stayed **1 → 1** after 1.3 seconds, longer than the connect-loop interval. This confirms the VOICE path. The WMS/WDS variants follow from the service filter and need their own targeted acceptance tests. A whole-modem restart that also removes DMS/UIM/NAS is a different, already-handled trigger.

**Proposed fix:** track readiness and generations per service. Invalidate affected state on withdrawal and rebind/re-register when that service returns. VOICE loss must clear stale calls and audio activation; WMS must restore routes/event registration and handle pending acknowledgements; WDS must reconcile data sessions and notify Android. Avoid resetting unrelated healthy services unnecessarily.

**Validation:** independent VOICE, WMS and WDS withdrawal/return with unchanged DMS/NAS/UIM; new endpoint addresses; no stale call/audio/data state and no duplicate indications after restoration.

## F18 — P1: failed radio power-off changes the reported slot state anyway

**Evidence:** `radio/hal/ModemCore.cpp:839–846` changes the shared power vote before sending DMS SetOperatingMode. On rejection it returns false without undoing/reconciling that change. `radioOn():824–834` combines the new vote with the previous modem state, so its answer can change even though the requested operation failed.

With a single online slot and a rejected power-off request, the harness observed `accepted=0`, `reported_on=0`, **`actual_modem_online=1`**. The fake modem supplies the last value; no RF behavior was measured on hardware.

**Proposed fix:** separate requested votes from acknowledged/applied state. Serialize physical modem mode transitions across slots; publish the new applied state only after success or reliable readback. On timeout, reconcile the uncertain result. A simple uncoordinated vote rollback can itself overwrite a concurrent request from the other slot.

**Validation:** rejected power-on/off, timeout with and without a physical transition, both slots requesting changes concurrently, and readback reconciliation. Android must not be told an applied radio state solely because that state was requested.

## F19 — P1: five transient call-audio open failures silence the rest of the call

**Evidence:** `kvoice/q6voiced/a6l_q6voiced.c:488–494` attempts `voice_open()` only while `fails < 5`. Once exhausted with `want=1` and `v.tx < 0`, it never tries again until call activity clears or the daemon restarts. The route-change reset at `483–486` requires an already-open TX descriptor, so changing route cannot recover this failed-open state.

The real daemon loop with a fake PCM open and accelerated sleeps ran 12 active iterations and made **exactly five open attempts**. No sound device was opened. With immediate failures, those attempts are approximately one second apart from first to last at the normal 250 ms loop interval; a DSP or PCM becoming ready later cannot help the current call. Actual open duration or socket servicing can change that timing.

**Proposed fix:** limit retry frequency, not the total opportunity to recover while a call remains active. Use capped backoff, log state transitions, reset on relevant device/DSP readiness changes, and cancel promptly when the call ends. Clean up partial opens and preserve current routing/mute state when reopening.

**Validation:** reject the first five or more opens, make the fake/real PCM available while the same call remains active, and require recovery without redial. Also test hang-up during backoff and DSP recovery during a call. This is separate from F7's mixer-write retry problem.

## F20 — P1: e-ink power-key interception survives loss of its delivery/state path

**Evidence A — unavailable uinput:** `eink/switcher/native/a6l_dualux.c:164–175` permits startup after uinput creation fails; emitted events then do nothing. `apply_grabs():205` still selects the physical power-key grab from display state. In e-ink mode, successful physical grabbing can therefore hide the key from Android while the replacement long-press/wake/sleep events cannot be delivered. Uinput initialization is not retried.

**Evidence B — lost events:** `drain():224–234` ignores every non-key event, including `SYN_DROPPED`, and closes a lost input FD without reconciling the key state. A dropped release can leave a long press or injected power-down outstanding. Linux requires consumers to discard through the next SYN_REPORT and query the current device state after an overrun; see [Linux input event synchronization](https://docs.kernel.org/input/event-codes.html#ev-syn).

The harness set `ui=-1` and observed the real grab policy still select `power_grabbed=1`. It used a regular file, so **it did not exercise a real EVIOCGRAB ioctl**. Separately, feeding SYN_DROPPED/SYN_REPORT to the real drain function left `power_down=1` and `injected_down=1`. These prove policy/state-handling defects, not a physical stuck-key reproduction. Existing no-uinput tests that inspect “inject” log lines cannot establish delivery to Android.

**Proposed fix:** require successful uinput creation/capability setup before taking over the physical power key, and track actual grab success. Fall back to normal Android key delivery if takeover cannot operate; retry readiness. Implement SYN_DROPPED resynchronization with EVIOCGKEY, cancel stale timers, and balance any outstanding virtual key-down. Reset/reconcile state on input-device loss and reappearance too.

**Validation:** unavailable uinput at startup then recovery, setup/write failure, rejected grab, event overrun, and input unplug/reopen during a press. Verify delivered key edges through Android input observation, not only daemon logs; retain a working physical power-key fallback.

## F21 — P1: transient GNSS configuration/start failures are sticky

**Evidence:** `gnss/lib/loc_client.cpp:262–270` ignores configuration command results and sets `configured_=true`, including after failed event registration. At `284–285`, a rejected START leaves `session_=false`. `workerLoop():497–527` retries missing transport/service transitions but has no reconciliation deadline for a desired active session whose configuration/start failed while LOC remains present.

Two fake-modem runs rejected REG_EVENTS or START, then cleared the error without withdrawing LOC. In both, the failing command's count remained **1 → 1** for the following 1.3 seconds. Failed registration still led to `session=1`; failed START left `session=0` with activation desired. The source has no periodic retry for either condition, so merely waiting longer does not supply a recovery trigger. The registration test does not simulate the hardware's actual delivery of position indications.

**Proposed fix:** require acknowledgements for essential configuration before marking it applied. Track desired versus applied session state and schedule capped retries for transient failures; handle unsupported optional commands separately. Reset configuration on the appropriate service generation and cancel retries on stop. This engine already has transport-failure recovery; the defect is command failure while the service remains available.

**Validation:** each essential configuration command and START fails once, then succeeds with LOC continuously present. Require registration and position delivery without toggling location again; verify stop during backoff and absence of duplicate sessions/events.

## F22 — P2: GNSS reports mean-sea-level altitude as ellipsoid altitude

**Evidence:** `gnss/lib/android_map.cpp:16–22` correctly prefers ellipsoid altitude, but falls back to MSL and still sets HAS_ALTITUDE. Android's GNSS `altitudeMeters` field is height above the WGS84 reference ellipsoid; these are different vertical datums. See the [official GnssLocation AIDL contract](https://android.googlesource.com/platform/hardware/interfaces/+/refs/heads/main/gnss/aidl/android/hardware/gnss/GnssLocation.aidl).

The mapping test supplied only MSL altitude 123 m and received `HAS_ALTITUDE=1`, `altitudeMeters=123`. That invents an ellipsoid measurement the input did not provide. No particular numerical offset is assumed; the difference depends on location.

**Proposed fix:** omit HAS_ALTITUDE when only MSL is available, unless a valid datum conversion is actually performed. Retain MSL separately where an appropriate API supports it. Keep vertical accuracy consistent with the altitude being reported.

**Validation:** ellipsoid-only, MSL-only, both, neither, assumed/invalid altitude, and any supported conversion with a known geoid reference. Confirm latitude/longitude remain usable when altitude is omitted.

## Reproduction artifacts and checklist additions

Run in WSL: `python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-pass2-20260928/reproduce.py`.

The [runner](../research/hardware-review-pass2-20260928/reproduce.py) copies implementation sources into a temporary directory, compiles only review executables, and uses fake endpoints/files. It does not run a ROM build or configure the host network. [Raw results](../research/hardware-review-pass2-20260928/results.txt), per-harness stderr/build logs, and source hashes are retained alongside it.

| Harness | Evidence reproduced | Result |
|---|---|---|
| [radio_review.cc](../research/hardware-review-pass2-20260928/radio_review.cc) | Fatal-reader stall; settings/bind/link false success; normal link-operation trace | Defect assertions reproduced |
| [core_review.cc](../research/hardware-review-pass2-20260928/core_review.cc) | Failed power-off state mismatch; isolated VOICE loss/return | Defect assertions reproduced |
| [voice_review.c](../research/hardware-review-pass2-20260928/voice_review.c) | Permanent retry exhaustion within an active call | Defect assertions reproduced |
| [dualux_review.c](../research/hardware-review-pass2-20260928/dualux_review.c) | Grab policy without uinput; missing dropped-event recovery | Defect assertions reproduced |
| [gnss_review.cc](../research/hardware-review-pass2-20260928/gnss_review.cc) | Configuration/start retry gap; incorrect MSL mapping | Defect assertions reproduced |

ASan/UBSan success does not establish absence of races or resource leaks. The core harness follows the existing process-lifetime fixture and exits without full teardown, so it is not a shutdown/leak test. Convert defect assertions to corrected expectations when fixes land; the current runner is intentionally expected to stop reproducing these failures after fixes.

Add these explicit acceptance items to the hardware checklist, rather than closing broad rows after a recovery demo:

1. Cellular data: modem settings **and actual Linux addresses**, Android traffic, teardown/readdressing, individual prerequisite failures.
2. Radio: fatal transport failure, independent service restart, accurate applied power state and dual-SIM transitions.
3. Call audio: delayed DSP/PCM readiness and recovery without hanging up.
4. E-ink/input: uinput failure, grab failure, input overrun and balanced power-key delivery.
5. GNSS: transient command failures while LOC stays present, plus altitude datum correctness.

Prioritize F14–F19 alongside the first report's daily-use blockers. F20/F21 should be covered before trusting input and location recovery. No additional hardware or camera success is inferred from this host-only pass; the first report's full inventory and attended validation gaps remain applicable.
