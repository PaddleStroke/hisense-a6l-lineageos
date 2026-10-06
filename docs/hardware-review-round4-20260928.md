# A6L hardware integration review — fourth pass

28 September 2026. Continues [F1–F13 and H1–H64](hardware-review-20260928.md), [F14–F22](hardware-review-pass2-20260928.md), and [F23–F34](hardware-review-deep-20260928.md). Read alongside the [integration ledger](rom-integration-ledger.md) and [VoLTE/camera review](volte-camera-review-20260928.md).

**Seven additional actionable finding groups, F35–F41.** This pass concentrated on e-ink output and recovery, the newly integrated audio/Wi-Fi controls, and modem/Bluetooth lifecycle coupling. Six groups have offline fault-injection reproductions; call-volume handling has a complete source trace. These are software defects under the specified conditions, not seven new failures observed on the phone.

The most useful result is the interaction between F35 and F36: a failed physical update can be reported as successful by the e-ink server, and even an honestly reported failure is treated as completion by the mirror. Fixing only one end will not provide recovery. Similar gaps remain between requested and applied state in call volume and Wi-Fi readiness.

Production source was not changed. No phone access, calls, RF changes, firmware/partition writes, ROM build or installation took place. Only this report and isolated review artifacts were added. The reviewed source includes the current F5/F6/F7/F8 integration work; this is not acceptance of those fixes or proof they reached an installed image. Sources remained unchanged during the final reproduction run; their SHA-256 hashes are recorded in the evidence directory.

## Findings at a glance

All seven are P2 correctness defects: fix in the affected integration work and exercise the stated failure conditions before declaring that behavior supported. This severity does not mean every issue occurs during ordinary use.

| ID | Affected behavior | Demonstrated defect | Evidence |
|---|---|---|---|
| F35 | Rear panel power/refresh | Failed rail-on and rail-off operations still produce a successful drive result | Exact `drive()` body with fake rail/DRM calls |
| F36 | Rear page recovery | Failed/unacknowledged frames are recorded as displayed and never retried for a static page | Full mirror executable, real local socket, 100 captures per case |
| F37 | In-call volume | Android volume updates only a cached configuration, with no voice-path gain operation | Framework → selected HAL → default Telephony source trace |
| F38 | Wi-Fi startup | Readiness timeout becomes success while MAC initialization is still unconfirmed | Exact readiness function with fake properties/netdev |
| F39 | Bluetooth after modem failure | Early failure omits public-address initialization; later failure omits even module loading | Real startup script against fake root/init/module operations |
| F40 | Explicit modem stop | Failed stop write still publishes `stopped` and stops all three supporting daemons | Real stop script with a failing fake state path |
| F41 | Rear screen fidelity | Capture ignores DRM plane rotation, opacity and blend mode | Exact capture function with controlled plane/pixel inputs |

Numbering deliberately groups related symptoms rather than counting each test case as a separate finding. F38 concerns the new readiness protocol, not the original missing Wi-Fi HAL. F39/F40 concern independent Bluetooth setup and stopping, not F8's already reported modem-start prerequisites. F41 resolves a previously unconfirmed composition concern with concrete host examples; physical Android composition remains to be checked.

Paths below are relative to `device/hisense/a6l/` unless stated otherwise. WSL platform paths refer to `/home/a6l/android/a6l-lineage24`.

## F35 — e-ink rail failures are discarded

**Source:** `eink/src/a6l_epdd.c:488–500`, especially lines 491 and 497; `run_update()` at 502; `exec_cmd()`'s final success/error reply. The selected service is `eink/a6l_eink.rc:23`.

`drive()` logs a failed `power(1)` and continues sending waveform frames. It also ignores the return from `power(0)`. Its result reflects page-flip success only. Therefore a missing/unwritable power attribute or a driver error can produce `OK shown` despite the panel never receiving its rails, or despite failure to switch them off afterward. The latter can leave power consumption higher than intended; this review does not claim physical panel damage.

**Reproduction:** the unchanged `drive()` body received `-1` from rail-on, then separately from rail-off. Both returned **0** and logged **ok**. The rail-on failure still issued all three configured fake flips. This is a control-flow test, not a measurement of voltage or waveform quality.

**Proposed fix:** require successful rail enable before waveform scanout. Route every exit through cleanup and propagate rail-off errors. Keep the panel's applied state unknown after a failed or partial update; do not quietly advance the next differential update from an assumed successful picture. Since `run_update()` advances the waveform library before hardware drive, recovery needs an explicit, validated full-refresh/reinitialization strategy rather than blind continuation from library state. That last strategy needs testing with the real library; it was not simulated here.

