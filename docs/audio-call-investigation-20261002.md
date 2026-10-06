# Android call and media audio — 2 October 2026

The r6g Orange test registered on LTE and loaded Google over mobile data after
the physical-slot-count workaround. The outgoing ordinary call and voicemail
connected, but Pierre reports no audio in either direction, including after
trying speakerphone. Recorder displays a waveform while he speaks, but playing
the recording is silent. Some short Android UI sounds are audible. A waveform
alone does not validate recorded audio content or microphone calibration.

Evidence is in
`firmware/extracted/rom-r6g-20261002/r6g-modem-call-test-20261002.tar.gz`.
The call boot ID starts `be06`; the phone subsequently suffered a GPU fault
at 661.048 seconds and rebooted to boot ID `1da5b`. The sound-node snapshot
with q6voiced PID 1131 belongs to the new boot, whereas the call-boot audio
processes are PIDs 1086/1087. Those snapshots do not establish an in-boot sound
card removal/reprobe.

## Confirmed media route configuration defect

The route daemon repeatedly fails verification of
`LPI_MI2S_RX_0 Audio Mixer MultiMedia1` (expected 1), then reconstructs the
libaudioroute state and eventually reopens the mixer. The persisted kernel
selection is `perdir`, and the patched q6routing driver is loaded.

Its `msm_routing_put_audio_mixer_dir()` stores one backend port per front end
and direction. Enabling another playback backend replaces the first selection;
the corresponding get control reports only the selected port as enabled.
The old XML defaults require LPI playback enabled and the speaker path also
requires TERT playback enabled. These simultaneous requirements cannot hold.
This explains the captured verification mismatch without assuming failed
microphone hardware or an absent sound card. It also creates unnecessary
reinitialization during normal speaker playback.

The speaker path now explicitly sets LPI playback to 0 before selecting TERT.
Other media outputs and call paths keep their existing selections. No DSP
ports, gain, power sequencing or voice-session parameters were changed.

The host mixer fixture previously treated these controls as independent
booleans, hiding the defect. It now models the kernel's exclusive selection.
The real libaudioroute and route daemon fail against the old XML and pass all
260 checks against the new XML; the Android route publisher passes 12 checks.
These tests include route transitions, missing controls, write failures and
card reset cases. Physical playback still requires validation.

## Call silence remains a separate unresolved failure

During the calls, the analog route daemon logs successful `voice-handset` and
`voice-speaker` applications. The kernel initializes voice path 6 with TX port
0x1035, first RX 0x102e and then speaker RX 0x1004. Speaker initialization
repeats at intervals matching q6voiced's failed-open retry/backoff. The saved
kernel log does not identify the failing PCM ioctl or prove successful voice
PCM opens. Media `q6routing`/ADM errors are insufficient evidence that the
separate CVD voice path failed for the same reason.

Existing attended reports identify the earpiece on this unit as physically
broken; downlink acceptance must use the speaker or wired headset. The earlier
recovery call proof used the wired headset. It does not establish Android
speakerphone audio. There is no new evidence justifying a voice-port or CVD
session change yet.

q6voiced already prints detailed open/HW_PARAMS/PREPARE/START errors, but Android
init discards its stdout/stderr. The ROM build now mirrors these messages to
logcat under `a6l-q6voiced`, retaining CLI output and preserving `errno` around
logging. Its Soong target links liblog and enables `A6L_Q6VOICED_LOGCAT`; existing
static NDK recovery tools remain independent of liblog. This adds diagnostics
without changing voice-call behavior.

Verification: 130 voice-daemon checks pass. A logging regression verifies
varargs, message tag and priority, retained stdout/stderr, and preserved errno.
Both the Android logcat binary and static recovery variant compile with NDK
r27c/API34 and warnings treated as errors. Isolated artifacts and reports are
in `firmware/extracted/audio-route-voice-log-20261002/`.

Next physical validation after the combined build:

1. Play a known recording on speaker, then wired headphones. Require stable
   playback and absence of repeated media-route verification/reopen failures.
2. Record speech, retain the file and inspect/listen to it independently, then
   repeat local playback. Check capture and output separately.
3. With the SIM inserted for the attended session, make an ordinary permitted
   call using the speaker or wired headset. Capture q6voiced's PCM error or
   successful OPEN marker, analog route state, mute/volume, call RAT/domain and
   corresponding CVD logs. Verify audio in both directions.

The XML and logging changes are prepared for integration. No physical phone
changes were made by this investigation, and call audio is not marked fixed.

## r6k control coverage audit and narrow candidate, 3 October

