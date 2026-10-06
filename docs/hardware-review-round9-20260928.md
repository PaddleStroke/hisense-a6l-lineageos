# A6L hardware integration review — ninth pass

28 September 2026. **Complete: two new findings, F62–F63.** This pass moved from radio protocol handling to audio device selection and frontlight recovery. Both cases reproduced offline; production source and the phone were not modified.

| Finding | Priority | Reproduced behavior |
|---|---|---|
| F62 | P2 | Normal/VoIP routing ignores requested speaker and built-in microphone when a wired headset is present |
| F63 | P2 | Frontlight backend recreation leaves the light off while the daemon caches the requested brightness as applied |

This reviews the current Windows integration sources and the selected local platform implementation. It does not establish which patches are present in a previously built image. Claude's later changes must be compared with the preserved snapshot. Previous findings are linked from the [eighth-pass report](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round8-20260928.md). No approval or security check interrupted this round.

## F62 — Android's device selection is not applied outside modem calls

**Sources:** [routing decision](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/audio/route/a6l_audio_route.c:94), [call-only publication gate](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/audio/patches/0002-a6l-call-route-mute.patch:115), [audio policy](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/audio/audio_policy_configuration.xml:25), and [integration of audio patches](C:/Users/Pierre/Desktop/A6L/tools/rom-v2-pipeline.sh:57).

The new HAL publisher examines and exports selected output devices only in `AudioMode::IN_CALL`. In NORMAL and IN_COMMUNICATION it exports no requested route. Independently, the routing daemon's `!voice` branch chooses output solely from headphone presence and input solely from microphone-jack presence. Even if device names were supplied, that branch would ignore them. Its `voice` flag comes from modem-call activity, not VoIP audio mode.

The local `StreamPrimary` implementation selects the common ALSA frontend for these devices; it does not implement their codec routing. The capture-PCM patch changes the frontend number, not microphone selection. The configuration advertises speaker and built-in-mic routes, so an accepted framework selection can disagree with the physical path chosen by the daemon. Android's [audio-policy documentation](https://source.android.com/docs/core/audio/implement-policy) explains the role of explicit device/stream connections; merely declaring them does not implement the underlying routing.

**Reproduction:** compile the exact publisher and device-name helpers extracted from the integration patch, plus the exact daemon `decide()` function. Supply a configuration containing output-mix → speaker and built-in-mic → input-mix patches. In both NORMAL and IN_COMMUNICATION, the publisher produces no route. With headphone and microphone jack presence set, the daemon chooses `headphones/headset-mic`, even when supplied `speaker/main` names. An IN_CALL positive control publishes speaker and selects the expected voice-speaker/main-mic paths.

This is a method/configuration test with recording Android/property fixtures, not a complete Binder/audio-policy run or an acoustic test. It establishes behavior when the stated framework patches have been selected; it does not assert that every app preference is accepted by Android or that every alarm uses this route combination.

**Impact:** VoIP speakerphone selection with a headset inserted, or a recording request routed by Android to the built-in microphone, can continue using the headset. This is separate from F7's modem-call routing and failed mixer-write recovery, which now have their own implementation. The remaining gap is ordinary PCM playback/capture routing and IN_COMMUNICATION mode.

**Proposed fix:** propagate active media/communication output and input devices to the component that owns the codec paths. Apply those choices independently of modem-call activity, with explicit priority when telephony and media coexist. Retain jack detection as availability information and fallback, rather than overriding an accepted route. Keep requested and applied state separate and report unsupported combinations honestly. Reuse the existing complete mixer verification and recovery machinery.

**Acceptance:** with a 4-pole headset connected, select headset versus speaker for a VoIP stream and headset versus built-in microphone for capture; verify actual sound/source and mixer readback. Repeat without a headset, across hotplug, NORMAL ↔ IN_COMMUNICATION ↔ IN_CALL, and after audio-service restart. Include conflicting concurrent streams and supported output combinations; do not infer route correctness merely from the selected device shown by Android.

## F63 — Recreated frontlight hardware is hidden by the brightness cache

**Sources:** [frontlight enforcement](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/eink/switcher/native/a6l_dualux.c:236), [conditional rediscovery](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/eink/switcher/native/a6l_dualux.c:337), and [frontlight overlay](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/kernel/a6l-eink-frontlight-v75.dtso).

After a successful write, `last_fl` stores the applied level. Every later iteration returns immediately if the desired level equals that cache, without reading the LED's state or checking whether its device still exists. The saved `fl_dir` also prevents periodic rediscovery. If the frontlight driver is removed and reprobed while the daemon remains alive, its configured `default-state = "off"` resets brightness, but the daemon does not reapply the unchanged request.

This is not a failed-write retry defect: writes that are actually attempted and rejected are already retried. The problem is skipping all backend access after a previously successful write. Unlike F50, it affects the rear frontlight's cached intensity, not LCD unblanking.

**Reproduction:** execute the exact enforcement method with the real brightness-conversion logic and controlled sysfs operations. Apply level 50 successfully. Make the backend unavailable for 20 iterations, then restore it at the same path with brightness zero. After 100 more iterations, desired/cached level is 50, actual fixture brightness remains zero, and there have been no recovery writes. Changing the slider to 60 immediately restores the light. A separate rejected-write/recovery control also succeeds.

The fixture models backend recreation and its off default, not an actual driver unbind or PWM measurement. Ordinary suspend/resume is not claimed to reset brightness: LED core/driver suspend handling may restore it correctly. The reproduced condition is backend loss/recreation while the daemon and unchanged request survive.

**Proposed fix:** periodically check backend existence/identity and available brightness readback. Invalidate `last_fl` after disappearance, recreation or mismatch, rediscover as necessary, and reapply the current desired level. Bound polling and error logging. The latest desired state must win: recovery while asleep or on the LCD should apply zero, not replay an obsolete nonzero level. Sysfs brightness is useful software-state evidence, not proof of optical output; see the [Linux LED interface](https://docs.kernel.org/leds/leds-class.html).

**Acceptance:** unchanged nonzero request across backend loss/recreation; changed maximum brightness or path; failed recovery writes; backend return while asleep, disabled or on LCD; ordinary suspend/resume; daemon restart. Verify eventual requested brightness without touching the slider and no unintended flash when the requested state is off.

## Evidence and scope

[Reproduction instructions](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round9-20260928/README.md), [results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round9-20260928/results.json), [hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round9-20260928/source-hashes.json), and [source snapshot](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round9-20260928/reviewed-source.zip) are preserved. Both harnesses passed their defect assertions under ASan/UBSan with no reported sanitizer errors, and the snapshotted source did not change during the run. Passing means the defects reproduced, not that the hardware passed acceptance.

The scan also examined microphone-mute integration, charger-guard logic and sensor control paths. No additional independent finding is claimed for those areas. Existing F1/F2/F6/F31 limitations and hardware acceptance requirements remain relevant; they were not renumbered to inflate this round's count.