**Acceptance:** inject rail-on/rail-off failures independently and a page-flip failure after partial delivery. Verify an error reply, correct cleanup, and a successful redraw after recovery. On the phone, confirm rails turn off after both success and failure and that the next picture is correct.

## F36 — mirror commits a picture before acknowledgement and ignores error replies

**Source:** `eink/src/a6l_eink_mirror.c:374–383, 605–616`; `pump()` at 369 and disconnect handling at 380.

The main loop calls `pol_sent()`, copies the current tiles into `shown_t`, and sets `have_shown=1` before the frame is acknowledged. `on_reply()` accepts every reply, including `ERR`, as completion. Disconnect handling clears the socket/queue but does not invalidate the displayed-image assumption.

Consequently, if a quality frame fails, the next identical source frame compares equal to `shown_t`. No change means no resend. Reconnecting to a restarted server also does not force synchronization. This can leave a blank or old rear page indefinitely until another visible source change or explicit clear.

**Reproduction:** compiled the complete mirror with its file-capture backend and real Unix-socket transport. A fake server accepted `clear` and then either returned `ERR update failed` for the first frame or closed without acknowledging it. In **each** case the mirror captured the constant source **100 times**, but sent only:

```text
clear
frame 720 1440 quality
```

There was no frame retry, including after reconnection in the disconnect case. Logs are retained separately.

**Proposed fix:** maintain pending and acknowledged frame/policy state separately. Commit the displayed picture only after a valid success response to the corresponding frame. Invalidate that baseline on errors, disconnect, timeout, or server restart; retry the current page with bounded backoff and the full-refresh recovery needed by F35. Treat clear + frame as a sequence whose frame can only establish displayed state after success.

**Acceptance:** constant-page tests for `ERR`, lost reply, disconnect before/after payload, server restart, and clear failure. Recovery must resend without requiring user scrolling. Also exercise stalled writes: `send_all()` is blocking and the reply deadline starts after sending, so transport deadlines must cover writes as well. The stalled-write concern was inspected but is not counted as another reproduced finding.

## F37 — in-call volume has no connection to the voice audio path

**Source chain:** platform `frameworks/av/media/libaudiohal/impl/DeviceHalAidl.cpp:205–221` → `hardware/interfaces/audio/aidl/default/ModulePrimary.cpp:48–55` → `hardware/interfaces/audio/aidl/default/Telephony.cpp`, `setTelecomConfig()`. The new `audio/patches/0002-a6l-call-route-mute.patch:186–188` retains creation of that same default `Telephony` implementation.

`DeviceHalAidl::setVoiceVolume()` sends `TelecomConfig.voiceVolume`. The selected module creates the example `Telephony` object, whose setter validates and stores the value and returns it. It performs no DSP, mixer, socket or gain update. The port's new patch handles route and TX mute, but does not replace this volume implementation. The q6voiced command handler likewise has mute/status operations, not a voice-volume command.

The downlink runs through the DSP voice path, so adjusting multimedia PCM/software gain does not implement this missing operation. `audio/mixer_paths_a6l.xml:70–99` sets fixed headphone/earpiece voice levels and a speaker profile; it does not consume Android's requested voice volume.

**Evidence:** the three complete platform methods are saved in `audio-volume-trace.txt`, with source hashes. This is a static call-chain finding; no acoustic volume sweep or ordinary call was performed. The earlier checklist asked for volume validation, but this report identifies the missing implementation behind that item.

**Proposed fix:** give the port a real Telephony volume implementation and an acknowledged path to a suitable voice RX gain control. Choose calibrated mappings for each supported output; preserve the requested level across route changes and DSP/session recreation. Avoid inventing arbitrary codec gain values or assuming the media stream controls voice audio. Report failures instead of caching successful application.

**Acceptance:** trace Android's volume steps to actual applied controls, including failure and route-switch cases. During a consented ordinary call, measure or listen to a repeatable downlink signal on headset, speaker and a healthy unit's earpiece; levels must change monotonically without unexpected jumps or clipping. Use the framework's specified minimum-volume behavior rather than assuming zero must mean mute.

## F38 — Wi-Fi readiness gate fails open after its deadline

**Source:** `wifi/hal/a6l_wifi_hal.cpp:275–306`; publisher in `rom/bin/a6l-radio.sh:75, 129–140`.

The new HAL is intended to wait until the script has set the factory MAC while the interface is down. Yet after the timeout, any existing wireless netdev yields `WIFI_SUCCESS`, irrespective of the readiness property. The modem script may still be initializing or waiting before its MAC step; it can subsequently bring down an interface the HAL has already started.

