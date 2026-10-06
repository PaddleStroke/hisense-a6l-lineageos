# A6L hardware integration review — tenth pass

28 September 2026. **Complete: one new finding, F64, and a remaining gap in F22's altitude fix.** Both reproduced offline against the current Windows integration sources. Production files and the phone were not modified.

| Finding | Priority | Reproduced behavior |
|---|---|---|
| F64 | P2 | Queued GNSS time assistance reaches the modem without accounting for its queue delay, retaining the original uncertainty |
| F22 follow-up | P2 | Synthesized NMEA reports ellipsoid-only or missing altitude as mean-sea-level height, with an invented zero geoid separation |

Previous findings: [ninth pass](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-round9-20260928.md) and [original F22](C:/Users/Pierre/Desktop/A6L/docs/hardware-review-pass2-20260928.md). Findings describe the preserved source snapshot, not necessarily a previously built image or Claude's subsequent changes. No approval or security check interrupted this round.

## F64 — GNSS UTC assistance loses its time reference before entering the worker queue

**Sources:** [HAL time adjustment](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:216), [queued injection](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_client.cpp:459), and [QMI encoding](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_v02.cpp:89).

The HAL correctly advances the Android UTC sample using `CLOCK_BOOTTIME - timeReferenceMs` when `injectTime()` is entered. It then passes only the resulting UTC and uncertainty to the engine. The engine captures those values in a queued lambda and sends them unchanged when the worker eventually runs it. The original reference, or an equivalent reference recording the HAL-entry adjustment, is no longer available there.

This worker also handles blocking requests and assistance work. Any delay between the HAL adjustment and the actual QMI injection makes the supplied UTC stale. The encoder sends UTC in TLV 0x01 and uncertainty in TLV 0x02; it sends no clock reference through which the modem could recover the elapsed host queue time. This agrees with the [libqmi LOC message definition](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-loc.json).

**Reproduction:** run the production engine, QMI client and encoders against the existing in-process fake modem. Hold the worker inside an earlier position-assistance request, then enqueue UTC `1800000000000` with uncertainty 1 ms. Release the worker after a controlled 350 ms delay. The recorded delay was **351 ms**, the encoded UTC advanced by **0 ms**, and the advertised uncertainty remained **1 ms**. A later request with a different UTC and uncertainty encoded both supplied values correctly, ruling out a constant or broken recording fixture. The engine shut down normally.

This is a deterministic scheduling reproduction, not a measurement of phone latency or satellite-acquisition degradation. The test enters the engine directly; the HAL's initial adjustment was verified in the archived source. It establishes that a legal time sample can become stale in this queue, not that every real injection waits this long or that firmware necessarily uses it for a particular fix.

**Impact:** firmware can receive time assistance outside its stated uncertainty, potentially reducing its usefulness for acquisition or time initialization. This differs from F61's radio NITZ receive-time bookkeeping: the direction here is Android to the GNSS modem. F34 addressed STOP responsiveness during assistance, not UTC freshness.

**Proposed fix:** preserve UTC and a suspend-inclusive boot-clock reference through the queue, preferably retaining the original Android tuple. Immediately before constructing the QMI request, advance UTC by the elapsed time from that reference. Use the same clock domain throughout. Validate invalid/future references and arithmetic overflow; discard or refresh unusable samples. Account for source and residual clock/transport uncertainty. Correcting a known queue delay is preferable to merely inflating uncertainty by the entire delay. Keep QMI access on its owning worker rather than introducing concurrent Binder-thread requests.

**Acceptance:** verify immediate injection and injection delayed behind ordinary requests and XTRA work; include a suspend-inclusive delay, multiple updated samples, and service loss/reconnection. Capture encoded UTC and uncertainty against a controlled reference. A queued sample must either remain current with honest uncertainty or be explicitly discarded according to a documented policy.

## F22 follow-up — The synthetic NMEA path still mixes altitude datums

**Sources:** [GGA construction](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/nmea.cpp:56), [fallback call site](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/loc_client.cpp:752), [default enablement](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:150), [NMEA callback forwarding](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/hal/Gnss.cpp:366), and [corrected Android altitude mapping](C:/Users/Pierre/Desktop/A6L/device/hisense/a6l/gnss/lib/android_map.cpp:19).

The Android location mapping now correctly requires ellipsoid altitude for its altitude field. However, synthetic GGA chooses MSL altitude if present, otherwise ellipsoid altitude, otherwise zero. It similarly emits zero geoid separation unless both altitude datums are available. GGA field 9 represents mean-sea-level/orthometric height, and field 11 represents geoid separation; see the [Trimble receiver GGA documentation](https://receiverhelp.trimble.com/alloy-gnss/en-us/NMEA-0183messages_GGA.html). Unknown separation is not necessarily zero, and ellipsoid height cannot simply replace MSL height.

This path is reachable when NMEA synthesis is enabled, which is the default, and a successful position report arrives without recent modem NMEA. The engine's fallback threshold is 2,500 ms. Thus fixing the Android location callback does not also fix NMEA consumers.

**Reproduction:** execute the production NMEA encoder and Android mapping with controlled fixes:

| Supplied altitude information | Generated GGA MSL height | Generated geoid separation |
|---|---:|---:|
| Ellipsoid 80 m; MSL unknown | 80.0 m | 0.0 m |
| Neither altitude known | 0.0 m | 0.0 m |
| Ellipsoid 80 m and MSL 35 m | 35.0 m | 45.0 m |

The third case is a positive control. The test also verifies that the corrected Android mapping reports the first case as ellipsoid altitude 80 m. This is a production-function test with a source-verified fallback call chain, not a full Binder subscription or a live satellite observation.

**Impact:** apps consuming synthesized NMEA can receive a height in the wrong datum or a fabricated sea-level height. The numerical error depends on the actual geoid separation. This does not establish an error in the corrected Android location altitude. It is counted as an extension of F22, not a new independent finding.

**Proposed fix:** populate GGA altitude only from valid MSL height, and calculate separation only when both valid datums are available. Leave unknown fields empty, or suppress GGA if the chosen output contract cannot represent the available data; keep usable RMC/position output. Emit zero only when it is actually known. An explicit, validated geoid model could support conversion, but substituting the ellipsoid value cannot. Apply finite-value and assumed-altitude checks consistently.

**Acceptance:** cover ellipsoid-only, MSL-only, both, neither, genuinely zero, non-finite and assumed altitude. Compare Android location and synthetic NMEA using their respective altitude contracts. Exercise the switch between modem NMEA and synthesized fallback without inventing missing height or separation.

## Evidence and scope

[Reproduction instructions](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round10-20260928/README.md), [results](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round10-20260928/results.json), [run log](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round10-20260928/run.log), [source hashes](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round10-20260928/source-hashes.json), and [source snapshot](C:/Users/Pierre/Desktop/A6L/research/hardware-review-round10-20260928/reviewed-source.zip) are preserved. The harness passed under ASan/UBSan with no reported sanitizer errors. No snapshotted source changed during the run. Passing these assertions means the defects reproduced; it is not acceptance of a fix.

The broader scan also covered Wi-Fi HAL lifecycle/netlink handling, module startup, storage configuration, sensor batching/flush paths, GNSS session handling and Bluetooth management responses. No additional independent defect is claimed from that inspection. Existing storage/encryption and on-device hardware acceptance gaps remain open; fewer new findings do not establish complete hardware support.
