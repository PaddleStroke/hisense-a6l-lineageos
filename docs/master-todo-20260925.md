# Master TODO: a daily-drivable LineageOS 24 on the Hisense A6L (25 Sep 2026)



Latest 5 October late evening: bounded default-off GPU restoration diagnostic
module/vendor kit ready, reviewed and staged; not flashed/tested. Phone untouched
after requested overnight sleep. Morning procedure: gpu-restoration-test-20261006.md.

Latest 5 October evening: GPU resume-order candidate installed/tested and FAILED.
Same opcode/ringbuffer fault captured, reboot confirmed by user and new boot ID.
Later user requested overnight sleep: USB stay-awake disabled, guard released,
framework Dozing observed, USB absent. Overnight stability unverified; no test armed.
Next audit CP/ring initialization and actual job/power sequencing.

Earlier isolation: video-to-sleep failure narrowed to GPU transition.

Post-video ordinary display callback prefix40 is stable for120s; identical

prefix42 adds Adreno and reproduces delayed GPU opcode fault/ringbuffer timeout

then a new boot. Storage and USB suspend callbacks were excluded. Host logger

captured the fault missed by phone-disk logs. Precise race/reset mechanism is

not yet proven. Restore-before-scheduler GPU candidate compiled, ABI reviewed,

and vendor content audited; vendor-only hash-only installation/readback passed; phone remains in recovery.

Read `handoff-20261005-claude.md` first and its linked live installation status.

Do not treat a successful callback return as a stable phone or mark sleep fixed.



Immediate TODO:

- GPU install/runtime verified but physical trial FAILED; preserve captured trace
  and audit/instrument postcollapse GPU initialization before another trial.

- On a freshly prepared/logged boot repeat main-camera video, GPU prefix42,

  and120s same-boot observation; then ordinary video→lock20s→wake acceptance.

- If it fails, instrument GPU scheduler/forced PM/ring initialization directly;

  do not repeat unbounded driver groups or assume the final reset stack exists.

- Preserve the two already-shipped October2 GPU recovery patches when rebuilding.

- Hardware Venus encoding is still disabled; software video measured9.295fps,

  Camera-only direct-YUV APK trial16.463fps. Complete C2/firmware/ABI/hardware

  acceptance before enabling a hardware encoder. Do not promise60fps.

- Camera colors remain washed out; calibrate channel/illuminant/black-level

  against stock. User accepts all three previews and recording/save/playback.

- User now confirms video audio audible and e-ink labels black/visible;

  dedicated voice recording/playback and two-way call audio still need retest.

- E-ink notification moving artifacts remain; keep waveform/animation changes

  out of the GPU sleep comparison. Dualux watchdog candidate remains uninstalled.

- Quiet console fixed measured wake delay; USB QSCRATCH parent restoration

  passes two attended cycles. Deeper CPU idle and sleep after video remain open.





Latest 4 October evening: r7c logger-only vendor update is installed. All29

runtime pins pass. Both fixed rings accept fresh CRC-checked markers through

38minutes awake with stable2.1MiB metadata headroom. Phone stays awake on LCD

while USB plugged; no unattended sleep or hardware-codec probe is performed.

RAM retention fails across both tested recovery resets, including explicit warm.

Sleep root cause remains unknown; PM_DEBUG kernel scope and module ABI audits pass.

Actual Android DT confirms Venus hardware-codec block disabled. Native software

AVC format probes encode about37-38ms/frame; both formats decode identically,

with no encode-only NV12 speed gain. C2 input candidate remains uninstalled.

Camera colors/framing/speed and e-ink notification artifacts remain open.

See logger-r7c-20261004.md and video-encoder-audit-20261004/README.md.





Latest 4 October: r7c is installed and passes all 28 runtime pins. Saved front

video measures 9.295 fps, versus about 3.7 fps on r7b, and finalizes normally

2.643 seconds after Stop. Exact SIMD conversion reduces sampled preparation

from 249.63 to 58.38 ms; compression remains 56.67 ms. Native capture advances

with hardware at 48,000.6 frames/s, sampled queue zero, closes normally, and

retains the correction to the prior sustained AAC timestamp gaps. Acoustic

speech/playback/call acceptance remains pending. No next image build has started.

Review video viewport/direct-YUV feasibility before another performance patch;

do not claim 30 fps, calibrated colors, optical labels or sleep reliability.

The build retains camera tuning, audio fix, clocks, kernel and e-ink waveforms.

Later same-boot Camera-only trial: matching native HD preview/video removes the

two-pixel shared-viewport crop and GL pass. Saved video reaches 16.463 fps with

406 distinct frames, normal saving/closure, and gap-free 24.512s AAC. Sampled

YUV preparation is 16.12ms; encoding 43.84ms. Signed APK matches the original

certificate; only classes.dex changes, with images untouched and one 8.2MB APK

for rollback. Current app is this /data/app trial; there is no r7d image. Test

other cameras, portrait/mirror and mode transitions before release integration.

Stock nonidentity color matrices are recovered but remain unapplied pending

channel/illuminant/black-level calibration. See rom-r7c and its trial evidence.



User r6y feedback remains the optical/acoustic baseline: previews about30fps,

selfie recording about5fps with failed save, washed-out colors, silent effects /

recording, e-ink status row fixed but app labels invisible, another lock/wake

failure. Later label and playback fixes require user confirmation; do not mark

these accepted from code or counters alone. Matched stock color photo is pending.

The later r7a camera-provider SIGPIPE was caused by an abbreviated diagnostic

pipe; full dumps now avoid that trigger. See rom-r7a/r7b reports.



r6z is installed, boots and passes all21 runtime payload pins. System/vendor

readback and protected checks pass; boot/DTBO retained, about1.1MB hash-only

receipts without development-image snapshots. Native VIDEO processing now

delivers29.995fps with21.64ms median ISP, but encoded720x1278 video remains

4.099fps and finalizes about6s after Stop. Both clean recordings save and close

normally; the software encoder's busy main thread needs conversion/encode stage

timings. Playback now runs through73 hardware-buffer wraps without the previous

premature XRUN. Audible clarity, new spoken capture and calls still need user

acceptance. Themed e-ink launcher labels are installed but optical acceptance

is pending. Washed colors and intermittent sleep/wake failure remain open.

Recovery USB appeared late; recovery's own kernel already ran and its gadget

watchdog reconnected at77s. This does not establish a four-minute Android

shutdown. r7a default-off encoder timing and RAW numeric calibration diagnostics

are being prepared; they are measurements, not claimed color/sleep fixes.

See `rom-r6z-20261004.md` and the newest status.json for actual build state.



r6y vendor-only installation/readback passed on 4 October after recovery

reconnection. Boot/system/DTBO skipped; hash-only receipts about 1.1 MB,

no development-image backup. Runtime verification and the complete three-camera

comparison pass on the same boot. Eight changing RAW frames per sensor match

before/after legacy cache synchronization. Native-lease matched preview rates

main28.77/selfie29.96/wide28.10fps; generic25.82/29.78/26.61fps. Native debug path

remains default-off after reboot. Front VIDEO preview still ~5fps: RAW resize

154ms dominates ISP170ms even without encoding. A full-field native VIDEO

source candidate is under review. User recording overlapped our force-stop;

repeat a clean recording before diagnosing a save crash. Washed colors remain.

Fresh WAV has microphone signal and lacks old85ms gaps; silent playback remains

downstream of nonzero AudioFlinger output. E-ink status icons visible, launcher

names invisible; theme-label fix compiled, not installed. Previous persistent

kernel log ends at s2idle entry around USB loss, without a retained panic;

resume remains unconfirmed. Source ownership/async retirement and fail-closed

native RAW verifier reviews and physical parity pass. Stable local Perfetto

after disabling an invalid AVF vsock relay is a separate debug correction,

not an established sleep fix. See `rom-r6y-20261004.md` and status.



r6x installed vendor-only; readback/protected and runtime hashes pass. All3

native preview comparisons completed in boot bc62c5e2, with Camera closed normally.

Diagnostic-off matched20s preview main26.74/selfie30.20/wide26.44fps; recent actual

presentation rings24.63/29.52/28.61fps show remaining main/wide variability.

Sensors remain30Hz. Main ISP36.84ms, RAW START1.80ms, combined END2.14ms,

requeue2.76ms, metadata1.60ms; mapping is now0.020ms. Paired dispatch overhead

over longest worker is0.78ms. Native RAW read-lease physical coherence and

async HAL destination/shared-source lifecycle successors remain isolated.

System/boot/DTBO retained; ~1.1MB receipts/no development snapshots. No

stock-equivalent smoothness or optical/video acceptance yet.

See `rom-r6x-20261004.md` and its status.json.



r6w is installed vendor-only with readback/protected checks passed; retained

system/boot/DTBO skipped and about1.1MB hash-only receipts/no image snapshots.

Boot completed12:11:54Paris; runtime hashes pass. Native camera workers share reviewed64-row

chunks; exact preview Y/UV scaling runs concurrently and shared-FD NV12

synchronization is deduplicated without omitting START/END. Actual ARM cached

main processing improved about30–31→26–27ms; preview scaling improved25–30%.

Pixel/AE/AF-stat equivalence, lifecycle sanitizers and independent reviews pass.

All3 opened/closed normally: processing main19.65/selfie28.81/wide29.80Hz;

SurfaceFlinger main~21.3/selfie~29fps. Wide presentation includes main startup

and needs isolated counter deltas. Main remains unresolved; stock-equivalent

FPS is not established. Sensor/FoV/resolution/tunings/IPA/clocks are retained.

See `rom-r6w-20261004.md`. Remaining RAW cache-maintenance and per-frame mapping

costs are under isolated investigation; no synchronization is being removed.



r6v is installed vendor-only with readback/protected checks passed, unchanged

system/boot/DTBO skipped and1.1MBreceipts/no snapshots. Boot completed11:20:40Paris;

runtime hashes passed. All three cameras opened/closed normally at native FoV.

Live processing completions improved from6.63/9.81/17.22Hz to19.34/23.25/30.01Hz

for main/selfie/wide; SurfaceFlinger independently shows main~19.8fps and

the other two in the mid20s. Main/selfie and presentation costs remain open;

stock-equivalent smoothness is not established. Camera sluggishness is

traced to per-pixel CPU software ISP work: all three sensor timestamps advance

at about30Hz, while native processing completes at6.63/9.81/17.22Hz for

main/selfie/wide. Exact NEON RAW/NV12, faster preview columns and six workers

are included after guarded differential and image/statistics checks. Cached

full-Debayer medians are about30–32/20/11ms; this is not live preview FPS or

stock-quality acceptance. Both vector gamma-table variants were slower and

rejected. The vendor-only build has54tasks; system/boot/DTBO are retained,

