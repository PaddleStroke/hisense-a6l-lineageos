# VoLTE and camera fresh-eye review — 28 September 2026

The current next candidates are **volte5** and **camera7/camfix6**. Neither has a recorded phone result in the evidence reviewed. Claude's IMS subscription-binding work is a credible next step. The camera evidence supports a shared capture-path problem, but does **not** yet prove AXI starvation. Fix the test gaps below before treating another PASS marker as a working feature.

This review used the current uncommitted sources, the dated reports, saved attended logs, stock vendor IDL, and independent offline regressions. It did not use ADB, submit relay commands, place calls, change modem provisioning, build a ROM, or modify the active driver/radio sources. Candidate patches and tests are isolated in [research/fresh-eye-20260928](../research/fresh-eye-20260928/). Hardware remains unverified.

**Evidence and actual status**

| Area | Latest observed failure | Prepared next candidate |
|---|---|---|
| VoLTE | Sep 28 t28b: IPA present, service 770 published on mux9, modem's initial helper requests answered; IMSA registration/services queries return `INVALID_OPERATION`; `pdps=0`. Orange MBN is active, LTE voice domain remains 3GPP-CS. | volte5 binds IMSS/IMSA to subscription 0, enables IMS through IMSS, then watches IMSA. No phone evidence yet that binding clears error 70, that flags were disabled, or that IMS registers. |
| Camera | Sep 28 camera6/camfix5: actual VFE clocks 200/404/480/540 MHz; 225/393/239/225 SOFs, 0/1/0/0 done interrupts, buserr=1 in every run. The single completion is about 4.3 seconds late. Prior fixed-buffer scans show only 5–93 of 2156 lines written. | camera7/camfix6 adds VBIF settings, UB alternatives, ICC votes, WM layout variants and experimental recovery. No phone result yet. |
| ROM integration | r4 source stages camera6/camfix5, not camera7/camfix6. VoLTE remains gated off; Orange profile boot policy and the staged VoLTE voice-session module remain unresolved. | Recovery-bundle success must still be integrated and validated in Android. |

Primary local evidence: [.relay/outbox/vo28-02.out](../.relay/outbox/vo28-02.out), [vo28-03.out](../.relay/outbox/vo28-03.out), [c6-clk.log](../.relay/outbox/cam28/c6-clk.log), [dmesg-cam6.txt](../.relay/outbox/cam28/dmesg-cam6.txt). Design/status reports: [volte5](volte5-20260928.md), [camfix6](camfix6-20260928.md), [r4 merge](merge-20260928.md). The old `port-status.md` is dated Sep 23; it is not the current status. The merge report's statement that camfix5 is untested is also superseded by the camera6 clock-loop logs.

**Findings to address before the next run**

1. **IMS setup can get permanently stuck after a transient failure.** In [a6l_imsdcm.cc](../device/hisense/a6l/radio/tools/a6l_imsdcm.cc), `watchIms()` sets `imssDone=true` and `attached=true` before checking setup outcomes. A failed bind/enable is not retried while the services remain published. Subsequent polling only reads IMSA. The test deliberately starts the daemon before taking the modem online, so an initial not-ready response is a realistic failure path. Use separate service-present/setup-complete state, bounded retry/backoff, and service-generation invalidation on withdrawal/republication. Retry binding/registration as needed; read flags before retrying writes. Do not retry a successful enable on every poll.

   Additionally, [ims_setup.cc](../device/hisense/a6l/radio/qmi/src/ims_setup.cc) continues after a failed explicit subscription bind. In enable mode it can send settings to an unbound/default subscription. The isolated regression reproduces this. The supplied **ims-review.patch** stops both setup functions on bind failure. Apply that together with the retry work; the patch alone does not implement recovery in the watcher.

2. **VoLTE registration tests accept stale success.** `wait_reg()` and `summary()` in [volte5-test.sh](../device/hisense/a6l/radio/tools/volte5-test.sh) search the entire daemon log for any previous `REGISTERED`. An offline run of the extracted, unchanged function reports registered even when followed by `not-registered` and `GONE`. Reusing an existing daemon makes this relevant across verify/call invocations. `regSeen` in the watcher also stays latched after loss.

   Replace historical-marker matching with current state scoped to daemon/service generation and subscription, or a fresh bound query immediately before a call. Reset on deregistration/SSR; include query failures as unknown. `verify` currently ends with `result=OK` even if registration timed out. The call summary drops the underlying `connected` field. Separate “script completed” from “IMS registered” and “connected IMS call”; report unsuccessful registration/call as failure of that stage. An IMS origination or CS fallback is not proof of a connected VoLTE call.

