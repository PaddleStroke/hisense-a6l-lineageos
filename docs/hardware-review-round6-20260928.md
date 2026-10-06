# A6L hardware integration review — sixth pass

28 September 2026. **This review round is complete. Four new findings, F51–F54, and two incomplete earlier fixes, F4/F7.** No security or approval check blocked the work.

This pass examined failure recovery and GNSS control semantics in the latest available integrations. Five offline harnesses reproduced the cases below against preserved source. These are source defects under specified conditions, not six newly observed failures on the phone. Production and the phone were not modified. Claude can integrate the proposed fixes independently.

Earlier findings remain in the [initial review](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-20260928.md), [second pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-pass2-20260928.md), [deep review](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-deep-20260928.md), [fourth pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round4-20260928.md) and [fifth pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round5-20260928.md). F4 and F7 below are deliberately not assigned new numbers.

| Finding | Priority | Reproduced behavior |
|---|---|---|
| F51 | P2 | Proximity activation succeeds after interrupt enable fails; restoring writability does not restore interrupts |
| F52 | P2 | Rejected GNSS STOP clears the engine's session state; neither another stop request nor shutdown sends a cleanup STOP |
| F53 | P2 | Android single-fix and periodic requests produce identical periodic QMI START commands |
| F54 | P2 | Selective GNSS aiding-data deletion acknowledges success without calling the engine |
| F4, incomplete | P2 | Proximity recovery expects error/hangup poll flags that the local IIO kernel implementation does not emit after unregister |
| F7, incomplete | P2 | Partial mixer failure outside the selected verification control is accepted and cached indefinitely |

P2 denotes correctness defects requiring the stated failure conditions or API requests. It does not establish their frequency in normal use.

## F51 — Failed proximity interrupt enable leaves a successful but inactive subscription

**Sources:** [interrupt writes](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/hals/sensors/stk3338/sensors_a6l.c:151), [activation](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/hals/sensors/stk3338/sensors_a6l.c:225), [startup disable](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/hals/sensors/stk3338/sensors_a6l.c:473), and [kernel event configuration](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/kernel/stk3338/stk3338_a6l.c:385).

Opening the HAL disables the proximity interrupt because there is initially no client. Activation marks `prox_on`, calls the void `set_ps_int()` helper, and returns success even when the write fails. Its comment assumes failure leaves the driver's default enabled state, but a successful startup disable has already invalidated that assumption. The kernel updates its remembered interrupt state only after a successful register write.

An open event FD does not enable the sensor interrupt. Once that FD is open, the new event-FD retry path has nothing to retry. The initial raw proximity event can still be delivered, making the subscription appear healthy while subsequent near/far interrupts are unavailable. The suspend wake decision also depends on the kernel's interrupt-enable state.

**Reproduction:** start with fake event-enable attributes set to one. The real HAL opens and sets them to zero. Inject failure opening the rising-enable attribute, activate proximity, then restore the attribute. Activation returns zero with a valid event FD. The initial proximity event arrives; after 12 real poll calls and about 2.5 seconds, both enable attributes remain zero. The fake FIFO does not model a physical sensor: missing physical interrupts/wake follow from the disabled register path, not from measured phone behavior.

**Proposed fix:** return and propagate interrupt-configuration errors. Maintain requested versus applied interrupt state. Either fail activation cleanly, or retain the request with a bounded retry deadline that runs independently of event-FD readiness. Handle disable failures as well, and cancel pending enable retries when the client disables. Both exposed directions share the same hardware interrupt bit; account for partial success without assuming the second write represents an independent channel.

**Acceptance:** successful initial disable, one failed enable, recovery while activation remains requested, then genuine near/far events and suspend wake. Also test enable followed immediately by disable, failed disable, and service restart. A valid event FD alone must not satisfy readiness.

## F52 — GNSS records a rejected STOP as completed

**Sources:** [stopSession](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_client.cpp:349), [reconcile](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_client.cpp:296), and [worker cleanup](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_client.cpp:646).

`stopSession()` discards the QMI result and unconditionally clears `session_`. Further reconciliation sees no active session. Worker shutdown also skips its conditional STOP. This is different from F21's failed configuration/START retries and F34's delayed STOP dispatch: STOP is sent promptly here, but a rejection is treated as successful application.

