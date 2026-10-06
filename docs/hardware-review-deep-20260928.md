# A6L deeper integration audit

28 September 2026 session; includes repository work labelled 29 September. Builds on the [H1–H64 hardware inventory and F1–F13](hardware-review-20260928.md), [F14–F22](hardware-review-pass2-20260928.md), and [VoLTE/camera review](volte-camera-review-20260928.md).

**Twelve additional finding groups, F23–F34, with reproductions.** More important than the count: the same design weaknesses recur across subsystems. Commands lose important options between Android and QMI; identifiers outlive the objects they name; queued work has no generation/cancellation rules; and tests frequently establish producer success without checking the consumer's result.

The highest-priority new finding is that an emergency-call request explicitly marked as a test reaches the ordinary modem emergency-dial path. Other consequential findings concern caller-ID privacy, dropped cell broadcasts, premature SMS deletion, wrong-message acknowledgements, stale network state, data connection teardown, sensor timing/calibration, and GNSS cancellation.

This was an **offline source and fault/interleaving review**. Six isolated host harnesses completed with ASan/UBSan, using fake devices/modems or extracted method bodies with stub dependencies. No phone, network call, emergency service, RF state, partition, firmware, ROM build, or production source was changed. These results demonstrate software behavior under the stated inputs; they do not measure how often those inputs occur on the phone or certify physical hardware.

## Scope and evidence boundaries

This pass followed complete paths and adverse event sequences instead of repeating the component inventory:

| Boundary inspected | New result or explicit limit |
|---|---|
| Android dial request → HAL → QMI | F23/F24: test/routing/identity options are lost |
| WMS indication → worker → Android → acknowledgement/storage | F25–F27: missing broadcast delivery, premature deletion, mismatched acknowledgement ordering |
| WDS session → deferred callback → Android data executor | F28: a reused CID identifies the wrong connection |
| QMI query/indication → shared registration cache | F29: an older response overwrites a newer indication |
| SMGR FIFO → time conversion → decimation → HAL event queue | F30/F31: delayed scans lose timing/events; queue does not preserve control boundaries |
| Raw gyro → bias estimate → calibrated Android sensor | F32: slow real movement is learned as bias |
| GNSS assistance transaction → acknowledgement → session control | F33/F34: invalid confirmations count as success; assistance blocks STOP |
| E-ink input/capture, audio routing, USB selection | Reviewed lifecycle/source paths; retain F6/F7/F19/F20 and the existing hardware validation gaps. No new defect promoted here without stronger evidence |
| New Wi-Fi HAL, charger guard, init/product/policy selection | Inspected current F1/F2/F5/F8/F11/F13 work and selection. This is not acceptance of the fixes or proof they reached an installed image |
| Camera, thermal/DVFS, Bluetooth audio, encryption/security, storage | Existing integration/attended-test gaps remain; no new phone evidence. Do not relabel them as new discoveries merely because they remain unfinished |

The obsolete USB gadget stub is excluded by the actual ROM integration path; it is **not** counted as a new installed-ROM bug. The sensor bridge already takes a scoped wake lock for wake-up events, so absence of that operation inside the C sub-HAL alone is **not** a finding. The new off-mode charging init path supersedes the first report's claim that there was no `on charger` module path; charger-mode acceptance remains outstanding. Fingerprint and absent NFC remain intentional exclusions.

The local Android tree `/home/a6l/android/a6l-lineage24` was used to check contracts and bridge behavior, including `hardware/interfaces/radio/aidl/android/hardware/radio/voice/{IRadioVoice,Dial}.aidl`, `hardware/lineage/interfaces/sensors/SensorsSubHal.cpp`, and the radio wake-lock reference. It is not assumed to contain every edit Claude is currently preparing.

Paths below are relative to `device/hisense/a6l/`. P1 = fix before relying on the affected behavior; P2 = narrower correctness problem. Numbering continues earlier reports. F25 extends F12 into the receive path; it does not repeat the already identified broadcast-configuration stub.

## F23 — P1: emergency-call testing and routing semantics are discarded

**Source:** `radio/hal/RadioMessagingVoice.cpp:290–303`; `radio/qmi/src/services.cc:995–999`.

`emergencyDial()` logs `isTesting`, `routing` and user intent, then captures only the address in its queued operation. It always calls `qv::dial(..., true, ...)`. On **any** failure it retries with `emergency=false`, even if the original request required emergency routing. A timeout does not establish that the first call was never created, so unconditional redial is also unsafe transaction handling.

