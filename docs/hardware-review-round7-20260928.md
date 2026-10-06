# A6L hardware integration review — seventh pass

28 September 2026. **This round is complete: four new findings, F55–F58.** The most consequential is F57: rejecting a call after the ringing/waiting state disappears can release an unrelated held call. No security or approval check blocked this review.

Two offline harnesses reproduced the cases against preserved production source. These are defects under the stated requests or failure conditions, not four newly observed phone failures. Production source and the phone were not modified. Claude may have integrated changes after this snapshot; use the archived source and hashes when comparing fixes.

| Finding | Priority | Confirmed behavior |
|---|---|---|
| F55 | P2 | TTY settings report success and read back the request without configuring the modem or audio path |
| F56 | P2 | A rejected STOP of continuous DTMF is ignored; the finite-tone request still reports success |
| F57 | P2 | Reject with only a held call sends an untargeted release-held-or-waiting command |
| F58 | P2 | Last call failure cause is permanently initialized to NORMAL_CLEARING |

P2 denotes correctness defects requiring the stated circumstances; frequency on the phone has not been established. Earlier findings are linked from the [sixth-pass report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round6-20260928.md). These four are distinct from the previous mute, voice-volume, routing, emergency-call and CLIR findings.

## F55 — TTY configuration acknowledges an operation it does not perform

**Sources:** [setTtyMode and getTtyMode](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioMessagingVoice.cpp:469).