unchanged kit images use hardlinks and no partition snapshots were created.

The phone disappeared from USB after a remote recovery request but eventually

returned in V75recovery after about6minutes. A duplicate shared-NV12 DMA

synchronization and remaining preview costs are under isolated investigation. See

`rom-r6v-20261004.md` and its status.json. The preceding r6u boot lasted about

82minutes; this does not establish sleep stability.



r6t is installed and booted on 4 October: 139 incremental tasks in 3 min 22 s. It corrects

the Camera capability gate and enables a narrowly guarded small-Auto REGAL

trial. Only reviewed app/mirror/init inputs and build metadata change; camera

HAL/tuning, daemon, kernel/GPU and electrical waveforms retain r6s. System/vendor

readback and protected checks passed, boot/DTBO skipped, no image snapshots.

All three native Camera streams are physically verified. Pierre reports faster

wide preview and exposure, main still slow, and much better E-ink. White launcher

labels and status icons against white rear wallpaper remain a rendering issue.

A reported automatic reboot on sleep reopens stability; saved live logs stop at

s2idle entry. Recovery pstore is empty; metadata has the same boot ID and no

later panic record. Persistent stream children stopped early while their

supervisor remained alive. Logger health supervision is under review for r6u;

sleep root cause is not established. See `rom-r6t-20261004.md`.



r6u is installed and booted with complete image/APEX signature checks,

system/vendor readback and runtime hashes verified. Boot/DTBO skipped; hash-only

receipts are about 1.1 MB. It includes bounded primary microphone buffering,

rear-white launcher contrast, persistent logger supervision and default-off

camera stage profiling. Audio fixtures

reproduce repeated silence insertion in capture and a separate DSP playback

queue ordering defect. The capture fix is reviewed; playback/call/gain and sleep

fixes are not included. Full module/image compilation passed; physical contrast

and capture validation remain pending. The logger's reader/writer children are

alive in the short awake baseline. See `rom-r6u-20261004.md`.



Late-evening update: r6s is installed and booted, with system/vendor readback and

protected checks passed, boot/DTBO skipped and about 1.1 MB of hash-only records.

It adds native camera metadata/selection, serial KMS tuple checks and an ordered

dither trial. Normal Camera opens/closes all three without an observed abort,

but the rear cameras still select 1600×1200; selfie uses native 2296×1728.

The preference was incorrectly gated by a high-resolution capability resource.

Its reviewed app correction is prepared for r6t. Static Auto still emits three

ordinary full-quality updates after its one forced clean; a narrow partial-mode

trial for minor settled damage is being reviewed. See `rom-r6s-20261003.md`.

Kernel, GPU, electrical programming and camera tuning are retained. Moving

optical artifacts and stock photo quality remain open. The phone reached

recovery and its exact raster trace was copied onto the laptop. SSH then became

unavailable; desktop build work continues without claiming a later flash.



Earlier evening successor: r6r is installed vendor only, retaining verified r6q system,

boot and DTBO. Vendor readback and protected checks passed; hash-only records

total about 1.1 MB, with no development-image snapshots. Included after independent review: terminal camera request

cancellation, bounded relative AE correction (four-output cadence retained),

and e-ink Auto damage/ACK bookkeeping and selected-mode settle. Physical

acceptance remains pending. Full capture overlap and native camera output-size

selection are isolated follow-up work. See `rom-r6r-20261003.md` and the latest

build status viewer; this is not a claim of stock-quality cameras or resolved

moving e-ink artifacts.



r6r boots. The native-wide probe saved both photos and closed without camera

aborts. Native 1624×1224 JPEG plus mapped 1600×1200 preview removes RAW resize:

ISP median 192.649 → 51.789 ms; preview callback median 196 → 55 ms, approximately

5.1 → 18.2 callbacks/s. Normal Camera selection is unchanged and remains a

separate isolated candidate; this is not photo-quality acceptance.



Latest investigation: `camera-eink-root-causes-20261003.md` records all-three

camera EV captures, longer convergence and genuine covered-lens tests. RAW

scaling dominates CPU ISP; wide timing metadata is wrong; asynchronous shared

statistics ownership has a reproduced race. Reviewed fixes are in built r6q,

whose full image/signature audit and hash-only installation passed. Rear preview

remains approximately 5 FPS; selfie is distinctly faster (estimated 15–20 FPS).

Wide brightness eventually settles, but takes about 47 seconds: valid statistics

arrive only every four processed frames and exposure increases about 7–8% per

update. Repeated camera stop/switch aborts now require request-cancellation

analysis before camera stability can be accepted.

The one-shot exact e-ink input/conversion trace and explicit WM notification are

included, with electrical sequences retained. Optical artifact origin and stock

camera quality remain unresolved. Covered JPEGs are nearly black, arguing

against a large universal black-floor mismatch; dark-scene AWB instability

needs retesting after the ownership fix. See `rom-r6q-20261003.md` for scope,

permissive-development diagnostic access and the physical validation plan.



## r6p installed and physically tested — 3 October



r6p is installed with system/vendor readback verified, protected/userdata

checks unchanged, and unchanged boot/DTBO skipped. No development-image backup

was made; the receipt capture is about 1.1 MB. The phone is back in recovery

after the attended test. It contains the numeric e-ink KMS blend correction,

aspect-preserving camera RAW scaling/mode selection, and Aperture Auto-FPS

validation fix. Actual image inventory changes exactly 12 files; all other

system/vendor content, kernel, GPU and tuning remain identical to r6o. Physical

Pierre confirms the camera crop is fixed on all three lenses and the wide

selector no longer crashes the app in his test. Preview is still roughly 5 FPS

by his estimate; wide is very dark, and main/front quality remains poor against

stock. ISP processing benchmarks show rear 207.580 ms, front 391.289 ms and

wide 193.395 ms per frame; these are processing times, not delivered FPS.

A recovered camera-provider tombstone at 17:53:20 shows a separate typed-control

assertion in the retained software IPA; fix it before calling camera stability

accepted.



The black top/bottom e-ink strips are gone. Moving artifacts remain. Observed

switch times are about 1 s EPD → LCD and 2 s LCD → EPD; the latter still fails

the requested one-second target. Preserve the blend fix and investigate capture

coherence and measured switch stages before altering waveform/cleanup policy.



Pierre reports a crash/reboot after leaving the phone on a menu, followed by a

black screen. Live and metadata logs end at uptime 498.95 s in the same boot,

after DPU vblank timeouts and entry into s2idle. No panic or subsequent Android

boot is retained; pstore is empty. Do not equate the saved suspend event with

the reported reboot or claim its cause is proven. Recovery diagnostics are

saved in `firmware/extracted/rom-r6p-20261003/attended-feedback`.

See `rom-r6p-20261003.md`. The following r6n/r6o results are historical.



This section supersedes the historical build states below. r6m main rear and

selfie preview and photo capture both work. r6n images are verified; the

hash-only update is installed and readback verified, preserving userdata and

skipping unchanged DTBO. Android boots; actual rear lens/AF initialization

and orientation metadata are confirmed. All three cameras now save upright

photos and both tested videos save. Quality and sustained video performance

remain unacceptable; copied originals and measured results are preserved.



r6o is now installed with system/vendor readback verified and protected/userdata

checks unchanged; boot/DTBO were skipped. LCD physical acceptance passes.

It adds fresh-draw synchronization, earlier prepare detection and first eligible

Auto image submission, plus JPEG bounds/AE-state corrections and opt-in camera

diagnostics. It does not mark tonal quality, video rate or audio fixed. See

`rom-r6o-20261003.md`.



r6o boots with hardware graphics. LCD Display baseline passes: responsive,

no garbled text. Minor UI follow-up: initialize tab filtering before preferences

are first shown, so opening Display does not visibly remove options. Rear

screen and camera tests still fail acceptance; audio remains unvalidated.



Latest Auto feedback on r6o: warm first rear image ~2 seconds, then intrusive

cleanup at ~4 seconds and a longer repeat at ~5–6 seconds; apparently static

pages also refresh repeatedly. Prioritize one cleanup per settled content epoch,

prevent automatic repeats without meaningful content/ghost-history changes,

and investigate a less intrusive stock-supported cleanup. Policy's quality

refresh versus daemon's retained fast mode is a confirmed request mismatch;

the source of later measured pixel changes remains unproven.



Fastest is verified and has no perceived sharpening, with tolerable ghosting,

but black status/navigation bars, irregular horizontal artifacts and moving

patches remain in both modes. The capture's blend enum is definitively wrong

for the retained KMS ABI; the minimal correction and an external numeric

150-case regression are integrated for r6p. Measure that correction first,

before hiding damage or changing cleanup thresholds. The isolated mode-correct

Auto cleanup candidate can increase one static cleanup from 39 to 79 waveform

frames and is not promoted as a smooth/nonflashing fix.



Pierre's stock comparison shows main/front camera fields of view look roughly

2× zoomed. The software debayer crops rather than scales the sensor input;

actual correction is being prepared. Wide selection crashes Aperture with

`Video frame rate not supported with the requested video quality`, confirmed

by two recovered Java reports. Pierre also observed a phone reboot afterward;

no pstore record survives, and metadata evicted that boot, so its kernel cause

is unestablished. See r6o recovery evidence and the upcoming r6p report.



- [x] **Main rear and selfie preview/photos on r6m.** Both saved JPEGs and

  recovery/live logs are preserved locally. The gralloc import bridge works.

- [x] **Camera opening, selection and orientation on r6n.** User confirms all

  three cameras work; the copied new photos are upright. Two videos save.

- [ ] **Camera quality and switching.** Continuous center AF improves detail

  but hunts and misses the intended subject. Photos have raised shadows, a

  color cast and clipped highlights. The two videos contain only 4.6–4.7 unique

  frames/second despite requesting 30 FPS from the software encoder. Switching

  and perceived recording startup remain slow. The JPEG footer is incorrectly

  placed at padded allocation capacity; r6o includes the bounds-tested fix,

  with physical acceptance pending. See `camera-quality-r6n-20261003.md` and

  the r6n media analysis.

  Tap-selected focus requires actual ROI statistics and coordinate mapping;

  current region counts remain zero. See `camera-controls-20261003.md`.

- [ ] **E-ink stock-parity speed and clean updates.** User still sees slow

  changes, intermediate irregular black bars and occasional clock ghosting.

  r6n includes authoritative WindowManager animation readiness, conservative

  hidden-layer culling, exact pixel damage and stage/damage timing logs.

  Measure verified Fastest separately from Auto; r6m's captured mode was Auto.

  Stock burst power retention/scanout overlap are follow-on experiments, not

  included electrical changes or a promise of 15–60 completed frames/second.

  r6n: theme/animation readiness succeeds, but four of six screen switches hit

  a three-second fresh-frame timeout. Warm Auto updates finish in about 956 ms.

  Verify Fastest separately. See the latest r6n investigation report.