The saved r6k first-boot log reports successful media routing on card 0 at
07:48:44.222 (`out=speaker in=main-mic`) and no repeated route verification or
mixer-reopen errors in the observed interval. The primary AudioFlinger output
opens for 48 kHz stereo PCM16. HAL playback command replies advance their
observable frame counters with success, reaching 1,630,208 frames before
suspend. These counters describe framework/HAL progress, not audible output;
the saved replies do not report a valid hardware position. No captured ALSA
write or DSP error identifies a media failure. A known audible input file,
active PCM status, applied media gains/mute and separate speaker/headphone
results are still needed. There is no evidence supporting a new media route,
card index, DSP port or gain change. The earlier `card2` shell result was a
two-line match count, not a sound-card index.

The new q6voiced logging exposes a separate real packaging mismatch: r6k
requests to write `VoiceMMode1 TX Mute` fail with ENOENT repeatedly at
07:49:01–07:49:08, and the audio HAL rejects `setMicMute` with error -38.
Both staged prebuilt and product vendor copies of `q6voice-dai.ko` lack the
`VoiceMMode1 TX Mute` and `VoiceMMode1 RX Volume Step` control strings.
The retained `q6voice.ko` and `q6cvp.ko` also lack their implementation symbols.
The existing audio4 staging default retained a proven older voice stack while
the later daemon/HAL already expected these controls. Missing controls do not
by themselves prove why a call is silent: the daemon retains a mute request
only after a successful control write, and unaccepted startup unmute requests
do not prevent opening an ordinary call. This r6k capture had no new attended
SIM-call test.

An isolated candidate adds only the two canonical v75 mute/volume patches to
that exact older stack. It is in
`firmware/extracted/voice-controls-20261003-063728/`; `manifest.json` identifies
the required all-or-none three-module replacement. No Bluetooth pseudo ports,
DT links, media PCMs, route XML, calibration, kernel Image or other sound
modules change. The unmodified isolated baseline produces `.text` sections
identical byte for byte to all three currently staged modules, establishing
that this candidate really starts from the retained drivers.

Candidate pins:

| Module | SHA256 |
| --- | --- |
| q6cvp.ko | 6214916403c68cf1c2e6c270418011ab42169e4d18525ac8fc2f1a134b56a082 |
| q6voice.ko | 4527a4ec7df48abd761f107019498ea9c538f7b8eeaa61067806bb82e40f5ad5 |
| q6voice-dai.ko | adad28e27135cb1a9df38a320d91e8bb6d0676e92a8f93670b849e717a2679a1 |

All modules have the installed vermagic
`7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64`. All 62 candidate
imports resolve with matching CRCs against the preserved full kernel and this
three-module set. Every shared import also matches the original staged
module directly; no existing export CRC changes. Scanning the other retained
modules found no consumer imports of these replaced exports. The six added
exports are the new mute/volume APIs between these three modules. The
manifest SHA256 is
`ac512b44683d16e0ee070576bbb981ae339ac7c6cf531ca4d90ca05f8e58957e`.

The actual candidate q6voice implementation plus the actual extracted CVP
command functions pass 75 ASan/UBSan checks: accepted state, failed live
commands leaving state unchanged, pre-call storage/restoration, mute failure
preventing a call path from starting, volume failure allowing the call to
start, invalid inputs, and APR packed sizes, TX/RX direction and ramp values.
Kernel compilation uses isolated prepared metadata and `W=1`; the preserved
Image, config, release headers and full symbol table stay unchanged. No source
or payload was copied into the ROM by this agent, and no phone operation was
performed. Root is reviewing integration. This restores missing control
coverage; physical call and media audio still require the attended tests above.

The read-only collector `tools/collect-a6l-audio-wake.py` is included in r6l's
kit extra directory. Run with a fresh output directory once idle and again during
the failing playback. It saves volumes, AudioFlinger/policy state, PCM status and
parameters, mixer values, power/display state and logs with boot IDs before/after.
It neither changes routing/volume nor initiates playback, calls or sleep. Missing
permissions/tools are recorded as command failures; they are not treated as
evidence of a silent hardware path.

After r6l boot, the idle capture confirms music/system volumes are nonzero
(speaker music 12/15, system 5/7) and master/stream mute is false. All PCMs are
closed while idle; this is not a playback measurement. Mixer capture cannot open
controlC0 because shell is not in the audio group (node 1000:1005, mode 0660),
not because the sound card is absent. Two test WAVs were copied locally to the
laptop: both contain nonzero PCM with peaks 17510/10073 and RMS -31.3/-38.3 dBFS.
This establishes a recorded signal, not its content/quality or audible playback.
Levels are in rom-r6l-20261003/audio-recording-levels.json. Active playback,
hardware mixer state through an authorized diagnostics path, and speaker/wired
comparison remain pending.
