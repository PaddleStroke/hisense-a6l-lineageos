# r6n media copied and inspected — 3 October 2026

User acceptance: waking from sleep is fixed; Android UI sounds are back; all
three cameras work. Focus improves detail but hunts. Contrast, sustained video
and on-phone recording playback remain unresolved. The phone was returned to
recovery after USB disappeared; USB loss alone is not evidence of a GPU crash.

## Copies and reproducible measurements

Fourteen original files (nine JPEGs, two MP4s, three WAVs) are in
`C:/Users/Pierre/Desktop/A6L-media-r6n-20261003`, preserving Camera and Sound
records subdirectories. SHA256 matches the laptop retrieval. Recovery userdata
was mounted read-only with `noload`, then cleanly unmounted. No partition backup
or userdata journal replay was performed. The metadata reader replayed a
separate laptop copy of metadata only.

`tools/media-tests/analyze-a6l-media.py` decodes media without altering originals.
Results and separate previews are in
`firmware/extracted/rom-r6n-20261003/media-analysis/`. An isolated temporary
WSL environment supplied PyAV, NumPy and Pillow; ROM dependencies are unchanged.

| File | Measured result |
| --- | --- |
| Voice, 11:59:09 | PCM16, 44.1 kHz stereo, 9.728 s; mono-downmix RMS -35.59 dBFS, peak -11.3 dBFS, no clipping |
| Video, 11:58:10 | 56 unique frames, 4.743 fps; AAC RMS -75.13 dBFS |
| Video, 11:59:45 | 56 unique frames, 4.608 fps; AAC RMS -55.09 dBFS |

Both videos are 720×1278 with median frame intervals about 267 ms and longest
intervals about 467 ms. Signal and audio tracks exist, but only listening can
establish intelligible speech. Separate peak-normalized video audio previews
are clearly labeled in the Desktop folder; originals remain unchanged.

The user was asked to listen to the latest original voice WAV on the computer.
Until that result arrives, distinguish microphone capture, very quiet video
capture and phone playback rather than treating them as one established fault.
The captured playback log opens output PCM 0:0 and restores the speaker route;
it lacks active hardware mixer/PCM evidence sufficient to diagnose silence.

## Camera findings

New JPEG orientations are correct. Keyboard lettering is readable, but arbitrary
scene photos show raised shadows, green cast and severe highlight clipping;
the bright-window photo clips about 47% of pixels to white. There is no chart
or matching reference shot to derive calibrated color correction.

Rear CPU ISP processing samples take about 61 ms/frame; the front sample takes
95 ms. CameraX nevertheless requests 30 fps and approximately 12 Mbps from the
software AVC encoder. The measured 4.6–4.7 fps requires investigating the whole
capture/ISP/encoder pipeline, not attributing every lost frame to one stage.

The existing logs measure Recorder pending-to-muxer at 0.98/1.65 seconds and
encoder stop-to-configured at 5.54/5.48 seconds. They do not cover the complete
user tap or mode change preceding the pending event, so the reported five-second
startup is not disproved by these narrower intervals.

Rear/wide JPEG files also contain 2.37–2.75 MB beyond their valid JPEG end. The
HAL places its footer at padded gralloc capacity instead of logical Android
BLOB capacity. An isolated bounds/overflow-tested correction is included in r6o;
it is separate from tonal quality and frame rate.

See `camera-quality-r6n-20261003.md` for actual source findings. Black-level
units and direct full-range NV12-to-JPEG conversion alone do not establish the
cause of lifted shadows. Do not apply arbitrary shadow subtraction or gamma
changes without controlled RAW/statistics evidence.

## E-ink findings

All six switches reach authoritative theme/animation readiness, but four then
hit the three-second fresh-frame fallback. Actual framework/source comparison
finds the sync group can complete before the requested client redraw. A narrow
fresh-draw requirement is being prepared, preserving valid present-fence and
theme/wallpaper proof.

Warm updates in the captured Auto policy complete around 956 ms. A first rear
switch took 3.379 s to acknowledge/light: theme ready at 1.215 s, with repeated
capture, cold bridge setup and first waveform overhead afterward. Fastest was
not verified by these logs. The latest section of
`eink-investigation-20261002.md` explains four separate proposals: fresh-draw
proof, earlier prepare detection, first eligible frame submission, and bridge
startup overlap. Electrical sequences remain unchanged. Modern waveform and
ghosting research does not establish 15–60 completed fps on this panel.

The r6n findings above describe the frozen measured build. The reviewed r6o
bundle passes actual-image verification and its recovery installation has
started. Physical acceptance remains pending.

## Next controlled run

The original voice WAV must be listened to on the computer, then compared with
phone speaker and wired-headphone playback at the same media volume. Capture
the bounded PCM watcher while playback is active. UI effects being audible
does not establish that media playback or video microphone gain is correct.

First compare rear switching in the same Auto policy, then select Fastest and
read back `persist.sys.a6l.eink.refresh` before and after a screen switch. The
native log must show `refresh mode fastest` and `fixed=1`. The r6n stored
selection was explicitly `auto`; a read-only source audit found no automatic
reset during switching. UI feedback alone is insufficient if a property write
failed.

The isolated fenced-capture research in
`firmware/extracted/eink-fenced-capture-20261003/feasibility.md` targets 235 ms of
CPU capture/resize work. Its ownership prototype and API compilation pass, but
no backend is integrated or measured. Java's native callback waits indefinitely
for its GPU fence, so a timed-out request must keep its sole capture credit
until a late result resolves. Native authenticated handle transport and a
one-shot measured capture are separate next steps.