- [ ] **Sleep/wake stability.** Waking passed on r6n, but Pierre reports an

  automatic reboot when putting r6t to sleep. Live logs stop at s2idle entry

  at 08:32:38 on 4 October; USB is absent. Recovery evidence is pending.

- [ ] **USB across suspend/resume.** Live USB ended on entry to s2idle at

  12:05:46 Paris; no successful USB resume was captured before recovery.

  This alone does not establish a freeze or contradict the user's wake pass.

- [ ] **Media and call audio.** Recording signal exists, but playback and

  both call directions are silent; Android UI sounds are audible again on r6n.

  Original WAVs and videos are copied to Desktop/A6L-media-r6n-20261003. The

  newest voice WAV contains substantial signal; listening must establish

  intelligibility. Both videos have AAC tracks with very low recorded levels.

  On 4 October Pierre clarifies that previous recordings were quiet and choppy.

  This remains unresolved: r6s/r6t include no audio-path correction. Restored

  UI effects do not establish usable microphone capture or recording playback.

  The 4 October offline audio investigation finds 17.1–19.3% exact silence in

  three original Recorder WAVs, with repeated roughly 85 ms gaps matching the

  HAL's 4096-frame capture-silence warnings. A bounded native-pipe wait candidate

  is isolated for review; it is not integrated or physically tested. Separate

  playback HAL write failures match DSP `q6asm` queue errors (`-EBUSY`). Quiet

  capture levels, playback routing and call audio remain separate open items.

  Active playback PCM/mixer evidence and separate speaker/headphone tests

  remain required. Voice control packaging correction alone is not a fix.

- [ ] **GPU stability.** No garbled text reported on the latest hardware

  rendering builds; long-idle, repeated Camera and E-ink/suspend survival need

  continued checking. Retained GPU PM diagnostic hold remains enabled.



## Current installed build and blocking diagnosis — r6j, 2 October 22:37



- [ ] **GPU stability.** The r6i freeze during rear-screen Settings scrolling

  has captured GPU faults and uncompleted fences. Android rendering/framework

  stall while the native LCD/e-ink switching service continues; button switching

  is not proof of Android responsiveness. CPU/adb and storage remained alive.

  r6j holds GPU runtime power active as a diagnostic comparison, with unchanged

  kernel/module/firmware/Mesa/clocks/voltages. Installed and readback verified;

  first six minutes show active hardware graphics and no recorded GPU hang.

  Pierre's LCD scroll, Camera attempt and rear-screen use remain responsive;

  no garbled text reported. Last log enters s2idle at 616.3 seconds. Overnight

  survival and wake are pending; leave the locked phone untouched.

- [ ] **Display tabs acceptance.** r6i's TabLayout theme exception is fixed in

  the embedded r6j Settings APK. Normal Display intent now opens and resumes

  successfully. Verify all three tabs, scrolling, search and recreation on phone.

  r6j physical opening/scroll pass. Its single E-ink navigation entry is not the

  requested organization. r6k embeds the controls directly and removes that entry.

- [ ] **Camera preview/photo.** Current candidate remains unvalidated on phone;

  Earlier r6i testing was interrupted before Camera opened. In r6j, Camera now

  reaches stream configuration and then the provider aborts cloning request

  metadata. The old metadata schema reproduces that failure; Android 17 schema

  alignment passes 24 regressions and is integrated into r6k. Preview/photo still

  need physical validation.

- [ ] **Rear-screen usability and appearance.** Mode-preserving cleanup and

  independent Light/Dark/Follow LCD are now installed in r6j. Physical refresh,

  side-bar removal, ghosting and LCD appearance restoration still need checks.

  White/custom rear wallpaper is still unimplemented. Earlier candidate-only

  notes below describe historical build stages.

  r6j physical feedback: side bars gone, full surface, 3–4 second page changes;

  independent theme works but follows screen illumination by about one second.

  r6k prepares white/LCD wallpaper rendering and pre-light theme/fresh-buffer

  readiness, plus output-equivalent ARGB composition/dither optimizations.

  Actual rear latency and first visible frame remain to test. Custom rear image

  selection is still pending.



## E-ink experience requirements — 2 October 2026



- [ ] **Stock-parity interaction and refresh sequence.** Pierre compared with

  stock A6L: switching apps takes roughly 1 second or less. The short flash

  appears partial/inverted rather than repeated full white/black pages; the

  visual waveform identification is uncertain. Trace selected waveform, forced

  refresh, INIT, screen entry and mode transitions separately. Full white/black

  cleanup must not be the normal interactive update path. Compare first entry,

  repeated app changes, scrolling and ghosting under the same settings.

  Stock-library tests now identify mode-preserving forced redraw: cleanup plus

  next Fastest page uses 49 rather than 118 drive frames at 25 C. A paired

  driver/mirror candidate compiles and passes 18 host suites, but is not

  installed. Hardware latency, idle-frame overhead and ghosting remain open.

- [ ] **Independent e-ink wallpaper.** Default rear-screen wallpaper is plain

  white. LCD retains its selected wallpaper. Add a setting to choose a separate

  e-ink image and return to white. Preserve the original wallpaper, crop, lock

  behavior and live-wallpaper component across switches, crashes and reboot;

  avoid rewriting the user wallpaper on every display-state poll.

- [ ] **Independent e-ink appearance.** Rear screen defaults to light mode,

  with Light / Dark / Follow LCD choices. Preserve LCD dark-mode preference

  and automatic schedule. Apply a temporary rendering/theme override while

  e-ink is active instead of permanently changing LCD preferences. Apps with

  their own explicit themes need separate verification.

  Light/Dark/Follow LCD candidate now compiles; 31 controller regressions and

  40 app checks pass. It is not installed. A targeted battery-saver exception

  for a DAY overlay passes patch applicability and 24 actual-source cases;

  framework compilation/actual-image checks now pass in r6i; physical

  validation remains open. See

  `docs/eink-appearance-20261002.md`.



These appearance features are requested after r6h installation and are not

present in r6h. Deep refresh investigation continues in

`docs/eink-investigation-20261002.md`; physical r6h refresh comparison remains

pending. Do not call sub-second stock parity confirmed from source tests alone.



## Display organization — 2 October 2026



- [ ] **General / LCD / E-ink tabs at the top of Display settings.** r6i

  candidate organizes shared text size, rotation and lock/gesture controls in

  General; LCD brightness/color/appearance controls in LCD; detailed rear

  refresh/frontlight/appearance entry in E-ink. Existing controllers and

  availability remain intact. Search must select the correct tab, selected tab

  must survive rotation/recreation, and text/touch accessibility must remain

  usable. Lightweight real-helper tree checks pass 25 cases; actual Settings

  compilation and final APK resource checks pass. Rendered UI and physical

  acceptance remain pending.



## Development update backup policy — 2 October 2026



Pierre requests no routine snapshots of old development ROM partitions.

Retain the verified stock restore and previous development image kits;

reflash a known development image while preserving data when needed.

The current r6f updater completed its default r6e backup before this

clarification. After successful installation/readback, retire development

snapshot payloads and keep manifests/reports. Development updates now default

to checking installed-image heads using hashes instead of saving full partitions,

while retaining device identity, protected-region and written-image verification.

All 17 updater regression cases pass, including the adb hash-only path,

unknown-image refusal and refusal to use a hash-only record as a rollback

snapshot. Verify the staged r6g kit selects this policy before flashing.

This supersedes the earlier stock-plus-one-development-snapshot policy.



## Physical feedback — 2 October 2026, installed r6f



### r6g installed later on 2 October



- [x] **r6g SIM and mobile-data retest.** After enabling radio, the modem and

  radio HAL stay running. The earlier cell-identity abort does not recur.

  A separate Phone crash (`UiccController`, one physical-slot entry vs DSDS)

  was resolved for this boot by setting `ro.telephony.sim_slots.count=2`;

  the property is now in next-build source, not the installed r6g images.

  Pierre unlocked the SIM, confirms service and completed a Google search

  over mobile data. Logs show Orange F home-network LTE registration.

  Outgoing call setup also works, but wife/voicemail tests are silent in both

  directions. Recorder shows waveform but silent playback; UI sounds audible.

  Confirmed media speaker-route conflict fixed in next-build source; voice

  silence needs q6voiced diagnostics. SMS, slot2 CARD_IO_ERROR, Wi-Fi MAC and

  suspend remain unverified. GPU READ fault at 661.048449 s then automatic

  reboot; preserve successful data result without claiming whole-phone stability.



- [x] Combined r6g installed, hash-only policy, no development partition

  snapshots. Physical readback passed and protected regions stayed unchanged.

  Boot completed at 71.919405 s, FD512 hardware graphics, `sysmem,noblit`, radio 0.

  Initial capture through about 168 s has no kernel BUG/panic or GPU fault.

- [ ] **r6g Display scrolling triggers GPU write fault and reboot.** Pierre

  reports no garbled text before the crash. Kmsg records a WRITE TRANSLATION

  fault at uptime 222.293810 s, IOVA 0x126bd040; USB then ends, and the phone

  returns with a reset uptime/new transport. The earlier display-fence BUG

  is not repeated. Preserve this partial text improvement without claiming

  the GPU fully fixed. Debug GPU memory lifetime/mapping next. Camera and

  e-ink were subsequently tested at Pierre's request, before recovery.

  Recovery requested for persistent diagnostics.

  See `docs/rom-r6g-20261002.md` for image pins, evidence and test order.

- [ ] **r6g Camera still fails stream preparation.** Rear camera opens but

  Adjusted 1600x1200 NV12 plus mapped JPEG is still rejected by the HAL.

  Pierre reports the app closes with its error message. Installed patch 0012

  did not resolve physical preview/capture; investigate the remaining rejected

  configuration fields before claiming success.

- [ ] **r6g e-ink remains unusably slow.** With hardware graphics and Fastest

  mode, Pierre reports possible improvement but still very slow updates;

  white side bars persist. Exact latency was not measured. This is a failed

  usability result, not confirmation of the refresh fixes. Logs end at s2idle

  entry at 832.596638 s; no new GPU fault was captured during these tests.

- [x] Future prep now preserves unchanged device-source/configuration timestamps

  and skips redundant line-ending rewrites; fixture and shell-syntax checks pass.

  The global incremental-cache invalidation that caused 226,000 tasks is still

  unproven; do not claim it fully resolved. The completed r6g images were not

  changed by this preparation-tool update.