The local `IRadioVoice.aidl:83–144` explicitly requires a test request not to reach a real emergency service. NORMAL and EMERGENCY routing also have different required behavior. The optional category/URN limitations of a particular radio technology do not justify ignoring those controls.

**Reproduction:** compiled the unmodified `dial`/`emergencyDial` method bodies with in-memory executor, response and modem stubs. `isTesting=true` generated one emergency modem request; NORMAL routing generated an emergency request first; a rejected EMERGENCY request generated a second, non-emergency request. This does not exercise Binder or place a call.

**Proposed fix:** preserve all relevant request fields in a typed request through the executor and protocol encoder. Implement a verified test-call path, or reject unsupported test requests before issuing any dial. Respect routing and explicit user intent. Limit any compatibility fallback to an identified, verified unsupported-encoding response, never arbitrary failure/timeout; reconcile ambiguous call creation before retrying.

**Acceptance:** mock-modem tests for testing × routing × intent, plus unsupported command, explicit rejection and timeout after possible call creation. A test request must produce zero live-dial operations. Use mock endpoints for these automated tests.

## F24 — P1: per-call caller-ID restriction is silently ignored

**Source:** `radio/hal/RadioMessagingVoice.cpp:274–285`; `radio/qmi/src/services.cc:995–999`.

The ordinary dial method logs `d.clir` but captures only `d.address`. The QMI helper accepts no caller-ID restriction and encodes only the number plus optional emergency type. Android's `Dial.CLIR_INVOCATION` requests restricted presentation; the call instead uses the subscription/network default. If that default reveals identity, a caller's explicit privacy request is not honored. The review did not observe an actual number disclosure.

**Reproduction:** the actual extracted method with CLIR=1 issues an ordinary dial with the unchanged number and no restriction argument. The lower-layer encoder source confirms there is no hidden CLIR mapping.

**Proposed fix:** carry default/restrict/allow explicitly into a verified VOICE dial encoding. Do not silently substitute the network default when the requested restriction cannot be applied; return an accurate error. Treat persistent CLIR supplementary-service settings separately from a per-call override.

**Acceptance:** inspect exact outgoing request fields for all three CLIR values and modem rejection. An ordinary-call test with consenting endpoints can subsequently validate presentation; the host test only establishes option propagation.

## F25 — P1: configured cell broadcasts are discarded on reception

**Source:** `radio/hal/ModemCore.cpp:666–689`; `radio/hal/ModemCore.h:33–47`; `radio/hal/RadioMessagingVoice.cpp:195–201`.

Claude's F12 work now programs broadcast activation/configuration. The receive path still accepts only `wms::kFormatGwPp` (6); `kFormatGwBc` (7) enters the “ignoring MT SMS” branch. There is no corresponding broadcast listener/delivery path to Android's `newBroadcastSms` indication. Sending broadcast subscription commands successfully cannot make alerts appear through this implementation.

**Reproduction:** a fake WMS transfer indication with format 7 reaches the real core's ignore branch. The fixture deliberately requested acknowledgement and also observed a negative acknowledgement; the central defect is the drop, not an assertion that real broadcast indications always request such an acknowledgement.

**Proposed fix:** implement a distinct broadcast receive path, preserve the PDU format Android expects, and deliver `newBroadcastSms`. Handle any modem-specific broadcast/ETWS indication forms explicitly. Do not send broadcasts through point-to-point SMS acknowledgement/storage logic. Verify routing as well as activation/configuration on this modem.

**Acceptance:** replay representative GSM/UMTS/LTE broadcast and warning PDUs through to a captured Android callback, including language filtering, multipart handling where applicable and restart/reconfiguration. Use synthetic/replayed alerts. This closes the receive side of F12; configuration tests alone are insufficient.

## F26 — P1: stored SMS is deleted before Android confirms it accepted the message

**Source:** `radio/hal/ModemCore.cpp:690–707,713–727`.

On a stored-message indication, the core reads the PDU, deletes the modem's copy, and only then delivers it to listeners. Its queued acknowledgement record carries no storage/index ownership. A failed Android delivery, negative acknowledgement or absent listener therefore cannot preserve/retry that stored copy. The comment “Android keeps its own copy” describes a future operation that has not happened yet.