**Reproduction:** run the complete production engine/client with its fake modem. Start successfully; reject STOP with QMI error 3. The fake receiver remains running while `sessionRunning()` becomes false. Remove the injected error, request stop again, and wait 1.2 seconds with retry settings of 50–100 ms. There is still only one STOP. `end()` sends no cleanup STOP either. The fake receiver intentionally changes state only on accepted commands. Actual modem behavior and current draw were not measured. Android location delivery is already gated by `mActive`; this finding does not claim callbacks necessarily continue after stop.

**Related state-consistency check:** injecting an indication that the existing parser/listener interprets as session ended increments the listener's ended count, but leaves `session_` true and sends no replacement START while activity remains desired. Treat this as a recovery case to cover when correcting the state machine; the confirmed STOP defect does not depend on firmware emitting that indication.

**Proposed fix:** preserve an active/unknown applied state after a rejected or timed-out STOP and reconcile it with bounded backoff, including shutdown cleanup. Keep requested inactivity separate from acknowledged modem state. Treat documented already-stopped responses appropriately; do not retry permanently invalid requests blindly. Process service/session indications through the worker, validate their session/service generation, and restart only when a current request still requires it.

**Acceptance:** rejected STOP followed by recovery; timeout with uncertain application; repeated stop/close; stop during assistance; service loss; start-stop-start while a retry is pending. Verify eventual applied inactivity without resurrecting an old session, and prompt cancellation of location delivery independently of modem cleanup.

## F53 — A single-fix request starts a periodic session

**Sources:** [setPositionMode](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:245), [START encoding](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_v02.cpp:76), and [fix callback](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:295).