- [ ] **Prioritize hardware graphics, then one combined build.** Pierre

  requests bundling the prepared camera/radio fixes with confirmed GPU work

  to reduce flashes. r6g is now installed with the display-fence kernel fix, three

  Mesa backports and tested `sysmem,noblit` copy fallback, camera stream fix,

  and radio cell-identity guard. Physical UI/stability results remain pending.

  See `docs/rom-r6g-20261002.md` and the GPU investigation.

- [ ] **Confirmed display-fence BUG in r6f.** At uptime 53.395118 s, the

  fresh Android comparison crashed in `drm_crtc_fence_get_timeline_name`,

  at the exact check removed by upstream 57acb869490b. New Mesa libraries had

  not run in Android. Exact patch applied; kernel compiles with unchanged

  configuration/module ABI. Include in r6g, then retest stable boot and UI.

  This explains the captured crash; other historical freezes remain unresolved.

- [ ] **Shader precision comparison did not fix text.** Pierre reports very

  responsive hardware UI with `sysmem,nofp16`, but glyph corruption persists

  in changing locations. Online research found September a5xx format-copy and

  stencil-copy fixes absent from the installed March Mesa snapshot; isolate

  the custom blitter next, then review targeted backports. No matching Android

  UI root cause is proven yet.

- [ ] **GPU copy regression reproduced off-screen.** In recovery, the original

  driver fails 144 of 338 framebuffer/texture-copy cases; three reviewed Mesa

  backports reduce failures to 18. `sysmem,noblit` passes 338/338 with both

  drivers while retaining FD512 hardware rendering. glFinish does not repair

  the remaining 2D-copy failures. Recovery uses its older compatible kernel

  modules; verify the same probe under Android's actual kernel, then attend a

  Display/expanding-clock comparison with `sysmem,noblit` and default precision.

  This is a copy workaround, not a confirmed UI or freeze fix. Both patched

  Mesa ABIs compile; neither is installed. See GPU investigation evidence.

- [ ] **Android radio HAL aborts on unset cell identity.** SIM inserted;

  modem firmware stayed up for about 400 recorded seconds and corrected rmtfs

  backing paths worked, but the HAL repeatedly aborts in `makeCellInfo` on

  `CellIdentity::noinit`. Empty-cell-list guard compiled; 10 old/new regression

  cases pass. Fix is prepared for next ROM, not installed. Registration and

  calls remain untested. See `docs/radio-cellinfo-fix-20261002.md`.

- [ ] **Slow wake and USB lost after suspend with radio enabled.** Pierre

  reports about 20 seconds to wake, then responsive but very laggy UI. Laptop

  sees no USB device after cable reconnect. Preserve this as a separate issue

  from a complete UI freeze; recorded kernel stream ends during s2idle entry.

- [ ] **WLAN MAC assignment fails during radio startup.** Property reports

  `ready:mac-failed`; investigate before validating Android Wi-Fi.

- [x] Installation/readback and protected-region checks passed; physical boot

  completed at 76.70 s. Original screen geometry, Mesa FD512 with sysmem,

  radio disabled. Settings scrolling remains responsive. At uptime 658.80 s,

  adb was responsive and the continuous kernel log had no GPU hang/fault/panic

  matches. This is a short baseline, not a long-term stability result.

- [ ] **Garbled text persists with sysmem.** Screenshot shows missing parts

  of glyphs in quick settings and notification text. Pierre reports the

  expanding clock looks correct at its initial size, then becomes garbled at

  some intermediate sizes as he drags the shade open. The affected regions

  appear random. Reproduce across animation positions and compare controlled

  renderer/debug options; investigate scaling/font atlas/shader/texture

  handling without assuming which stage is faulty. Evidence:

  `logs/r6f-install-20261002/settings-cropped-20261002.png`.

- [ ] Physically validate the r6f camera preview/capture metadata fix, e-ink

  fast-mode latency/ghosting fixes, then modem startup as separate tests.

- [ ] **Camera stream preparation fails under software rendering.** Aperture

  passes its splash, opens rear camera 0, then closes with the stream-preparation

  error. HAL rejects Adjusted 1600x1200 NV12 + mapped JPEG configuration.

  Patch 0012 allows internal allocation adjustments while preserving requested

  image requirements; 11 regression cases, Android compile and IPA signature

  checks pass. Prebuilts staged for next ROM, not installed. Physical preview

  remains unverified. See `docs/camera-streamfix-20261002.md`.

- [ ] **r6f e-ink speed test inconclusive under CPU rendering.** Pierre reports

  each UI swipe already takes about two seconds with SwiftShader, making panel

  responsiveness hard to judge; result does not look good. Fastest/mirror was

  verified in properties. Do not count the refresh fixes as physically proven.

  White side bars remain; full-area layout switching was not included in r6f.

- [ ] **GPU stability remains a blocker despite sysmem.** Opening Aperture

  triggered ringbuffer drain timeout and repeated a5xx GPU fault/hangcheck

  recovery at uptime 659.94 s, before any camera device opened. App splash,

  SystemUI and launcher stalled; screenshot timed out, adb shell stayed alive.

  Live interface restart could not complete (SurfaceFlinger zombie with a

  remaining RenderEngine thread). Reboot requested for software-renderer

  isolation. Do not classify this as a failed camera-preview metadata test.

  Evidence: `logs/r6f-install-20261002/camera-stuck-20261002T070616Z/`.



## Physical feedback — 1 October 2026, installed r6e



This update takes precedence over the historical 25 September status below.

The phone uses Mesa with the temporary sysmem workaround; radio is disabled.



- [x] Pierre confirms the e-ink button and Power button work for switching

  between the LCD and e-ink sides. The e-ink Settings page opens.

- [x] Rear touch works in Android. This confirms basic input, not complete

  edge/corner calibration or acceptable interaction latency.

- [ ] **E-ink refresh speed is a usability blocker.** Pierre reports about

  six seconds from pressing the e-ink button to seeing the image: initially

  about two seconds with no visible response, then white → black → white →

  black flashing, followed by the image. He described the flashing screen as

  the LCD; verify which panel flashes during the recorded reproduction.

  Subsequent interaction is roughly one picture every six seconds, also

  reported when trying the fastest mode. Touch works but the delay makes

  normal use impractical. Measure key-to-switch, capture/conversion, queued

  clear, waveform generation and panel-drive times separately. Check the

  actual selected waveform and Settings mode mapping. The mirror currently

  forces clear + quality redraw on entry; fast/fastest still use quality on

  settled changes. Investigate those policies and avoid redundant clears;

  do not attribute the entire delay to expected e-paper behavior or CPU speed

  without measurements. Acceptance: an attended switching, tapping and

  scrolling test shows a major improvement over the observed six-second cycle,

  and fastest-mode updates use the intended fast waveform.

- [ ] **Use the full e-ink screen area.** Pierre sees white bars on both sides

  and wants the whole surface used. Source currently letterboxes the

  1080×2340 LCD image into 720×1440, producing about 27–28 px side bars.

  This is the current scaling policy, not yet a hardware fault. Evaluate an

  Android layout with the e-ink aspect ratio when switching sides, alongside

  corresponding capture geometry and rear-touch mapping. Compare with a

  stretch or crop option: stretching distorts content, while cropping may

  hide controls. Pierre has not selected an implementation. Acceptance:

  full-area image with accessible status/navigation/app controls, correct

  edge/corner touch, and normal LCD geometry after switching back.



Read-only snapshot after this feedback: refresh property was `fast`, active

side `lcd`, e-ink state `off`. This later snapshot does not establish which

mode was active during Pierre's fastest-mode test. No mode was changed by

the agent. The final r6f rebuild now also includes the tested refresh-policy,

event-loop and conversion fixes described in

[the investigation](eink-runtime-fixes-20261001.md). Phone CPU benchmarks pass,

but improved panel latency and ghosting remain unverified after installation.

The temporary 1080×2160 test confirmed a centred scanout viewport, but Pierre

reported a freeze while navigating to Display settings. The automatic reset

could not reach adb shell; after recovery, original geometry was restored

and verified in Android using software rendering. Full-area switching remains unvalidated, and sysmem

does not establish graphics stability. See the investigation for exact geometry.



---