The protocol also lacks a fresh startup generation: the script sets radio state to `starting`, but does not clear the previous WLAN `ready` value before initialization. A repeated start can therefore bypass even the timeout. Finally, MAC-command failure is logged but still followed by `ready`. These are related weaknesses in what this readiness signal actually guarantees.

**Reproduction:** with a present fake wireless netdev, the exact HAL function returned **success** after the deadline for readiness values **empty, starting, stopped and missing**. The test uses a one-second configured limit and an accelerated sleep stub; it executes the same timeout branch as the default 30-second configuration. The stale-generation and failed-MAC publisher cases are source evidence, not separate dynamic cases.

**Proposed fix:** keep timeout a failure when the current ROM's initialization handshake is required. Reset readiness before setup; publish success only when initialization is complete and MAC handling has an explicit outcome. Make any legacy compatibility fallback opt-in, distinguishable from successful initialization, and incapable of racing later link/MAC changes. Avoid changing a live WLAN interface during a repeated modem-start request.

**Acceptance:** delay MAC setup past the HAL deadline, fail the MAC write, begin a new startup with an old `ready` value, and repeat start while Wi-Fi is connected. Android must not receive readiness and then have its connection disrupted by the unfinished initialization sequence.

## F39 — Bluetooth independence breaks on modem failure paths

**Source:** `rom/bin/a6l-radio.sh:41–45, 127, 142–144`; `rom/v2/init.a6l.wifibt.rc` defines `vendor.a6l-bt-addr`; `hals/macs/a6l_macs.c`, `do_bt()`.

The script explicitly recognizes that UART Bluetooth is independent of the modem. Its early `fail()` path loads the BT modules, but immediately exits without starting the public-address service. This controller can initially be **UNCONFIGURED** until `a6l_macs` sets its address. Loading the modules alone is not complete Bluetooth initialization. The selected platform Bluetooth backend waits for a configured controller index; it does not replace the missing public-address operation.

There is another exit after the modem fails to reach `running` that bypasses `fail()` altogether, so that path does not even load BT modules. The complete module + address sequence exists only at the successful end of startup.

**Reproduction:** the real script against fake prerequisites produced:

```text
EARLY_FAIL state=failed:ipa-module bt_modules=2 addr_start=0
LATE_FAIL state=failed:mss-start bt_modules=0 addr_start=0
```

These results demonstrate omitted setup operations. They do not simulate HCI packets or establish pairing behavior on a controller that was already configured by an earlier successful startup.

**Proposed fix:** move Bluetooth module loading and public-address initialization into one idempotent service independent of successful modem/WLAN bring-up. Ensure the entire sequence executes on the intended startup paths and exposes readiness/errors to the Bluetooth HAL. Preserve the existing explicit RF policy; this recommendation is not authorization to turn on Bluetooth during review or to bypass that policy.

**Acceptance:** on a fresh unconfigured controller, fail IPA prerequisites, fail a modem prerequisite, and fail the final modem-running check. Bluetooth should still complete authorized initialization and become usable, with no premature Android HAL timeout. Repeated starts must not disrupt an already configured controller.

## F40 — failed modem stop is reported as stopped and loses supporting services

**Source:** `rom/bin/a6l-radio.sh:146–152`.

The `stop` case does not check the result of writing `stop` to remoteproc. It then publishes `vendor.a6l.radio.state=stopped`, publishes WLAN stopped, stops DIAG/tqftp/rmtfs, and exits 0. A rejected write can leave the modem running while those services disappear. The start path itself documents their necessity, including modem failure on DIAG starvation.

**Reproduction:** in a temporary fake root, the modem continued reporting `running` while its state-write path rejected redirection with `EISDIR`. The unmodified script nevertheless returned **0**, published **stopped**, and issued **three daemon stops**. The exact error is a test injection; the defect is ignoring a failed write, regardless of the real kernel error. No real remoteproc was stopped.

**Scope:** this is the script's explicit `stop` command. I did not find an active init caller for it and am not claiming Android airplane mode or ordinary shutdown currently traverses it.

**Proposed fix:** check the stop result and establish the remoteproc's final state before tearing down dependencies. On failure, preserve the needed services, publish a truthful failed/stopping state, and return failure. Define idempotent behavior for already-offline, absent and crashed remoteproc states. Do not label WLAN fully stopped merely because a modem operation was attempted.

**Acceptance:** successful stop, rejected write with modem still running, already offline, and a crash/recovery race. Verify both the reported state and which services remain alive. Stopping rmtfs while modem access remains possible must not be treated as successful shutdown.