**Reproduction:** the real core sends WMS Delete before invoking the first SMS listener. That listener rejects receipt. The copy was already deleted. This is specific to the stored-message path: the default transfer-only route does not mean every SMS takes this path, but the implemented stored-route/fallback path is unsafe when used.

**Proposed fix:** retain storage/index and payload ownership until Android positively acknowledges acceptance. On negative acknowledgement, callback failure or framework reconnection, preserve and retry/re-enumerate according to an explicit delivery policy. Handle delete failure separately so successful delivery can be deduplicated without losing messages.

**Acceptance:** no listener, failed callback, memory-full rejection, successful receipt, duplicate indication, restart before acknowledgement and failed delete. Require no deletion before durable acceptance and no permanent silent loss. Also implement or accurately reject `reportSmsMemoryStatus`, which currently logs “not forwarded” and returns success (`RadioMessagingVoice.cpp:126–129`). That related omission is not counted separately.

## F27 — P1: mixed SMS paths can acknowledge the wrong transaction

**Source:** `radio/hal/ModemCore.cpp:682–707,713–725`.

Transfer-route acknowledgement entries are appended on the QMI dispatcher, while stored-route placeholders are appended later on the core worker. Delivery happens on the worker for both. These two orderings can disagree.

**Reproduction sequence:** hold the worker; deliver stored message A, then transfer message B; release the worker. A is delivered first, but B's acknowledgement was queued first. Rejecting A sends a negative WMS acknowledgement for B's transaction **0x2222 before B has even been delivered**. The later stored placeholder can then consume B's framework acknowledgement without sending its required modem acknowledgement.

**Proposed fix:** create the delivery record and its acknowledgement identity together on one ordered owner. Keep modem transaction or storage identity attached to that record, and expose only the framework-compatible outstanding delivery sequence. Include slot, framework connection and modem generation so restart cannot acknowledge an old transaction accidentally.

**Acceptance:** interleave transfer/stored/status-report messages under delayed reads, blocked callbacks, modem restart and framework reconnection. Assert the transaction acknowledged is precisely the one whose delivery Android accepted/rejected, including no-ack messages. The reproduction uses the real core with fake modem/listener and controlled thread ordering.

## F28 — P1: an old data-loss callback can destroy a new connection with the same CID

**Source:** `radio/qmi/src/datacall.cc:119–124,242–247,273–309`; `radio/hal/ModemCore.cpp:137–139`; `radio/hal/RadioNetworkData.cpp:469–476`.

Loss notifications capture only `cid`. That CID is the mux ID and is immediately reusable after deactivation. Notifications cross the core worker and the data executor before performing `deactivate(cid)`; no connection generation is checked. An old loss can consequently target a replacement session.

**Reproduction:** create A at CID 1; queue its loss; deactivate A; create B, again CID 1; execute the deferred loss operation. B is removed and the manager's list becomes empty. This uses the real data manager and a deferred callback list matching the HAL's queued operation; it is not a full Binder scheduling test.

**Proposed fix:** assign each call a monotonically changing generation/token. Carry `(cid, generation)` through every delayed indication, teardown and Android update, and ignore work for a retired generation. Apply equivalent ownership to per-family WDS handles and modem reset. Do not rely on a longer delay before reusing four available mux IDs.

**Acceptance:** old disconnect before/after explicit teardown, immediate CID reuse, dual-stack indications arriving separately, and reset while setup is queued. An old callback must never mutate the replacement connection.

## F29 — P1: stale query results overwrite newer modem state

**Source:** `radio/hal/ModemCore.cpp:604–616,765–774`; related cache getters at `730–807`.

`serving(true)` issues a synchronous query and later unconditionally writes the response into `mServing`. Meanwhile the dispatcher can apply a newer registration indication. The older query then restores stale state, potentially undoing the roaming update used by F9's data policy. A mutex prevents simultaneous assignment; it does not establish which observation is newer.

**Reproduction:** hold a home-network query response; deliver a roaming indication and verify the cache becomes roaming; release the older response. The cache changes back to home (`1 → 0`) without another network transition.

**Proposed fix:** version state updates. Capture the service/cache generation at query start and reconcile or discard a response when a newer indication or reset has intervened. Serialize state publication through one owner where practical. Apply the rule to card, registration, call and operator caches. Also bring the unlocked cache-presence checks (`!mCard`, `!mServing`, `!mOpName`, `mIccid.empty()`) under their mutex; the functional replay is not a ThreadSanitizer test of these additional races.