Agent `todo`, offline review only: nobody touched the phone. Sources: every project doc (claude/*.md up to

attended-20260925), the repo docs (port-status, all docs/*-2026092[345].md, flash/hals/rest/realinit/roadmap/hardware

readiness), /home/claude/overnight-status.md, and three read-only checks made today: (a) `device/hisense/a6l/rom/`

and `lineage_gsi_a6l.mk`, to see what is actually integrated in the ROM; (b) a read-only listing of the laptop

`~/A6L-usb-20260915/` (relay `todo-02`); (c) `git status` of the repo.



**Goal definition ("daily driver").** The phone is installed on the eMMC and boots by itself. It charges and sleeps

safely and lasts at least a day. It makes and receives calls with audible audio, and the screen goes off at the ear.

SMS works, Wi-Fi works (with WPA), and Bluetooth audio works. GPS gets a fix. Mobile data works, or Pierre explicitly

accepts Wi-Fi-only. At least the main camera works. Rotation and auto-brightness work. The e-ink is usable from Android.

The phone can be updated and rolled back to stock. SELinux runs enforcing.



Status key: **P** = proven on the phone (Pierre saw it, or a read-back proved it). **P~** = partly proven, with a known

gap left. **B** = built and checked offline, but never run on the phone. **D** = designed or documented only, nothing

built. **N** = not started by anyone. "DD" is whether the item is needed for a daily driver: **Y** = required,

**R** = strongly recommended, **O** = optional.



---



## 0. Headline findings (read these first)



1. **The installable ROM (rom-v1 kit-r8) contains almost none of the 24 to 25 Sep work.** `rom.mk` inherits only

   `gnss.mk` and the dalvik heap config. The `full` variant (`rom/full/full.mk`) adds audio3, the Wi-Fi/BT/lights/power/

   thermal HALs and nothing else. The following are **not integrated anywhere**: the radio HAL (`radio.mk`),

   eink3 (`eink.mk` and the composer patch), the sensors multihal, the USB gadget/MTP HAL, the vibrator HAL, q6voice

   and a6l-q6voiced, the audio4 q6routing fix, the speaker, the camera patches, haptics, hall, the charger, flash LED,

   cpufreq, IPA and the USB watchdog. The kernel/DT is still the **V74 base DT without the speaker, charger, camera,

   haptics or hall overlays**. Nobody owns a "rom-v2 integration" (one V75 kernel + DT + vendor). That integration is

   the first item on the critical path.

2. **The rom-v1 kit is NOT on the laptop.** Checked today: `~/A6L-usb-20260915/rom-v1` does not exist. kit-r8 exists only

   in WSL at `/home/a6l/rom-v1/kit-r8/rom-v1`.

3. **There is no proven rollback path from an installed ROM.** Once system is replaced, stock cannot boot, so there is no

   `adb reboot edl`. Our kernel has no `reboot edl`, and the ABL has no known EDL command. The flash doc's "Plan B"

   (dd from the V74 recovery over adb) is untested, and the hardware EDL entry method is undocumented. **Before the first

   install, prove one way back into EDL, or test Plan B.**

4. **Charging under rom-v1 is unverified.** The charger overlay (`a6l-charger-v75`) is not in the rom-v1 DT. An

   installed phone might not charge, or might charge only at the PMIC hardware default. This must be checked in the

   first installed boot, and the phone must not be left on the ROM for long until it is proven.

5. **The in-call proximity sensor is missing.** The FRONT sensor is the STK3338 (`i2c@c1b6000` 0x47), and it has

   **never answered on I2C**. The TMD3702 that works is the REAR sensor. Without the front sensor, the screen stays on at

   the ear during calls, and front-side auto-brightness has no input. Nobody is working on this.

6. **The earpiece is physically broken on the test unit.** Handset-mode calls cannot be validated on this phone.

   Calls need the headset, or the speaker once the TFA9894 driver works.

7. **No VoLTE/IMS, no USSD, no call waiting/forwarding, no MMS.** Nobody has started any of these (the RIL doc lists them

   as unsupported). Calls rely on CSFB to 2G/3G, which is a long-term risk as French operators switch off 2G/3G.

8. **Nobody has started these:** Vulkan (turnip), hardware video codecs (venus + Codec2), Widevine, OTA/updater, a

   LineageOS recovery for installs, release-key signing, a GApps/Play Integrity plan, SELinux enforcing policy

   compilation, USB OTG, the charger (off-mode) boot, the notification LED check, a production kernel config (watchdog,

   `panic=` reboot, debug options off), and a user guide for Pierre.

9. **Repo hygiene.** The last git commit is from **21 Sep**. `git status` shows 153 changed or untracked entries, which is

   all the work of 22 to 25 Sep (eink, hals, radio, gnss, audio, camera, rom, …). One disk loss would erase four days.

   Commit it soon (Pierre, or the main agent from the sandbox shell with `-c user.name=Pierre`). Note: my read-only

   `git status` left an empty `.git/index.lock`, because the sandbox cannot delete files. I removed it through relay

   `todo-01-unlock` (REMOVED_EMPTY_LOCK). **Other agents: do not run git in the sandbox shell.**

10. `docs/port-status.md` is stale (23 Sep). It is owned by the main agent and should be refreshed from this file.



---



## 1. Critical path to a daily driver



```

[A] rom-v2 integration (flash agent owns it: V75 kernel + DT + vendor with all deliveries)  ── 3–5 days

      │

[B] Prove rollback (EDL entry or V74 Plan-B dd) + stage kit on laptop                    ── 0.5 day attended

      │

[C] First install on eMMC: boot, /data, USB, CHARGING, battery %, suspend/resume, e-ink    ── 1–2 attended sessions

      │

[D] Power basics: charger overlay + SW JEITA, s2idle + wake sources, thermal HAL,

      cpufreq (CPR/OSM open-loop, attended)                                                ── 1–2 weeks

      │

[E] Telephony in the ROM: RIL HAL on the real modem → SIM/SMS/calls; q6voice + q6voiced

      + in-call routing; FRONT proximity (STK3338); speaker (TFA9894) for handsfree        ── 2–3 weeks

      │

[F] Wi-Fi WPA association + MAC; Bluetooth HAL + bdaddr + A2DP; GNSS fix                  ── 1 week (parallel to E)

      │

[G] Mobile data (ipa2_lite → rmnet/QMAP → RIL data call → netd)                           ── 2–4 weeks, may fail

      │

[H] Cameras: frames to DDR → libcamera (SoftISP) Android HAL → CameraX apps              ── 3–6 weeks

      │

[I] Release: SELinux enforcing, release keys, recovery + OTA path, user guide              ── 2–3 weeks

```

The longest and most uncertain items are G (IPA v2.6L) and H (camera HAL). The rest is integration and validation.

Realistic total if everything goes well: **6–10 weeks** of combined agent and attended time. A usable "Wi-Fi + calls +

SMS" build is possible after about A–F (**3–4 weeks**).



---



## 2. Hardware



| # | Item | Status | Evidence | Next concrete step | Estimate | Risk | DD |

|---|---|---|---|---|---|---|---|

| H1 | LCD (FT8719, DPU/DSI0) | **P** | attended-session-results-20260921-v71.md | In the installed ROM: blank/unblank, resume after suspend, find the backlight sysfs name for the lights HAL (`ls /sys/class/backlight`) | 1 day | low | Y |

| H2 | GPU GLES (Adreno 512, Mesa freedreno) | **P** from RAM (21 Sep) | attended-session-results-20260921.md | Confirm in the installed ROM (`persist.graphics.egl=mesa`); GPU devfreq/power | 1–2 days | low | Y |

| H3 | Vulkan (turnip) | **N** | (none) | Cross-build Mesa turnip for Android (NDK), `ro.hardware.vulkan=freedreno`; check that HWUI/SF stay on GL if it is unstable | 3–5 days | med | R (many apps, and ANGLE-on-Vulkan paths) |

| H4 | Rear e-ink panel (TC358767 bridge, TPS65185, a6l_epdd) | **P** (kernel + v2 clear, 23 Sep) | claude/fresh-eye-eink-bridge, eink-clear-prep | none at the panel level; see S5 for Android | — | low | Y (it's the point of the phone) |

| H5 | E-ink frontlight | **D**/unknown | attended-session-results-20260922-eink-bridge.md (candidate `a6l-eink-frontlight.dtso`, LPG ch4); hals doc: "none known" | **Ask Pierre whether the stock rear screen has a frontlight.** If it does: V75 DT + `leds-qcom-lpg.ko` + a sysfs control in the e-ink settings | 1–2 days | low | O/Y depending on the hardware |

| H6 | Front touch | **P** | android-input-v47 | Check it in the installed ROM; wake/suspend behaviour | 0.5 day | low | Y |

| H7 | Rear touch | **P** (events) / **B** mapping | eink3-20260924.md | E8/R5 in eink3 §7B: orientation property, uinput → InputReader | 1 day | med | Y |

| H8 | Keys: power, vol +/-, e-ink key 616 | **P** (evdev) / **B** (Android) | controls-v46, eink3 | ROM: volume UI, power-key wake from suspend, e-ink key handled by a6l_eink_mirror (short = mirror, long = clear) | 0.5 day | low | Y |

| H9 | Vibration (PM660 LRA) | **P~** failed: nothing felt (24 Sep) → **B** rest2 | misc-20260924.md §3, rest-20260924.md §2 | Attended rest2 MODE=haptics (ilim 800, SC debounce, auto-res, sweep). Then add the VibratorOL HAL (`vendor/qcom/opensource/vibrator`) to the ROM | 1 day + HAL 0.5 day | med (actuator fault possible) | Y |

| H10 | Loudspeaker (TFA9894 amp) | **B** (driver, stub DAI, v75 overlays); never played | audio3-20260924.md | V75 DT with the speaker overlay → the amp probes → the first tone at low level. **Calibration/profile (the stock tfa container) must be reviewed to avoid damaging the speaker** | 3–5 days | **high** (speaker damage, card-wide probe dependency) | Y (ringtone, handsfree) |

| H11 | Earpiece | **broken on this unit** | attended 23 Sep | Codec EAR path in mixer_paths only (untestable here); verify on another unit, or leave it | — | n/a | Y on a healthy unit |

| H12 | Headset out + jack detection | **P** (detect; call downlink) / **P~** media (ADM 0x10325 error) | attended-20260924, audfix | audio4 MODE=tone/media (q6routing per-direction patch) | attended 1 h | med | Y |

| H13 | Microphones (main, secondary, headset) | **P~** uplink proven in a call (probably the headset mic, 24 Sep) | attended-20260924 | audio4 MODE=media capture; test the main mic without a headset; the secondary mic/ECNS has no ACDB | 1–2 days | med | Y |

| H14 | Headset button (hook/media keys) | **N** | (none) | Check the codec MBHC button kcontrols/input events; map KEY_MEDIA | 0.5–1 day | low | R |

| H15 | Call audio (q6voice / CVD) | **P** in recovery (legacy CVD=0, headset, both ways, 24 Sep) | attended-20260924, kvoice-20260924.md | audio4 MODE=call GAIN=0/3/6; then **ROM integration**: q6voice patch in the ROM kernel, voice DT, `a6l-q6voiced`, RIL sets `vendor.a6l.voice.active`, audio HAL in-call routing. No EC/NS (no ACDB) | 1 week | med | Y |

| H16 | Modem (MPSS boot, rmtfs, diag-router over QRTR) | **P** (stable, 24 Sep) | morning-20260924, attended-20260924 | ROM `a6l-radio.sh` on the real eMMC (EFS read-only by default) → 30 min stable; then decide rmtfs_rw | 1 attended session | med | Y |

| H17 | SIM / registration | **P** (CLI: PIN, online, LTE Orange F) | attended-20260924 | Radio HAL V4 on the real modem in the ROM (never talked to a modem): SIM status, PIN UI, signal bars, NITZ time | 3–5 days attended | med | Y |

| H18 | SMS | **P** (CLI send + receive) / **B** decode fix | attended-20260924, misc §1 | ril2 sms-listen (bare TPDU decode); then SMS in the Messaging app through the HAL | 2 days | low | Y |

| H19 | MMS | **N** | (none) | Needs mobile data (or a Wi-Fi-calling-free path: no) → after G | after G | high (depends on IPA) | R |

| H20 | Voice calls: outgoing | **P** (CLI dial to voicemail) | attended-20260924 | ril2 dial-dtmf; then calls from the Dialer through the HAL | 2–3 days | med | Y |

| H21 | Voice calls: **incoming/ringing** | **N** on the phone (HAL code exists, callRing) | ril-20260924.md | Attended: CLI `call-wait`/answer, then the HAL (ring, answer, reject). Needs a ringtone → speaker (H10) | 2 days | med | Y |

| H22 | DTMF | **B** (ril2) | misc §1 | ril2 dial-dtmf `1234#` | attended 10 min | low | Y (IVRs, voicemail) |

| H23 | Emergency calls | **B** in the HAL (emergency dial + 112/911 list); **cannot be tested live** | ril-20260924.md | Offline review of the emergency flow (no SIM / locked SIM / no service); never dial 112 for a test. Check what the modem does with an emergency number without a SIM | 1 day review | **high** (safety) | Y |

| H24 | USSD, call waiting/forwarding, CLIR, network scan, cell broadcast | **N** ("not supported" in ril doc) | ril-20260924.md §limits | Implement VOICE USSD (orig_ussd / ussd ind) first (prepaid balance), then SUPS | 3–5 days | med | R (USSD), O (the rest) |

| H25 | VoLTE / IMS / VoWiFi | **N** (nobody) | ril doc: "IMS/VoLTE not supported" | Research only: the stock IMS stack is proprietary (QTI ims + DPL). An open IMS client (e.g. the Doubango-based ones) is a multi-month project. Plan: rely on CSFB and **check the Orange F 2G/3G switch-off dates** | months | **high** (network sunset) | R (long-term), not for v1 |

| H26 | Mobile data (IPA v2.6L + rmnet + WDS) | **B** (ipa2_lite resets the phone at load → ipa2b step-logged) | ipa-20260924, ipa2fix-20260924, ipa-sdm660-plan | ipa2b STOP_AT walk with `dmesg -w` streamed → fix → MODE=status/data → RIL setupDataCall → netd routes/DNS | 2–4 weeks | **high** (upstream v2.6L never passed packets) | Y (or Pierre accepts Wi-Fi-only) |

| H27 | Wi-Fi (WCN3990/ath10k) | **P~** scan only (4 networks, 23 Sep). **Association/WPA never tried** | prep-20260923, hals §Wi-Fi | ROM full variant: Wi-Fi HAL (the fallback-legacy-HAL question is open; plan B goldfish `libwifi-hal-emu`) + wpa_supplicant → connect to Pierre's Livebox (WPA2), DHCP, DNS, sustained throughput, reconnect after suspend | 2–4 days | med | Y |

| H28 | Wi-Fi MAC | **B** (a6l_macs; the real MAC 7c:b3:7b:99:40:46 is in persist) | hals §Wi-Fi | Attended `dmesg | grep A6L_MACS`; the permanent MAC stays random (framework randomisation is fine) | 0.5 day | low | R |

| H29 | Wi-Fi hotspot/tethering, P2P | **N** | hals §3 | hostapd packaged; test after H27 | 1–2 days | med | O |

| H30 | Bluetooth (hci0, WCN3990 UART) | **P** hci0 (23 Sep, serial1 DT) / **B** HAL + bdaddr | hals §Bluetooth | ROM full variant: `a6l_macs bt` (hci0 UNCONFIGURED finding) → HAL `hci interface 0 found` → pair a device | 1–2 days | med | Y |

| H31 | BT audio: A2DP / HFP (SCO) | **N** | hals: "A2DP/SCO offload not wired" | A2DP: AOSP software encoding through the audio HAL's `/bluetooth` module (the AIDL example HAL registers it) → test with headphones. HFP/SCO: WCN3990 SCO is routed to the SoC PCM/SLIMbus on stock; on mainline you have to **check whether SCO-over-HCI (UART) works** (a vendor command may be needed) | A2DP 2–3 days; HFP 1–2 weeks | med / **high** (HFP) | Y (A2DP), R (HFP) |

| H32 | GNSS | **P~** (LOC engine on, 7 SVs, max 32 dB-Hz, no fix in 600 s) / **B** gnss2 XTRA + HAL | attended-20260924, gnss-20260924, misc §2 | gnss2 XTRA + coarse position outside with open sky; compare C/N0 with Pierre's everyday A6L; then the HAL in the ROM (a map app) | 1–3 days | med (antenna/LNA config unknown) | Y |

| H33 | Main camera IMX576 + GT9769 VCM | **P~** (chip-ID, sensor streams at frame counter 6→157, no frame in DDR; stream-off oops) / **B** camera3 | attended-20260925, camfix2-20260925 | camera3 bars (oops fix + ICC vote + VFE diagnostics) → first raw frame → AF sweep | 1–2 weeks to stable frames | high | Y (at least the main camera) |

| H34 | Front camera S5K3T1 | **P~** chip-ID / **B** own driver (585-reg init) | cam-20260924, camfix | camera3 bars SENSOR=s5k3t1 after IMX576 works | 1 week | high | R |

| H35 | Aux camera Hi-846 | **P~** chip-ID / **B** (upstream drv, 4-lane patch) | camfix | camera3 bars SENSOR=hi846 | 3–5 days | med | O |

| H36 | Camera Android HAL (libcamera + SoftISP) | **D** (camera_hal.yaml, package list; libcamera ≥ 0.7 **not built**) | cam-20260924 | Cross-build upstream libcamera with the Android HAL + simple pipeline + SoftISP; tuning (no chromatix CCM/LSC decoded); CameraX/Camera2 app test | 2–4 weeks | **high** (quality, performance without cpufreq) | Y |

| H37 | Flash / torch LED | **B** (`a6l-flash-v75.dtso`, leds-qcom-flash) | rest-20260924 | V75 DT → `echo 1 > …/brightness` → torch HAL (Lineage flashlight via camera HAL or `torch` sysfs) | 1 day | low | R |

| H38 | Motion sensors (accel/gyro/mag via SMGR) | **P** (IIO devices) | attended-session-results-20260921-v71 | Sensors multihal + trout IIO sub-HAL (in no variant yet): verify the IIO names, the XML schema, axis signs, mag support → auto-rotate | 3–5 days | med | Y |

| H39 | Rear ALS/prox TMD3702 | **P** (ALS + prox with stock regs, 23 Sep) | prep-20260923 (als2) | Buffered IIO/hrtimer trigger (not done) or its own sub-HAL; useful for e-ink policy | 2 days | low | O |

| H40 | **Front ALS/prox STK3338** | **P~ FAILED**: never answers at 0x47 | hardware-readiness-20260920, port-status | **Nobody is working on it.** Check the stock DT for the supply/reset/IRQ (vdd, vio, any enable GPIO), probe with the rails on, and try `stk3310.ko` (ID acceptance). Needed for in-call screen-off and auto-brightness | 2–4 days | med | **Y** |

| H41 | Hall sensor (gpio75, flip cover) | **P~** stuck high / **B** rest2 hall | misc §4 | rest2 MODE=hall with a strong magnet / the flip cover; the Android lid RRO is optional | 0.5 day | low | O |

| H42 | Step counter/significant motion (SSC virtual sensors) | **N** | hardware-readiness (stock fusion from the ADSP) | Skip; Android derives what it needs; SMGR fusion ports are out of scope | — | low | O |

| H43 | Fingerprint (Sunwave, QSEE TA) | **dropped** by Pierre | rest-20260924 §4 | none | — | — | no |

| H44 | NFC | **not fitted** | rest §7 | none | — | — | no |

| H45 | USB device: ADB | **P** (recovery; enumeration flaky after warm reboot) | usbfix-20260925 | V75-usb watchdog (optional); ROM `init.a6l.usb.rc` `soft_connect` gate on the real eMMC | 1 attended session | med | Y |

| H46 | USB MTP / file transfer | **B** (gadget HAL fork, **not in any variant**: it conflicts with the a6l_manual_usb gate) | hals §USB, full.mk comment | Merge the gadget HAL with the soft_connect gate (or drop `a6l_manual_usb=1` in the ROM); test "File transfer" | 1–2 days | med | Y |

| H47 | USB OTG / host | **N** | rest §7 | PM660 Type-C detection + OTG VBUS + dwc3 role switch driver | 2–3 days | med | O |

| H48 | Charging (PM660 SMB2) | **B** (`a6l-charger-v75`, 1.95 A, no JEITA); **not in the rom-v1 DT** | rest §6 | Attended C2 (current-affecting). Then **software JEITA** (thermal trip or qcom_smbx patch) before unattended charging | 2–3 days | **high** (battery safety) | **Y** |

| H49 | Off-mode charging (plugged in while off) | **N** | (none) | Check what the ABL does (`androidboot.mode=charger`?) with our boot image; Lineage charger or just boot fully | 1–2 days | med | Y |

| H50 | Fuel gauge / battery % | **P** (readings, V45 and 21 Sep) | port-status | Health HAL in the ROM: % matches sysfs; the capacity-learning accuracy of pmi8998_fg on PM660 | 1 day | low | Y |

| H51 | Thermal | **B** (tsens zones present; the thermal HAL reports fake values) | rest §6 | The thermal HAL reads the real zones; battery-temperature trip; CPU cooling needs cpufreq | 1–2 days | med | Y |

| H52 | CPU frequency (CPR3/CPRh + OSM) | **B** (ported, Image.gz built, open-loop, capped 1536/1747 MHz) | rest §1, cpufreq-assessment-20260920 | **Do not use MODE=fuses** (it reset the phone on 24 Sep). Take the CPR fuse values from stock dumps (root on stock: `/sys/kernel/debug` or the stock kernel's CPR logs) → attended RAM boot, Pierre only | 3–5 days attended | **high** (voltage) | R (performance, thermal, battery) |

| H53 | Suspend (s2idle) + wake sources | **N** on the phone (QEMU only) | rest §6, flash §9 | Installed ROM: screen off → `cat /sys/power/suspend_stats`, wake by power key, alarm (pm8xxx RTC), incoming SMS/call (modem SMP2P/GLINK), charger plug | 1 week | high | **Y** |

| H54 | RPM deep sleep (XO/CX shutdown), battery life | **N** | rest §6 | After H53: measure the drain (overnight) and find the votes that block XO shutdown | 1–2 weeks | high | Y |

| H55 | Video codecs (venus) + Codec2 HAL | **N** | (none) | Check mainline venus support for SDM660 (HFI 4xx), the firmware from stock `venus.mbn`; an Android Codec2 v4l2 HAL (the ChromeOS v4l2_codec2 approach). Until then the software codecs work (CPU heavy) | 2–3 weeks | high | R |

| H56 | DRM / Widevine | **N** | (none) | Lineage has ClearKey only. Widevine L3 needs a prebuilt `drm-service.widevine` blob (a legal/licensing question); L1 is impossible without QSEE | 1 day (L3 blob) | med | O (streaming apps) |

| H57 | Keymint / Gatekeeper | **B** (software keymint "nonsecure" in the vendor; QEMU) | flash §3, realinit | Installed ROM: set a PIN, reboot, unlock; the Keystore works. No hardware-backed keys (no QSEE) | 1 day | low | Y |

| H58 | Storage /data (ext4, formattable) + /metadata | **B** (QEMU r8) | flash §9 | First install: 107 GiB format, fstrim, performance | 0.5 day | low | Y |

| H59 | Encryption (FBE / metadata encryption) | **N** | flash §2 alt D | fscrypt v2 + metadata encryption with software keymint (`fileencryption=aes-256-xts:aes-256-cts:v2+inlinecrypt_optimized` without inlinecrypt) | 2–3 days | med | R (a lost phone = readable data) |

| H60 | microSD slot | **unknown** (stock DT enables sdhc_2, cd-gpio54; `ro.build.characteristics=nosdcard`) | rest §7 | **Pierre: look in the SIM tray.** If present: overlay + vold adoptable/portable | 0.5 day | low | O |

| H61 | Notification LED | **N** (nobody checked) | (none) | Check the stock DT (qcom,leds / pm660l RGB) and the stock `/sys/class/leds`; if present: lights HAL notification | 0.5 day | low | O |

| H62 | RTC / alarms, NITZ time | **N** | (none) | pm8xxx RTC wakealarm in the installed ROM; NITZ from the RIL | 0.5 day | low | Y |

| H63 | Reboot / power-off / reboot-to-recovery/bootloader/EDL | **P~** (`reboot bootloader` works from the kernel) / `reboot edl` **N** | flash §5 | PON reason for EDL (qcom,pon `reboot-mode` edl = 0x01 on PM660?) → also closes finding 3 | 1 day | med | Y |

| H64 | Hardware watchdog | **N** | port-status ("production config, watchdog") | Enable the APSS WDT (`qcom-wdt`) + Android watchdogd | 0.5 day | low | R |



## 3. Software, system and release



| # | Item | Status | Evidence | Next concrete step | Estimate | Risk | DD |

|---|---|---|---|---|---|---|---|

| S1 | Real Android init + vendor image, boot from the eMMC | **B** (kit-r8: QEMU boot_completed + 11 min stable; captured-ABL emulation PASS) | flash-20260924 §9 | Stage kit-r8 on the laptop → `Verify-RomV1Stage.py` → attended install (after S14) → H1–H10 of flash §7.3 | 1 attended session | med | Y |

| S2 | **rom-v2 integration** (a single V75 kernel + DT + vendor with all the deliveries) | **N** (nobody owns it) | this review §0.1 | Flash agent: V75 kernel = 7.2.3 + q6asm xlate + q6routing per-direction + q6voice + camss patches + haptics + ipa2_lite (if it passes); V75 DT = base + speaker/charger/haptics/hall/flash/camera/voice(/cpufreq off); vendor = radio.mk, eink.mk + composer patch, sensors multihal, gadget HAL, VibratorOL, q6voiced, full variant; QEMU r-run + ABL emulation | 3–5 days | med (every added module can break the boot) | **Y** |

| S3 | Production kernel config / cmdline | **N** | flash §3 (cmdline keeps `a6l_probe=1`, `panic=0`, `loglevel=6`, earlycon, `*_ignore_unused`) | `panic=5`, `loglevel=4`, drop `clk/pd/regulator_ignore_unused` once the drivers vote properly (it matters for sleep!), watchdog, drop `a6l_manual_usb` once USB is reliable | 2–3 days (the ignore_unused removal is iterative) | med | Y |

| S4 | SELinux enforcing | **N** (every fragment is written for permissive; hals sepolicy never compiled; eink sepolicy compiles monolithically) | hals §sepolicy, eink3 | `m selinux_policy` with all the fragments → boot permissive → collect avc denials in the installed ROM → fix → `androidboot.selinux=enforcing` | 1–2 weeks | med | Y (security, some banking apps) |

| S5 | E-ink Android integration (eink3: lease from drm_hwcomposer, a6l_epdd v4, mirror v2, key 616, rear-touch uinput) | **B** (unit 50/50, e2e 14/14; never on msm) | eink3-20260924 | Include it in rom-v2; attended eink3 §7A (RAM) and §7B E1–E14 (installed) | 1 week | med (the lease under a live LCD is unproven) | Y |

| S6 | Dual-screen switching + e-ink settings UI | **in progress** (dualux agent; no doc seen yet) | (dualux) | Deliver: a mode switch (LCD / e-ink / mirror), a Settings tile/page, per-app e-ink, a reading mode; rear-touch face switching and inactive-face rejection | 1–2 weeks | med | Y |

| S7 | HAL inventory (see §3a) | mixed | hals-20260924 | Put every HAL in one variant; `lshal`/`dumpsys` check on the phone | — | — | Y |

| S8 | Audio HAL (AIDL example + a6l-audio-route + mixer paths + policy) | **B** (full variant) | audio3-20260924 | Installed ROM: media playback on the headset, then the speaker; in-call routing hooks (`vendor.a6l.voice.active`) | 3–5 days | med | Y |

| S9 | Telephony framework (the radio HAL declared, carrier config, APNs) | **B** (HAL m rc=0, 105+131 host tests) — **not in the ROM** | ril-20260924, misc | Add radio.mk + BoardConfig-radio.mk to rom-v2; `telephony.*` features; the Orange F APN/carrier config | with S2 + H17 | med | Y |

| S10 | OTA / updater | **N** | (none) | Choose a path: (a) LineageOS non-A/B OTA installed by a recovery that has our kernel, or (b) a laptop EDL "update" tool (the current installer with `--allow-nonstock` + preserve userdata). Lineage Updater needs signed builds + a server; (b) is realistic first | 1–2 weeks | med | R |

| S11 | Recovery able to install (sideload, wipe, OTA) | **N** (the recovery slot holds the V74 diagnostic recovery) | flash §2/§7.4 | Build a Lineage recovery (`recoveryimage`) with the V74 kernel + sdhci-msm + touch/display; keep V74 as a debug image elsewhere | 1 week | med | R |

| S12 | Build signing (release keys, `user`/`userdebug`) | **N** (test-keys userdebug) | (none) | Generate the keys, `sign_target_files_apks`; decide user vs userdebug (adb root is needed during bring-up) | 1–2 days | low | R |

| S13 | GApps / Play Integrity | **N** (no plan) | (none) | Decide: none / MicroG / MindTheGapps (Android 16 arm64). Expectation: with an unlocked bootloader, test-keys and software keymint, Play Integrity "device"/"strong" **fails**; "basic" at best. Banking/wallet apps may refuse. Pierre should know this before choosing the ROM as a daily driver | 1–2 days to integrate | med | Pierre's choice |

| S14 | Backup / restore to stock (EDL, byte-exact) | **B** (Test-RomV1Flash 8/8 on a virtual eMMC; Verify-RomV1Stage PASS) — **kit not on the laptop** | flash §5, §9.4 | Stage kit-r8 → Verify on the laptop; **decide alternative A (full userdata backup, ~1.5 h)**; **prove EDL entry from a non-stock state** (finding 3) | 0.5 day + attended | **high** if the rollback path is untested | Y |

| S15 | Installer + rollback UX (one command each) | **B** (Launch-RomV1Install/Restore) | flash §7 | A dry-run on the real phone = backup-only mode? (not implemented: consider `--backup-only` to rehearse EDL without writes) | 1 day | med | Y |

| S16 | V75-usb recovery (USB enumeration watchdog) | **B** (image sha 8ecb8e9e…; captured-ABL PASS; mock 7/7) — **install tools NOT generated** | usbfix-20260925 | Generate the Prepare/Stage/Run install tools (the V74C pattern) before it can be installed; then sysrq-reboot 3–5× | 1 day + attended | low | O (it makes the bring-up faster) |

| S17 | Persist / modem EFS policy | **B** (persist ro; rmtfs read-only EFS by default) | flash §4 | Decide `rmtfs_rw` after a stable modem in the ROM (the EFS holds the NV/IMS/carrier state) | decision | med | Y |

| S18 | Performance tuning (dex2oat, zram, lmkd props, schedutil) | **N** | (none) | After cpufreq: zram + lmkd props for 4 GB, the dex2oat thread count | 1–2 days | low | R |

| S19 | Documentation for Pierre (install, rollback, enable the radio, e-ink key, known issues, recovery from a hang) | **N** as a user guide (only agent docs) | (none) | Write `docs/user-guide.md` once rom-v2 exists | 1 day | low | Y |

| S20 | Regression matrix / test automation on the phone | **N** | roadmap §4 | A script that checks every A6L_* marker after the boot (a `bugreport`-like dump) | 1–2 days | low | R |

| S21 | Repo: commit 22–25 Sep work; refresh port-status.md | **N** (last commit 21 Sep, 153 entries pending) | `git status` (today) | Pierre / main agent: commit per area; update port-status from this doc | 1 h | **high** (data loss) | Y |

| S22 | Kernel maintenance (7.2.x updates, out-of-tree patches rebased, Lineage monthly merges) | **N** (no process) | (none) | Keep the patches as a series in `device/hisense/a6l/kernel/`, with one rebuild script | ongoing | med | R |



### 3a. HAL list (target rom-v2) and where each one stands



| HAL / service | Source | In the ROM today? | Proven? |

|---|---|---|---|

| composer (hwc3 drm + e-ink ignore/lease patch) | realinit + eink3 patch | hwc3 yes; patch **no** | UI on the phone from RAM (21 Sep), not from the eMMC |

| graphics allocator/mapper (minigbm msm) | realinit | yes | from RAM |

| EGL/GLES Mesa freedreno | V71 payload | yes | from RAM |

| Vulkan | none | no | **N** |

| audio (AIDL example + a6l-audio-route) | audio3 | full variant only | no |

| a6l-q6voiced (voice session) | kvoice | no | CLI equivalent in recovery |

| radio (android.hardware.radio-service.a6l, V4) | ril/misc | **no** | no (the CLI is proven) |

| GNSS (AIDL V7 over QMI LOC) | gnss | yes (base) | QEMU registration only |

| Wi-Fi (android.hardware.wifi-service + wpa_supplicant) | hals | full only | no (legacy HAL fallback question open) |

| Bluetooth (bluetooth-service.default, HCI_CHANNEL_USER) + a6l_macs | hals | full only. The rc starts `/vendor/bin/a6l_macs`, but `full.mk` does not list it in PRODUCT_PACKAGES: check that the binary is really in the vendor image | no |

| Bluetooth audio (A2DP software) | AOSP | unclear | **N** |

| sensors (multihal + trout IIO sub-HAL) | hals | **no** | no |

| lights (lineage) | hals | full only | no |

| vibrator (QTI VibratorOL) | rest | **no** | no |

| health | example | yes | no (battery readings proven in sysfs) |

| power, thermal | example | full only | no (thermal = fake values) |

| USB (usb-service.basic) + USB gadget (a6l fork) | hals | **no** (it conflicts with the manual USB gate) | ADB only |

| keymint / gatekeeper (software) | realinit | yes | QEMU |

| camera (libcamera Android HAL) | cam | **no** (not built) | no |

| drm (clearkey), Widevine | AOSP / none | clearkey only | — |

| memtrack, PowerStats | none | no | noise only |

| e-ink: vendor.a6l_epdd, vendor.a6l_eink | eink3 | **no** (only the manual `/vendor/a6l/epd` payload) | v2/v3 from RAM |



---



## 4. Things nobody has started, or that are missing from every plan



Nobody has started these, and no plan even mentions them:

- In-call **front proximity** (STK3338 never answers) → screen-off at the ear, auto-brightness (H40).

- **Rollback from an installed ROM** (EDL entry without stock, `reboot edl`, a rehearsal of Plan B) (S14, H63).

- **Off-mode charging** behaviour (H49).

- **Headset button** (H14), **main-mic-without-headset** test (H13), **incoming call/ringtone** (H21).

- **Emergency call** review (H23).

- **A2DP / HFP** Bluetooth audio (H31).

- **Vulkan**, **venus/Codec2**, **Widevine** (H3, H55, H56).

- **FBE encryption** (H59), **notification LED** check (H61), **hardware watchdog** (H64), **RTC alarms** (H62).

- **Production kernel config/cmdline** (S3); the removal of `*_ignore_unused` matters for battery life.

- **OTA/updater, installable recovery, release signing, GApps/Play Integrity decision** (S10–S13).

- **User guide** (S19), a **regression matrix** (S20), **kernel maintenance** (S22).

- **rom-v2 integration** (S2): each agent delivered "for flash", but nobody merged it.



Known but not started: VoLTE/IMS (H25), USSD/SUPS (H24), MMS (H19), OTG (H47), hotspot (H29), SMS in the Messaging app

through the HAL, a microSD overlay (waiting for Pierre).



Questions for Pierre (they block choices): (1) Does the rear screen have a frontlight? (2) Is there a microSD slot in

the SIM tray? (3) Install layout: default, or alternative A (a full userdata backup, ~1.5 h)? (4) Is Wi-Fi-only

acceptable as a first daily-driver milestone, if IPA takes weeks? (5) GApps: none, MicroG or MindTheGapps? (6) Does he

know the hardware EDL entry (key combo or test point) used on 14 Sep? (7) Can the modem EFS be read-write in the ROM?



---



## 5. Attended test queue for tomorrow (25/26 Sep), in order, with exact bundles



Base: laptop `~/A6L-usb-20260915`. Phone: V74 recovery, **fresh boot** (long-press Power if USB does not enumerate). Setup:

`.relay/prep.sh` as usual (toybox links in `/tmp/bin`, sdhci-msm). Every phone line starts with `export PATH=/tmp/bin:$PATH;`.

Pierre types the RF lines himself and appends `2>&1 | grep -v linker`. Before long kernel-log phases, tell Pierre, and

wait for his "ready" before any step that needs him.



**0. (Optional, Pierre decides) V75-usb recovery** (`v75usb/image`, sha 8ecb8e9e…). **Not ready:** the install tools are

not generated (usbfix doc §"Not generated"). Skip it tomorrow, unless an agent generates and offline-tests the

Prepare/Stage/Run tools first. The V74 workaround is to replug at the phone, or long-press Power.



**Boot 1 (fresh V74):**

1. **audio4, no RF, before the ADSP.** `adb -s HLTE730T-PROBE push v75/audio4 /tmp/audio4`

   - `D=/tmp/audio4 MODE=ovl sh /tmp/audio4/run.sh` → `A6L_AU4_OVL_PASS` (it must run BEFORE the ADSP starts)

   - ADSP bundle (v68, modules + `echo start`, as in the audio2/audio3/kvoice sessions)

   - `D=/tmp/audio4 MODE=load sh /tmp/audio4/run.sh` → `A6L_AU4_LOAD_PASS`, `A6L_Q6ROUTING_PATCHED yes`

   - `D=/tmp/audio4 MODE=tone sh /tmp/audio4/run.sh` (headset NOT worn, −30 dB) → `A6L_AU4_TONE_PASS`, no `0x10325`

   - `D=/tmp/audio4 MODE=media sh /tmp/audio4/run.sh` → `A6L_AU4_MEDIA_PASS`

2. **rest2 haptics + hall (no RF).** `adb -s HLTE730T-PROBE push v75/rest2 /tmp/rest2`

   - `export D=/tmp/rest2 MODE=haptics; sh /tmp/rest2/run-rest2.sh`: Pierre reports after each pulse whether he felt it; read the 0x0A/0B/0C snapshots

   - `export D=/tmp/rest2 MODE=hall; sh /tmp/rest2/run-rest2.sh`: a strong magnet or the flip cover over both halves; pass = level toggles + IRQ count rises

3. **radio2 KEEP (RF, Pierre's go, Pierre types it).** `push v74/radio2 /tmp/radio2`, `push v75/ril2 /tmp/ril`, then

   `export A6L_RF_APPROVED=1; export D=/tmp/radio2; export A6L_WATCH=40; export A6L_KEEP=1; export A6L_WIFI_SCAN=0; sh /tmp/radio2/run.sh`

   → `crashes=0`, `A6L_KEEP=1: modem and daemons left running`. Then the PIN/online steps from the 24 Sep ril-test flow if needed.

4. **ril2 SMS decode.** `sh /tmp/ril/ril-test.sh sms-listen 180` → Pierre sends an SMS → `A6L_QMI_SMS_RX … form=bare-tpdu … text='…'`

5. **ril2 DTMF + audio4 call gains (RF, own number/voicemail only).**

   `export A6L_RF_APPROVED=1 A6L_DIAL_TO=<num>; sh /tmp/ril/ril-test.sh dial-dtmf <num> 1234# 40` → `A6L_QMI_DTMF_PASS digits=5`.

   During an active call, in a 2nd shell: `D=/tmp/audio4 MODE=call CVD=0 GAIN=0 SECS=60 sh /tmp/audio4/run.sh`, then

   `GAIN=3` and `GAIN=6` only if Pierre wants louder (the cap is raw 90). Then `sh /tmp/ril/ril-test.sh call-list`.

   **New, not in any doc yet:** if time allows, an **incoming call** from Pierre's other phone with the CLI's call-wait/answer (H21).

6. **gnss2 XTRA (modem still up; phone outside, open sky).** `push v75/gnss2 /tmp/gnss2`;

   `adb shell "date -u $(date -u +%m%d%H%M%Y.%S)"`; `sh /tmp/gnss2/gnss2-test.sh query`;

   `sh /tmp/gnss2/gnss2-test.sh xtra <lat>,<lon>,5000 600` → `A6L_GNSS XTRA_OK parts=32`, then a FIX. The XTRA files expire

   ~7 days after 24 Sep. Put Pierre's everyday A6L next to it for a C/N0 comparison.

7. **camera3 LAST in this boot (it may still oops).** `rm -rf /tmp/camera3`; `push v75/camera3 /tmp/camera3`;

   `D=/tmp/camera3 MODE=probe sh /tmp/camera3/run-camera.sh` → `CAMSS_CAMFIX2_PASS`;

   `D=/tmp/camera3 MODE=bars SENSOR=imx576 sh /tmp/camera3/run-camera.sh`; `pull /tmp/cam-imx576-bars.dmesg v75/logs/`.

   Pass = no `KERNEL_OOPS_FAIL`; ideally `CAPTURE_imx576_bars_PASS`. If that works, repeat with s5k3t1 and hi846, then `MODE=off`.

   After an oops: stop and reboot (run-camera refuses to continue anyway).

8. `pull /tmp/audio4-out v75/logs/audio4-out` (and the ril/gnss logs) **before** the reboot.



**Boot 2 (fresh V74, its own boot; it may reset the phone):**

9. **ipa2b step walk.** `push v75/ipa2b /tmp/ipa2b`; **first** `adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; dmesg -w' > v75/logs/ipa-klog.txt &`;

   then for N in **1, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 0**:

   `export D=/tmp/ipa2b; export MODE=load; export STOP_AT=N; sh /tmp/ipa2b/run.sh` → `A6L_IPA2_STEP_PASS`. On a reset, the last

   `A6L_IPA_STEP` in ipa-klog.txt is the culprit. Retries: `NOIOMMU=1 STOP_AT=13` (SMMU attach), `IDENTITY=1` (first DMA at step 13).

   After `A6L_IPA2_LOAD_PASS`: radio2 KEEP (RF), then MODE=status / MODE=data per ipa-20260924.



**Not tomorrow, but the next attended block:** stage kit-r8 and prove the rollback path (S14/H63) → first eMMC install (S1)

→ charging check (H48) → C2 → cpufreq (H52, Pierre only, no MODE=fuses).



---



## 6. Evidence index

Project: claude/attended-20260925, fixes-20260924, attended-20260924, fullphone-20260924, camfix/cam-ipa-20260924, morning-20260924,

prep-20260923, session-notes. Repo docs: port-status (stale), flash-20260924 (§1–§9), hals-20260924, rest-20260924, ril-20260924,

misc-20260924, kvoice-20260924, audfix-20260924, audio3-20260924, gnss-20260924, ipa/ipa2fix-20260924, cam/camfix-20260924,

camfix2-20260925, usbfix-20260925, eink3-20260924, realinit-progress-20260923, roadmap-to-working-image-20260920,

hardware-readiness-20260920. Checks made today: `device/hisense/a6l/rom/{rom.mk,full/full.mk}`, the laptop listing (relay todo-02:

v75/{audio4,rest2,ril2,gnss2,camera3,ipa2b} and v75usb/image present, rom-v1 absent), `git status` (153 entries, last

commit 21 Sep).



---



## 7. VoLTE update (25 Sep, `volte` agent; see docs/volte-20260925.md). This supersedes the H25 estimate

- **H25 VoLTE: research done, plan staged.** The stock SDM660 IMS runs **on the modem**: SIP, RTP and the vocoder; the

  AP only answers IMSDCM (770) PDP requests. The modem already has an **Orange FR MCFG** (IMS_enable=1, voice

  domain = IMS preferred). The recommended path is **B**: modem IMS, transparent to Android (no ImsService). It needs

  an IMSDCM server + an IMSA reader in the radio HAL, dialling over the existing QMI VOICE path, and a VoLTE CVD

  session in q6voice. That is **about 2–3 weeks + 3–4 attended sessions**, and it does not depend on IPA.

  The fallback is **A**: the AP ImsService from joan-volte-lineage (Apache-2.0, has an ORG.FR profile), which needs IPA data.

  **Not needed for the first image.** Needed before the Orange 3G shutdown (2028). IMS emergency also depends on it.

- Attended (queue after radio2 KEEP + online; queries only): **step −1**: on the *stock* A6L with Pierre's SIM,

  `*#*#4636#*#*` → "IMS registration: Registered"? (if not, Orange may refuse this device). **Step 0**:

  `push v75/volte /tmp/volte` → `/tmp/volte/volte-probe all` (twice: before and after `online`), then `imsa-watch 120`.

  Expect `PDC_INFO … desc='France-Commercial-Orange'`, `IMS_SVC 33 … PRESENT`, `NAS_SYSINFO ims_voice_support=1`.

- The unknown QRTR services are now identified: 68 = OTT, 74 = ANTSWITCH, 4098 = Hisense OEM (do not touch), 54 = still unknown (modem-internal).