The setter assigns `mTty` and reports success. The getter returns that member. Neither method configures the modem or audio path. Android defines this operation as setting the TTY mode, so retaining the preference alone does not satisfy the request. See the [Android voice contract](https://android.googlesource.com/platform/hardware/interfaces/+/f665b8fa862e55c76ada5eee8bdc20eac9aa0734/radio/aidl/android/hardware/radio/voice/IRadioVoice.aidl).

**Reproduction:** execute the exact production method bodies with recording Binder responses and the production QMI client connected to a fake modem. FULL, HCO and VCO each return success and are echoed by the getter; the modem receives zero requests. Static inspection confirms that the methods invoke no other configuration endpoint. This does not test an actual TTY accessory.

**Proposed fix:** implement the supported modem configuration and any required audio/DSP routing, and report success only when applied. Until available, return `REQUEST_NOT_SUPPORTED` for unsupported non-OFF modes and keep readback honest. Use an explicit enum conversion: Android uses OFF=0, FULL=1, HCO=2, VCO=3, whereas QMI uses FULL=0, VCO=1, HCO=2, OFF=3. A raw cast would introduce another bug. The Android enum is archived with the reproduction; compare [libqmi's TTY definitions](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/src/libqmi-glib/qmi-enums-voice.h).

**Acceptance:** all four modes, configuration rejection/timeout, modem restart, and readback of applied state. Follow with accessory tests for each supported mode and ordinary voice regression. TTY and RTT require separate acceptance.

## F56 — Finite DTMF reports success even when its continuous tone cannot be stopped

**Sources:** [sendDtmf](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioMessagingVoice.cpp:393), [QMI continuous-tone helpers](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/services.cc:1028).

The implementation starts a continuous tone, waits 150 ms, then sends STOP. It discards the STOP result and bases the Android response solely on START. Consequently, an accepted START followed by a rejected STOP reports success, with no cleanup retry. Android's single-digit `sendDtmf` operation differs from its explicitly continuous start/stop operations. See the [Android voice contract](https://android.googlesource.com/platform/hardware/interfaces/+/f665b8fa862e55c76ada5eee8bdc20eac9aa0734/radio/aidl/android/hardware/radio/voice/IRadioVoice.aidl).

**Reproduction:** accept START and reject STOP with QMI error 3. The HAL reports success while the fake modem retains its continuous-tone state. Remove the error and wait 300 ms: there is no additional STOP. An explicit `stopDtmf` positive control then stops it. The wrong success response and absent cleanup are confirmed; continued audible tone on real firmware is a possible consequence, not a phone measurement.

**Proposed fix:** account for both command results and retain cleanup responsibility after a failed or uncertain STOP. Use bounded retries tied to the current call and service generation; never stop a new tone on a recycled call ID. Consider the existing burst-DTMF helper if its behavior is verified on this firmware. Validate the Android single-digit input contract as part of this path.

**Acceptance:** rejected STOP followed by recovery, timeout with uncertain application, call termination/replacement during the 150 ms delay, service loss, and overlap with an explicitly started tone. A cleanup failure must not be reported as a successfully completed finite tone.

## F57 — Reject can release a held call when no incoming or waiting call remains

**Sources:** [rejectCall](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioMessagingVoice.cpp:380), [supplementary-service constant](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/include/a6lqmi/services.h:450), [manageCalls encoder](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/qmi/src/services.cc:1023).

The method targets an INCOMING call when one is found. Otherwise it unconditionally sends `kSupsReleaseHeldOrWaiting`, without verifying that a WAITING call exists and without specifying a call ID. Android's rejection operation applies to a ringing or waiting call; it is not a request to release an unrelated held call. See the [Android voice contract](https://android.googlesource.com/platform/hardware/interfaces/+/f665b8fa862e55c76ada5eee8bdc20eac9aa0734/radio/aidl/android/hardware/radio/voice/IRadioVoice.aidl).

**Reproduction:** queue rejection, then arrange for the modem's current call list to contain only held call 7 when the handler runs. The exact production method sends Manage Calls with supplementary-service value 1 and no call-ID TLV, then reports success. The fake modem models the command's release-held behavior and marks that call released. A positive control with an incoming call uses targeted END_CALL instead.

The harness controls executor dispatch to make the ordering deterministic; it is not a stress test of the production executor. The confirmed wire request is sufficient to expose the missing state guard. A real-world trigger is a waiting caller hanging up before the queued rejection executes. Physical call release was not tested.

**Proposed fix:** return `INVALID_STATE` without modifying calls when neither an incoming nor a waiting call exists. Prefer a supported rejection operation targeting the identified call. A WAITING check alone does not eliminate the interval between the query and command, so validate target selection and call/service generation. Do not turn a failed call-list query into an untargeted release fallback.

**Acceptance:** incoming, waiting alongside held, only held, only active, empty list, failed query, and a waiting call disappearing while rejection is queued. No unrelated call may be ended.

## F58 — Every last-call failure is reported as normal clearing

**Sources:** [cause initialization](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.h:230), [getter](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/ModemCore.h:138), [Android response](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/radio/hal/RadioMessagingVoice.cpp:426).

`mLastCallFailCause` is initialized to 16 (`NORMAL_CLEARING`) and never assigned again. Its only other use is the getter. The Android response casts this constant into `LastCallFailCause`, returns success, and leaves the vendor cause empty. The interface requests the cause for the most recently terminated call. See the [Android voice contract](https://android.googlesource.com/platform/hardware/interfaces/+/f665b8fa862e55c76ada5eee8bdc20eac9aa0734/radio/aidl/android/hardware/radio/voice/IRadioVoice.aidl).

**Reproduction:** run the full production `ModemCore` and QMI implementation with a fake modem, introduce an active voice call, then remove only the VOICE service. Wait for the service-loss handler to clear the call list: the failure getter still returns normal clearing. The preserved member-use inventory independently demonstrates the constant behavior. This test does not inject a real network busy/rejection cause or establish its QMI encoding.

**Proposed fix:** decode supported modem termination reasons, map them explicitly to Android causes, and retain the last terminated call's reason after removing it from the live call list. Scope and synchronize this state per subscription and call/service generation. Represent transport/service loss with an appropriate unavailable or unspecified failure instead of pretending it was a normal hangup. Do not cast QMI cause values directly into Android enums.

**Acceptance:** normal remote hangup, busy, rejection, network failure, modem/service loss, dual-SIM isolation, and reused call IDs. Query after removal and verify the cause belongs to the most recently terminated call. This primarily corrects misleading diagnostics and disconnect information; framework retry/UI effects need separate validation.

## Evidence and review boundaries

The [reproduction README](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round7-20260928/README.md) explains execution and fixture boundaries. [Results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round7-20260928/results.json), [source hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round7-20260928/source-hashes.json), and the [preserved source archive](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round7-20260928/reviewed-source.zip) accompany it. Both harnesses passed their defect-reproduction assertions with AddressSanitizer and UndefinedBehaviorSanitizer enabled. No source files changed during the run. Passing means these defects reproduced, not that the implementation passed acceptance.

The wider scan also examined charging/power, USB packaging, storage/recovery declarations, Wi-Fi and recent peripheral fixes. It did not establish additional distinct defects there. In particular, the unfinished USB gadget HAL is excluded by the pipeline, and MTP remains an existing integration gap; counting its stub methods as new shipped defects would be misleading. This scan is not hardware acceptance or proof that those subsystems are bug-free.