**Acceptance:** both response/indication orderings, reset while a query is in flight, failed refresh with an older cached result, and concurrent slot requests. Consumers making policy decisions must be able to distinguish fresh, stale and unavailable state. This is separate from F18's failed power-vote commit.

## F30 — P1: delayed sensor reads corrupt timestamps and discard valid samples

**Source:** `hals/sensors/stk3338/a6l_motion.c:166–194,694–760,764–789`.

Each 64-record read is independently anchored as though its last record were current. If a backlog is waiting when the first clock anchor is established, the oldest chunk is incorrectly treated as recent. Subsequent chunks move the inferred clock offset backward; the monotonic timestamp clamp compresses their spacing, and decimation discards samples that appear too close together. The configured IIO buffer holds 256 records, so a backlog larger than one read is within its design capacity.

**Reproduction:** identical 256 scans at 200 Hz: prompt reads produce **256 events**; four delayed 64-scan chunks produce **64 events**. The first timestamp is **960.028 ms too late**. The harness feeds the real processing/time-mapping code with deterministic arrival times and scan data. It does not measure the phone's actual scheduling delays.

**Proposed fix:** establish clock correspondence from a defensible common anchor, preserving sample intervals across a complete backlog; do not equate every read boundary with acquisition time. Drain/inspect the available batch before choosing its newest anchor, or use a validated AP/DSP clock correlation. Separate monotonicity repair from rate decimation so repaired timestamps cannot silently erase a batch. Account for suspend, tick wrap and DSP reset explicitly.