3. **IMS rejection text is silently lost by the parser, and the old test repeats the bug.** [ims.cc](../device/hisense/a6l/radio/qmi/src/ims.cc) decodes the registration-error string using `str8()`. These top-level string TLVs contain the string bytes directly; the TLV already gives the length. For `Forbidden`, the old parser treats `F` as a length and discards the text. This affects responses and indications. The existing `volte2_tests.cc` fixture incorrectly uses `str8()` too, so it passes the wrong wire format.

   This is confirmed against the [upstream IMSA schema](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-imsa.json) and its [string code generator](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/build-aux/qmi-codegen/VariableString.py). The candidate patch fixes the parser and fixture. It will expose future network rejection reasons; it does **not** explain today's QMI error 70.

4. **The camera `burst3` sweep currently tests nothing.** [run-camera.sh](../device/hisense/a6l/camera/run-camera.sh) defaults the sweep to `WM=4`, but [camfix6_patch.py](../device/hisense/a6l/kernel/camera/patches/camfix6_patch.py) only changes frame-based `BUFFER_CFG` inside `if (a6l_wm & 2)`. Consequently `V6=32` cannot select burst3 with that default. The stock-buffer-config control is missing too.

   The review compiles the actual reconstructed `vfe_wm_frame_based()` with memory-backed registers: `WM=4` gives `BUFFER_CFG=0` with and without bit32; `WM=6` gives 2 then 3. Use **WM=6 for both baseline and burst3**, so the comparison changes only the burst field. The supplied **camera-sweep-review.patch** changes the sweep default accordingly. Explicit `WM=4` remains available as an upstream-buffer-config control. A winning fixed-buffer diagnostic must then pass the normal `WM=3` queue path.

5. **Camera progress/PASS markers do not establish a complete valid frame.** The camfix6 progress probe writes the fill word back over the first word of each supposedly written line while DMA can still be active. Its line-prefix search also assumes contiguous writes. Use it as an intrusive diagnostic; repeat any apparent improvement with bit128 disabled and reduced IRQ logging. In [a6l_camcap.c](../device/hisense/a6l/camera/tools/a6l_camcap.c), timeout breaks out and returns success, buffer-error flags are ignored, and zero `bytesused` falls back to saving the entire allocation. The shell calls any nonempty output a capture PASS.

   Make timeout/error-buffer/short-payload/write failure explicit failures; preserve the capture command's return code; validate returned format, stride, payload bounds and expected image extent. Finally inspect complete pattern coverage and several consecutive frames. Even a full-size allocation or the driver's reported payload size is insufficient if unwritten fill bytes remain. This is especially important with recovery, which can produce a torn frame.

6. **The camera diagnosis is more certain in the report than the measurements justify.** The clock experiment rules out insufficient *VFE core speed alone* for the tested configuration. It does not rule out AXI clock/vote problems, CSI payload/EOF/framing errors, or WM geometry/packing. SOF proves frame-start reception, not a complete valid frame. Absence of a reported SMMU fault does not validate the whole DMA path. Stock software ignoring an RDI error does not establish that writing the same status register clears it.

   Keep camfix6's bit8 W1C/reload experiment separate from the first stock-register comparison. Its semantics remain unverified; default `0x0f` combines four changes, including recovery. Start with `V6=0`, then `V6=1` (VBIF only), then explicit stock RDI UB `V6=64`, bandwidth `V6=4`, and WM mode tests. Combining candidates can follow once isolated results exist. `A6L_AXI` uses hard-coded parent frequencies, so its approximate kHz value needs confirmation against clock-provider state. `icc_set_bw` returning zero proves request acceptance, not achieved bus bandwidth.

**VoLTE: shortest useful route to working service**