## F41 — DRM capture omits transforms and blending used by the compositor

**Source:** `eink/src/a6l_eink_mirror.c:222–281`; platform `external/drm_hwcomposer/drm/DrmPlane.cpp:449–462`.

The mirror reconstructs the front picture from individual plane buffers, but its plane descriptor stores only position, crop, size and stacking order. It never reads `rotation`, `alpha`, or `pixel blend mode`, and assumes per-pixel premultiplied alpha. The selected compositor explicitly programs all three properties when supported.

The existing e-ink document's argument that the physical CRTC remains portrait does not remove this requirement: a plane's buffer can be transformed **before** scanout within that fixed portrait CRTC. The same applies to opacity during fades and overlay composition. The kernel documents these as part of plane composition. [Linux KMS blending and rotation documentation](https://cdn.kernel.org/doc/html/latest/gpu/drm-kms.html).

**Reproduction:** the exact capture function, with a 2×2 fake KMS plane and memory-backed pixels, returned success but produced:

| Plane input | Correct result | Actual capture |
|---|---|---|
| White plane, plane-wide alpha 0, black background | Black: 0 | White: 255 |
| Source `[0,64;128,255]`, rotation 180° | `[255,128;64,0]` | Unrotated `[0,64;128,255]` |
| Gray 200, pixel alpha 128, Coverage blending over black | Approximately 100 | 200 |

None of the three metadata properties was queried. These are conditional rendering defects, not proof that the current phone uses every illustrated plane configuration. A full GPU-composed, opaque, unrotated buffer can hide them.

**Proposed fix:** reproduce supported KMS transforms and blend equations correctly, including plane-wide opacity and reflection, or obtain a trusted already-composited frame through a suitable supported interface. Explicitly reject unsupported states rather than returning a plausible wrong picture. If choosing forced client composition as a temporary workaround, validate that it actually produces the assumed single linear buffer and measure its power/performance cost.

**Acceptance:** compare front display/reference composition with captured rear input for 0°/90°/180°/270° transformations, reflections, plane alpha, supported blend modes, multiple overlapping planes and clipping. Check the physical phone's actual plane properties while exercising rotation and animations; take into account the existing unsupported-format/modifier limitations.

## Reproduction artifacts and limits

Artifacts: [research/hardware-review-round4-20260928](../research/hardware-review-round4-20260928/). The runner is [run_review.py](../research/hardware-review-round4-20260928/run_review.py); results are [results.json](../research/hardware-review-round4-20260928/results.json), with [source hashes](../research/hardware-review-round4-20260928/source-provenance.json).

```text
wsl --exec python3 /mnt/c/Users/Pierre/Desktop/A6L/research/hardware-review-round4-20260928/run_review.py
REVIEW4_REPRODUCTIONS_PASS
```

The C/C++ reproductions and full mirror binary ran with AddressSanitizer and UndefinedBehaviorSanitizer; they completed without sanitizer diagnostics. Assertions establish the **current defects**, so this PASS is not a firmware acceptance result. Hardware APIs are mocked, radio operations are confined to a fake root, and platform traces are read-only. The runner snapshots inputs before use and checks their hashes again afterward. It uses temporary Linux build paths so it does not alter Claude's source or build outputs.

## Checklist changes and closure criteria

Carry these into the existing hardware checklist without marking older findings fixed:

| Existing area | Additional completion requirement |
|---|---|
| H4 rear panel and mirroring | F35/F36/F41: truthful physical-update result, failed/static-page recovery, composition fidelity |
| H15 call audio | F37: actual voice RX gain across supported routes, separately from mute and routing |
| H27–H29 Wi-Fi/identity/hotspot | F38: readiness reflects completed initialization and cannot race later MAC/link writes |
| H30 Bluetooth/BLE | F39: complete controller/address initialization despite independent modem failures |
| H16 modem lifecycle / shutdown | F40: failed stop retains truthful state and required dependencies |

The earlier camera ISP/provider, VoLTE, HFP/SCO, thermal/DVFS, USB/OTG, charger-mode, FBE/security, storage and final-image validation gaps remain. This pass does not certify those areas merely because it found no additional reproduced defect there. Input stream loss remains F20; mute ownership/getter consistency and incomplete mixer-write verification remain follow-up checks under F6/F7 rather than new numbers here.

For integration, require three kinds of evidence before closing an item: the failure regression test, proof that the selected final image contains the fix, and the relevant attended hardware check. The repeated finding counts reflect missing failure and integration tests; they cannot be used to estimate how many defects remain.