The HAL logs recurrence but only forwards the interval. `makeStart()` always encodes periodic recurrence; the fix callback does not implement a compensating single-fix stop. Android distinguishes single from periodic requests, and QMI has distinct recurrence values: periodic is 1 and single is 2. See the [Android interface](https://android.googlesource.com/platform/hardware/interfaces/+/8e2b9272b0939f5609fe078f24405732a582339a/gnss/aidl/android/hardware/gnss/IGnss.aidl) and [libqmi enum definitions](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/src/libqmi-glib/qmi-enums-loc.h).

**Reproduction:** execute the exact extracted HAL method with a recording engine, then its real START encoder. Otherwise identical periodic and single requests yield byte-identical commands, with recurrence 1. This is a method/encoding test, not a full Android Binder integration test. Framework cleanup might limit the practical duration, but does not make the requested HAL semantics correct.

**Proposed fix:** retain recurrence in the requested session configuration and propagate it to QMI, with coherent session completion and restart handling. If native single-fix operation is unsuitable for this firmware, explicitly implement one-result completion and reliable modem stop. Validate supported options instead of silently accepting a configuration the backend cannot apply.

**Acceptance:** periodic delivers repeated fixes; single delivers the intended single result and completes its session; cancellation before a fix works; retry/reconnect does not convert single into an endless periodic request. Test configuration changes across stop/start boundaries.

The same method discards `mode` and `lowPowerMode`; these deserve contract coverage, but are **not additional numbered findings here**. Low-power capability is not advertised, so this review does not infer that ordinary framework requests exercise it or claim a measured energy regression.

## F54 — Selective assistance deletion is acknowledged but ignored

**Source:** [deleteAidingData](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:237).

Only a flag value exactly equal to `ALL` calls the engine. Requests to discard ephemeris, time, or another subset return success with no deletion operation. Android defines these flags as information the next start must not reuse. See the [aiding-data contract](https://android.googlesource.com/platform/hardware/interfaces/+/8e2b9272b0939f5609fe078f24405732a582339a/gnss/aidl/android/hardware/gnss/IGnss.aidl). This can invalidate targeted stale-assistance recovery or diagnostic tests even if ordinary cold-start tests pass.

**Reproduction:** the exact extracted method accepts EPHEMERIS and TIME while the recording engine observes zero deletion calls. ALL increments the count, providing a positive control for the fixture. No claim is made that the modem currently contains corrupt aiding data.

**Proposed fix:** implement and validate selective QMI deletion with completion/error handling. If a conservative full deletion is the supported fallback, apply it deliberately for supported nonempty subsets and document the extra acquisition cost. Otherwise return an explicit error rather than success. Preserve requested deletion across temporary service absence so a later start cannot silently skip it.

**Acceptance:** individual flags, combinations, ALL, zero/invalid inputs, QMI rejection and service absence. Inspect the outgoing deletion request and confirm the following start cannot overtake pending deletion.

## F4 remains incomplete — IIO unregister does not produce the expected poll error

**Sources:** [HAL polling and FD recovery](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/hals/sensors/stk3338/sensors_a6l.c:356), plus the archived local kernel `drivers/iio/industrialio-event.c`, `iio_event_poll()` at line 99.

The HAL now reopens the proximity event FD on `POLLERR`, `POLLHUP`, `POLLNVAL`, or a read error after readable data. However, the reviewed kernel's event poll callback returns a zero mask when the IIO device's `info` becomes NULL after unregister. It does not report hangup, error or readable status. Its read path can report `ENODEV`, but the HAL does not proactively read a non-readable FD. With only proximity active and its initial event delivered, the HAL waits with timeout -1.

**Reproduction:** the complete HAL's idle wait was intercepted and recorded as indefinite. Separately, the exact local kernel poll function was compiled with minimal structure stubs: a live device with queued data is readable; clearing `info` returns zero, even with queued data. Together these establish why the expected error branch cannot recover this state. This is contract-level evidence, not an actual kernel unbind test. A FIFO hangup test would not establish the real IIO behavior.

**Proposed fix:** add a bounded liveness check appropriate to actual IIO semantics, using nonblocking event reads where needed to expose `ENODEV`, or change the relevant kernel poll contract deliberately and test both sides. On loss, clear the old sysfs identity, rediscover by device name, reopen the event FD and reapply the requested interrupt state. Merely reopening the cached `iio:deviceN` path misses recovery if enumeration changes.

**Acceptance:** real driver unregister/re-register with proximity as the only active sensor, both empty and nonempty event queues, same and changed IIO indices, and no light/motion client or user action to wake the loop. Recovery must return a fresh current state without spinning.

## F7 remains incomplete — One successful mixer control does not prove the whole route

**Sources:** [verification table](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/audio/route/a6l_audio_route.c:136), [cached route](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/audio/route/a6l_audio_route.c:233), and [headset microphone path](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/audio/mixer_paths_a6l.xml:116).

The new route verification checks `Digital DEC1 MUX=ADC2` for the headset microphone, but the same path also requires `ADC2 MUX=INP2`. If that latter write fails while the checked control succeeds, `apply()` returns success. The route is cached and subsequent unchanged requests skip application. Other paths likewise use a single verification control rather than proving the required signal path.

**Reproduction:** extend a temporary copy of the existing fake mixer fixture with the actual XML's ADC2 MUX control. Run the unchanged production daemon; discard only that control's write. The daemon reports route success with ADC2 MUX still ZERO. Restore healthy writes and execute 100 unchanged route steps: there is still only one application and the analog mux stays ZERO. This proves false acceptance/retry suppression; it is not a recording made through the physical codec.

**Proposed fix:** propagate individual control write failures and commit applied mixer state only after successful writes, or verify the complete required control set and keep the route pending on any mismatch. Ensure the underlying routing library's cached values do not suppress a repair after a failed hardware write; invalidate/reinitialize that cache where necessary. Include both input and output paths and shared controls in the consistency check.

**Acceptance:** independently lose each required headset/microphone/speaker control write, then restore it without changing jack or call state. Require failure reporting followed by successful automatic repair. Repeat after card/DSP reset, including a reset that preserves card enumeration.

## Evidence, exclusions and remaining work

The [evidence README](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round6-20260928/README.md) describes each harness boundary. [Results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round6-20260928/results.json), individual logs, [source hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round6-20260928/source-hashes.json) and [source archive](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round6-20260928/reviewed-source.zip) are preserved. All five harnesses exited zero while reproducing the defects. UBSan reported no runtime violation in these runs. The captured production files did not change during the final run.

One suspected suspend defect was rejected: `stk3310_set_state(STANDBY)` explicitly preserves the enabled flags, so ordinary standby does not erase the requested PS/ALS state. It is not a finding. Recovery of an already-open voice PCM after a DSP failure still needs a real restart test; keeping a nonnegative FD is not proof of recovery, but this pass did not establish a new physical failure there.

The next high-value validation is **the assembled ROM under lifecycle transitions**, using the same fixed build throughout: boot with delayed devices, disable during retry, suspend/resume with active clients, restart a service or DSP after successful operation, and change device enumeration. Keep each previously numbered issue open until its acceptance case passes on the integrated build. Offline reproductions complement that work; they cannot establish VoLTE calls, camera frames, physical wake behavior, current draw, or daily-driver completeness.