The missing bind/enable sequence is the best current hypothesis. Stock IDL was independently decoded during this review, and the public [IMSA binding definition](https://raw.githubusercontent.com/linux-mobile-broadband/libqmi/main/data/qmi-service-imsa.json) agrees with the IMSA message ID/TLV. This supports Claude's implementation direction. The fake modem intentionally fails unbound requests and succeeds after binding: its passing tests verify the code, not the real modem hypothesis.

Proceed through distinct checkpoints, preserving raw response TLVs and modem-generation boundaries:

| Checkpoint | Required evidence | If it fails |
|---|---|---|
| Stable modem prerequisites | IPA4 loaded **before** MSS, SIM ready/provisioned on the intended slot, active Orange MBN, LTE PS attachment | Fix these before interpreting IMS results. The run without IPA crashed; the IPA-first run supplies the useful IMS baseline. |
| Bound IMS clients | IMSS bind0x98 and IMSA bind0x33 success on the same clients used for subsequent requests | Retry after readiness/online and rebind after SSR; inspect selected subscription and full bind result. Do not immediately rewrite NV/EFS. |
| IMS/VoLTE settings | IMSS get0x90, conditional set0x8f, successful readback with both IMS and VoLTE values | A missing optional VoLTE TLV is **unknown**, not proof it is enabled. Current `ImssOutcome::enabled()` intentionally checks only `ims_service_enabled`. Log both values and distinguish unsupported/omitted fields from zero. |
| IMS bearer requested | Actual IMSDCM `PDP_ACTIVATE` request with APN/profile, family, subscription and instance | If binds/settings work but no request appears, inspect current IMSA rejection/registration state and network VoPS indication. Establish whether the same SIM/operator/device works on stock; the existing stock bugreport has no SIM and cannot answer that. |
| Bearer established | Successful WDS setup on the intended subscription/mux and a valid address for the requested family; capture call-end reasons/P-CSCF data | Audit `ImsDcmService::activate()`: it currently ignores bind-mux/set-family failure, does not bind WDS to the requested subscription, and can report `State::Up` with no address after settings failure. These are downstream risks, not the explanation for the observed `pdps=0`. |
| Usable IMS | Current IMSA registered + voice available on WWAN, followed by an answered call whose actual domain is IMS | Registration alone is insufficient. Then verify incoming calls, two-way audio, correct VoLTE DSP session, hangup and recovery after radio restart/sleep. |

Do not equate two different NAS enum fields: the report's voice-domain **preference** value 3 (IMS preferred) and the observed system-info `lte_voice_domain=3` (3GPP-CS) are different fields. An Orange profile being active is also not proof of subscriber IMS entitlement or a successful IMS attachment. Recheck `ims_voice_support=0` after the corrected setup; if it persists, investigate network/provisioning with a known-good baseline.

Before integration is complete, ensure the ROM starts/enables/rebinds IMS after the correct SIM is ready, applies the chosen Orange-profile policy, stages the tested voice-session module, and exposes appropriate registration/call/SMS behavior through Android's radio interfaces. A separate Android ImsService is not yet demonstrated to be the blocker for the chosen modem-centric approach.

**Cameras: the missing discriminating experiment**

Use the **CSID internal test generator** to remove the sensor and D-PHY from the failing path. Current `MODE=bars` invokes `-t 2` on the *sensor*. It still exercises that sensor's mode table, lane configuration and CSI transmitter.

The saved kernel source already implements the CSID generator: [camss-csid-4-7.c](../.relay/outbox/cam28/src5/camss-csid-4-7.c), `csid_configure_stream()`, and [camss-csid.c](../.relay/outbox/cam28/src5/camss-csid.c), `csid_set_test_pattern()`. This is a specific next diagnostic, not a claim that it has run.

Extend the capture tool with a separate CSID-pattern option:

- Disable the selected CSIPHY→CSID sink link **before** enabling the CSID `V4L2_CID_TEST_PATTERN` control; the driver rejects the generator while that link is enabled. Keep the downstream CSID→ISPIF→VFE RDI links. Do not require a physical sensor for this mode.
- Set/read back the CSID **source** format explicitly while the generator is active; propagate it downstream and to the capture node. Test a modest aligned RAW10 geometry first, then the failing full geometry. Record actual clocks and SOF rate. Smaller images do not automatically mean a lower generator frame rate; add blanking control if a deliberately slower source is needed.
- Disable the generator and restore the physical path for the paired sensor test. Fail explicitly on control/link setup errors; the existing tool mostly prints those errors and continues.
- First run with recovery/progress rewriting disabled. Require repeated complete, recognizable patterns with valid buffer metadata. Compare the same WM/UB/VBIF configuration across synthetic and physical sources.

| Result | What it narrows down |
|---|---|
| CSID pattern has the same partial-write/no-done failure | Sensor register tables and D-PHY are bypassed. Concentrate on CSID output/ISPIF routing, VFE WM programming, SMMU/AXI/VBIF and buffer completion. |
| CSID pattern works at full geometry; physical sensor fails | Reopen sensor CSI mode, lane mapping/rate, data type/VC, frame-end and embedded-data handling. Obtain input-side packet/error evidence; SOF alone is inadequate. |
| Modest geometry works, full geometry fails | Measure rate/bandwidth/stride/UB and clock dependencies; compare at controlled source rate before calling it AXI starvation. |
| VBIF-only or a WM variant works in both paths | Repeat with normal WM=3, probe/recovery off, repeated start/stop, then each sensor. Only then promote the minimum proven change. |

Test S5K3T1 and HI846 separately as controls too; do not postpone every other sensor until IMX576 succeeds. Record their PHY/CSID route and geometry, because they are not automatically identical load/path comparisons. Shared failure points to shared CAMSS plumbing; one sensor succeeding provides a useful working reference.

Raw capture is only the first camera milestone. The remaining tracked work includes IMX576/GT9769 autofocus, S5K3T1 front and HI846 auxiliary capture, exposure/gain/test-pattern controls, orientation/Bayer correctness, calibration/quality, repeated stream-off/reopen, and libcamera/SoftISP Android HAL plus Camera2/CameraX preview/still/video. Chip-ID or VCM initialization is not a complete camera PASS.

**Offline deliverables and limits**

- [reproduce.py](../research/fresh-eye-20260928/reproduce.py): snapshots sources into an ignored `work/`, exercises original and patched IMS code under ASan/UBSan, runs relevant existing suites, reconstructs camfix6 for a register simulation, and reproduces stale registration using only the extracted shell function.
- [ims-review.patch](../research/fresh-eye-20260928/ims-review.patch): raw IMS error-string decoding, corrected old fixture, and stop setup on failed explicit bind. Does not implement watcher retry/state tracking or WDS hardening.
- [camera-sweep-review.patch](../research/fresh-eye-20260928/camera-sweep-review.patch): makes the default baseline/burst3 sweep actually exercise BUFFER_CFG. It changes the diagnostic script only, not the driver or default camera behavior.
- [results.txt](../research/fresh-eye-20260928/results.txt) and [source-hashes.json](../research/fresh-eye-20260928/source-hashes.json): reproducible outputs and captured source identity. The active checkout has concurrent/uncommitted work; validate patch applicability again before merging.

Validation completed: original IMS code **6 passed / 5 failed** on the new regression checks; candidate **11/11 passed**. Existing VoLTE2 **291/291** and VoLTE5 **210/210** pass against the candidate with ASan/UBSan. The compiled camera register simulation and isolated stale-registration shell reproduction both confirm the findings above. Both patch applicability checks pass against the reviewed checkout.

These patches are review candidates, not changes to the staged phone bundles. Rebuild affected test binaries/scripts and refresh the bundle hashes before an attended test. The new regression failures are software findings; **neither VoLTE nor camera operation has been newly proven on the phone by this review**.

**Follow-up: complete hardware inventory review**

See [hardware-review-20260928.md](hardware-review-20260928.md) for all H1–H64 items, 13 actionable findings with proposed fixes/validation, and offline charging/sensor reproductions.

The integration ledger now contains a newer **r5 / 29 September** update: VoLTE5 still has no IMS PDN and the modem closes service 770 after 0x2e/0x34; camfix6 and the corrected WM=6 sweep have now been tried and still fail sustained capture (best reported iccmax 3 completed frames, burst3 1). This supersedes this report's earlier “not yet phone-tested” status and means the WM sweep correction alone did not fix capture. The follow-up also incorporates reported MBHC v2, flash and microSD successes. These are ledger-reported results, not phone tests performed by this review.