**Acceptance:** 1/64/65/128/256-record backlogs, first activation and re-enable, delayed poll thread, clock wrap and DSP restart. Check event counts and acquisition-time spacing, not just increasing timestamps. Android batching concerns measured events and their maximum delivery delay, not arbitrary userspace read chunks; see [AOSP sensor batching](https://source.android.com/docs/core/interaction/sensors/batching).

## F31 — P2: the sensor queue does not preserve disable and flush boundaries

**Source:** `hals/sensors/stk3338/a6l_motion.c:113–133,360–373,392–399,793–815`.

Disabling a handle clears its enabled/flush counters but does not purge its queued samples. `a6l_motion_pop()` returns them without checking activation state/generation. Separately, the queue's indiscriminate drop-oldest rule can discard `META_DATA_FLUSH_COMPLETE`, which is a control completion rather than an expendable sample.

**Reproduction:** a queued accelerometer event is returned after its handle is disabled. A queued flush completion followed by 2048 sample events is discarded (`flush_completions=0`, `dropped=1`). The overflow test directly exercises the real queue policy; it does not claim that a particular phone workload has already filled it. Re-enabling a handle before draining also makes an activation generation useful for distinguishing old samples.

**Proposed fix:** define queue ownership at activation boundaries, remove/retire samples for disabled generations, and preserve mandatory completion records independently of sample overflow. Reconcile pending flushes according to the HAL contract rather than clearing counters without an explicit rule. Ensure the bridge sees the correct completion count and ordering.

**Acceptance:** disable with queued events, rapid disable/re-enable, multiple flush requests, mixed sensor handles, queue overflow and driver loss during flush. Verify the final HAL/bridge callback sequence as well as internal queue state.

## F32 — P2: gyro calibration learns real slow rotation as zero bias

**Source:** `hals/sensors/stk3338/a6l_motion.c:601–637,655–659`.

The stationary detector uses only gyro mean/variance. A constant real angular velocity below 0.05 rad/s per axis qualifies as stationary, is learned as bias, and is subtracted from calibrated measurements. The resulting sensor can report approximately zero angular velocity while the phone is steadily turning; the code then reports high calibration accuracy.

**Reproduction:** 221 samples, 5 ms apart, of a real Z rotation of **0.04 rad/s** (about 2.29 degrees/s). After the one-second window, learned Z bias is 0.04 and corrected Z is effectively zero. This is a mathematical ambiguity in the implemented estimator, independent of a hardware driver failure.

**Proposed fix:** use a validated bias estimator with independent motion evidence and conservative confidence handling. Gyro stability alone cannot distinguish constant slow motion from bias. Gate learning appropriately, retain trustworthy calibration, and avoid declaring high accuracy from this test alone; simply checking stable gravity would still miss yaw rotation.

**Acceptance:** verified rest with injected bias, slow constant rotation, changing rotation, vibration and temperature drift. Compare calibrated/uncalibrated outputs and an independent angle reference. Calibration must remove sensor bias without removing genuine angular motion.

## F33 — P2: GNSS assistance reports success with invalid part confirmations

**Source:** `gnss/lib/loc_client.cpp:349–396,455–457`; `gnss/lib/loc_v02.cpp:157–162`.

The XTRA uploader ignores the parser's boolean result. A malformed indication with no mandatory status leaves the local status initialized to zero, so it is treated as success. A confirmation for the wrong part number is only logged. Some missing-confirmation sequences also fall through to success; `onXtraResult(ok)` is emitted before the later validity query.

**Reproduction:** all three upload parts receive part number 99 instead of the expected part; the engine reports success. A second run supplies empty indications with no status; it also reports success for all three parts. These are real engine/parser paths with fake modem responses. The missing-indication fall-through is source-reviewed, not a separate timeout reproduction.

**Proposed fix:** require a valid confirmation correlated to the current transfer and part. Reject malformed records, ignore unrelated/stale parts until the deadline, and fail an unconfirmed transfer. Distinguish “all parts acknowledged” from “usable assistance verified” if validity is checked later. Do not infer that a successful QMI envelope confirms the payload transaction.

**Acceptance:** empty/truncated indication, wrong/duplicate/out-of-order part, old transfer's confirmation, missing final confirmation, explicit error and valid upload. Check both reported result and later validity without turning a failed upload green.

## F34 — P1: GNSS assistance blocks stop/session-control work

**Source:** `gnss/lib/loc_client.cpp:293–309,339–396,408–458,497–527`; `gnss/hal/Gnss.cpp:209–214`.

Assistance upload, indication waits and retry sleeps occupy the sole worker that processes session start/stop. `setActive(false)` changes desired state but queues STOP behind the upload. The upload checks neither that desired-state change nor a control-task cancellation condition while waiting for parts. The Android HAL can return from stop while the modem's session remains active.

**Reproduction:** start a session, begin an upload whose first part gets a QMI response but no completion indication, then request stop. After **1.2 seconds**, no STOP request has been sent and `sessionRunning()` remains true. Source waits are five seconds per part; three initially missing confirmations can consume roughly 15 seconds before the transaction fails, with other queued query waits potentially adding delay. This is a bounded observed delay plus a source-derived longer case, not a measured phone power figure.

**Proposed fix:** make assistance an interruptible transaction/state machine, give session stop priority, and cancel or pause waits when the relevant control/session generation changes. Do not introduce uncontrolled concurrent access to the client merely to bypass the worker. Preserve the ability to process assistance when intentionally requested without an active fix session, but do not let it keep a stopped session running.

**Acceptance:** stop/close during source query, each part wait and retry backoff; service loss and rapid start-stop-start. Require a bounded STOP dispatch and no stale operation restarting a canceled session. `mActive` already gates location delivery in the HAL, so this finding does **not** claim that location callbacks necessarily continue after stop.

## Common fixes that address the pattern

| Design weakness | Examples | Shared engineering change |
|---|---|---|
| Requested state/options are dropped or mistaken for applied state | F18, F23/F24, F33 | Typed requests carried end-to-end; explicit requested/pending/applied/failed state and truthful completion |
| Resource identity has no lifetime | F17, F27–F29 | Service, session, connection and framework-client generations on delayed work |
| Multiple queues disagree about ownership/order | F27, F29, F31 | One ordered state owner per subsystem; record payload and completion identity together |
| Recovery work monopolizes control processing | F16/F19/F21, F34 | Cancellable asynchronous transactions, capped retry frequency, priority stop/shutdown |
| Host tests do not check the downstream contract | F14/F25/F26/F30/F33 | Assert actual consumer events, kernel configuration, retained data, timing and rejected work |

These changes should be applied deliberately within each subsystem, not as a large generic framework rewrite. Fix the highest-impact findings first, then use the shared rules to review sibling paths that the individual tests do not cover.

## Validation weaknesses and the next acceptance gate

Existing test counts are useful but do not establish the missing properties. Two existing core assertions are tautologies: `radio/tests/modemcore_tests.cc:299,362` use `... || true`, so they cannot reject the unwanted acknowledgement/cache states. The normal sensor feeder delivers incrementally and does not exercise an initial multi-read backlog. Assistance tests check explicit modem errors but did not reject malformed/wrong-part confirmations. A fake callback success is not evidence of Android storage durability.

For each affected subsystem, require the following matrix, tied to the exact source and installed build identity:

| Dimension | Required cases | What must be asserted |
|---|---|---|
| Lifecycle | Before ready, start, active, stop, restart, framework reconnect | No orphan operation; applied state agrees with owned resources |
| Ordering | Response before/after indication; delayed callback after teardown/reuse | No old generation changes new state |
| Failure | Explicit rejection, timeout/unknown outcome, malformed response, lost endpoint | No invented success; deterministic retry/rollback/reconciliation |
| Multiplexing | Two SIMs, two IP families, calibrated/uncalibrated sensors, multiple queued deliveries | Correct ownership; one client's event does not consume another's completion |
| Load/timing | Delayed consumers, full buffers, long assistance tasks | Control work remains responsive; counts/timestamps/completions remain valid |
| Phone integration | Installed HAL → framework → application, enforcing policy, real suspend/resume | Observable end-to-end behavior with the host assumptions checked |

Additional issues needing targeted evidence, **not counted as confirmed new findings**:

- Radio sleep handoff: SMS uses `UNSOLICITED_ACK_EXP`, while `responseAcknowledgement()` is a no-op and no radio wake-lock ownership was found. Compare against the local libril acquire/ack/timeout path and validate the kernel-to-framework handoff under actual suspend before claiming reliable background SMS/call delivery. A membership in the `wakelock` Unix group alone is not an acquired wake lock.
- Actual kernel/DSP recovery for audio, sensors, Wi-Fi and data, including stale descriptors and module re-probe. Host mocks do not establish electrical or driver recovery.
- E-ink rendering of nontrivial composition/rotation and the final physical key stream under input loss. F20 remains the known input recovery finding; framebuffer capture success alone is insufficient.
- Final package/manifest/permissions/source-to-image consistency. A change in this workspace, a platform-tree compile, a staged module and a flashed ROM are separate artifacts. The in-progress tree must not inherit a PASS from a different build.

Do not close VoLTE or cameras based on this review. The latest experiment reports and the first inventory's per-camera/Android-provider requirements remain the acceptance authority. Likewise preserve the existing thermal/DVFS, HFP/SCO, USB role, FBE, hardware-security, suspend and watchdog gaps instead of conflating driver presence with completion.

## Reproductions and provenance

Artifacts: [review directory](../research/hardware-review-deep-20260928/README.md), [runner](../research/hardware-review-deep-20260928/reproduce.py), [main results](../research/hardware-review-deep-20260928/results.txt), [cache results](../research/hardware-review-deep-20260928/run-cache/results.txt), [voice contract results](../research/hardware-review-deep-20260928/run-voice/results.txt).

| Harness | Finding groups | Test boundary |
|---|---|---|
| `voice_contract_review.cc` | F23/F24 | Exact extracted method bodies; fake Binder-facing types, synchronous executor and modem recorder |
| `sms_review.cc` | F25–F27 | Actual ModemCore/QMI source, fake modem/listener, controlled dispatcher/worker interleaving |
| `data_generation_review.cc` | F28 | Actual data manager; deferred loss callback, no kernel interface creation |
| `cache_review.cc` | F29 | Actual ModemCore, delayed query reply and newer fake modem indication |
| `sensors_review.c` | F30–F32 | Actual processing, queue and estimator; synthetic scans/timestamps, no IIO device |
| `gnss_review.cc` | F33/F34 | Actual engine/parser with fake LOC service and invalid/delayed confirmations |

The initial four-harness run and the cache/voice additions each record source SHA-256 values and `changed-during-run.json`; all three reported **zero source-input changes during their run**. At the pre-report freshness check, the main run's hashed inputs were still unchanged. These manifests cover copied implementation sources, not a complete ROM. The full current runner executes all six; optional `cache` or `voice` arguments run only those cases into their own result directory.

The final [report-close freshness check](../research/hardware-review-deep-20260928/report-close-freshness.json) also found no changes against any of the three 80-file manifests.

All expected defect assertions reproduced. Sanitizers reported no errors in these runs. That does not prove absence of thread races, leaks or hardware faults. Core fixtures use process-lifetime threads and `_Exit`, so they do not validate teardown/leak behavior. The assertions intentionally recognize broken behavior; after fixes, replace them with corrected invariants rather than preserving their current expected output.
