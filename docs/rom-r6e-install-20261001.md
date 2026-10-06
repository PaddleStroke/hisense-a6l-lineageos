# A6L r6e attended installation — 1 October 2026

Installed from the laptop kit `~/A6L-usb-20260915/rom-r6e`, through
`Launch-RomUpdateV1.py --mode update --transport adb-recovery`.
Started at 12:18:48 UTC and finished at 12:35:12 UTC (16 minutes 24 seconds).

The worker verified the phone's recovery, GPT and expected r6d predecessor.
It saved full boot, dtbo, vendor and system backups, wrote boot/vendor/system,
skipped unchanged dtbo, flushed caches and verified the written images by
reading them back from storage. Protected-region hashes matched before and
after; the program allowlist excludes userdata. Coordinator exit 0, no error,
host services restored.

| Partition | Verified SHA-256 |
|---|---|
| boot | `b28ef87c54af9f992d688d7cacec0a66319e3ca78484fe4adcb468e878c1e0a8` |
| vendor | `85aedbb45b76960df690ddec6c028e3b7503011128c63f75e5dde993cf9ce666` |
| system | `9e3e65927fa7c39c23c2461713bc721bcb3d14361d7b9a80a5b0d9e5e21e9592` |
| dtbo (unchanged) | `6925112258af276ad2d16757df054bbc6e98148b5273f0a214e7d8ddd5044e7e` |

Rollback source on the laptop:
`~/A6L-usb-20260915/rom-r6e/capture-rom-update-update-20261001T121848Z/edl`.
Do not delete this capture. Small verification reports are copied to
`logs/r6e-install-20261001T121848Z/` in the desktop repository; the full
partition backups remain on the laptop.

The worker reported `power: off`, but adb still showed recovery afterward.
The reported shutdown is therefore not proof of actual power-off. The tested
recovery-to-Android sysrq reboot is used only if `HLTE730T-PROBE` still shows
recovery; otherwise Pierre boots manually with USB unplugged first.

The early-boot collector is waiting for Android serial `1e529013` and verifies
the r6e build marker before recording. It streams kernel and Android logs,
waits for the Settings provider, saves the prior stay-awake setting, keeps the
plugged-in phone awake for 20 minutes, then restores the prior setting. It
identifies eMMC through its controller path instead of assuming `mmcblk0`.

Laptop log directory: `rom-r6e/logs/live-first-20261001T121848Z`.
Collector output: `rom-r6e/logs/collector-first-20261001T121848Z.log`.
Final collector SHA-256:
`d413247db13b514949fb903a130dd3f8368a11256f4248f545d1165309b23751`.
The local kit archive was repacked with this helper; SHA-256:
`8c9fddb2f5f0ab6e551ed2266b698170f4c4959ac6111a0fbee404e4e3b8d810`.

First physical test: Android booted fully, but froze again on the lock screen.
The live kernel stream stopped at about 210 seconds and adb became offline.
There was no suspend entry or A6L_IOSTALL report; the storage heartbeat and
host samples continued completing before the freeze, with zero eMMC inflight
requests in the last samples. This does not establish the original freeze's
cause, and removing discard has not established stability.

The now-executable radio daemons exposed a definite configuration error:
rmtfs attempted `/dev/disk/by-partlabel/modemst1`, which does not exist in
Android. The modem crashed immediately after each failed EFS open, repeatedly
recovered (13 cycles), and IPA channel resets timed out. The daemon supports
`-P -o /dev/block/by-name`; that targeted source fix is prepared but is not
included in the installed images. A radio-disabled boot is the next isolation
test before attempting a live modem fix.

Android enumerated all three cameras; preview/capture, Bluetooth, backlight
and e-ink settings remain unverified. Full logs are in the desktop repository
under `logs/live-first-20261001T121848Z/`.

Radio-disabled test started at 13:09:05 UTC (log tag
`radiooff-20261001T130905Z`, collector PID 1058677). The shell verified
`persist.vendor.a6l.radio=0` at uptime 32.41 seconds, before boot_completed.
The phone was responsive at uptime 254.54 seconds, with boot_completed=1,
no started radio/rmtfs service and regular successful filesystem heartbeats.
Pierre's correction is essential: he was actively using Settings and the camera
during this run, and the reboot occurred when he pressed Power to lock the
screen. This was NOT a spontaneous idle failure. The logcat records
`Going to sleep due to power_button` at 07:01:47.499 (phone clock), followed
by GPU ringbuffer drain timeout, opcode error, GPU fault and hangcheck recovery
at kernel uptime 307.39–308.43 seconds. A subsequent boot reports reason
`reboot`. The radio remained disabled. Storage heartbeats completed normally
before locking. This isolates a screen-off-associated graphics failure from
the separate radio startup problem; the exact cause within the power/render
transition remains unproven. No 20-minute idle pass was completed.

Pierre confirmed the e-ink settings entry opens. Display-page text was cropped
on first visit, then normal after opening e-ink settings and revisiting Display.
Other UI text also crops intermittently; this is not a fixed label/layout defect.

Further physical e-ink feedback: e-ink/Power controls switch sides, and rear
touch works. Switching takes about six seconds (first two seconds inactive,
then white/black/white/black flashes before the image); subsequent updates are
also about one picture per six seconds even when Pierre tries fastest mode.
White bars occupy the left/right edges. These are usability blockers recorded
in `docs/master-todo-20260925.md`, in its 1 October physical-feedback section.
The source's default letterboxing explains the bars; the refresh policy forces
clear + quality on entry. Actual timing bottlenecks remain unmeasured. Preserve
Pierre's report that the LCD flashed and verify the affected panel in a capture.

Camera app still shows no image and closes. The actual Aperture exception is
CameraX initialization failure (`Failed to build surface combinations`), caused
by an assertion reading `android.scaler.streamConfigurationMap` while loading
CameraId-2. The camera inventory shows the Hi-846 wide camera exposes JPEG-only
stream configurations; cameras 0/1 expose PRIVATE/YUV/JPEG. Android's
StreamConfigurationMap requires an IMPLEMENTATION_DEFINED output for a
backward-compatible camera. Investigating the HAL metadata generation before
selecting a fix; camera preview is not established by enumeration alone.

Logs are copied to `logs/live-radiooff-20261001T130905Z/` in the repository.
Passive logging was restarted after the reboot with tag
`lockdiag-20261001T132014Z`, collector PID 1063245. The current boot remained
responsive at uptime 1211.52 seconds, with zero eMMC inflight requests and
IO avg10=0; Pierre's interactive test supports a lock-triggered fault rather
than an elapsed-time failure. No further automatic lock or reboot should be
performed while Pierre is using the phone.

With Pierre ready for a controlled Settings test, compared fresh Display-page
launches under default rendering and the temporary `sysmem` option. Both
screenshots are visually normal and show the same page. Properties were
restored. Mesa option logging was absent even from the complete live log,
so consumption of the option is not verified; this is an inconclusive test,
not evidence of a rendering fix. Captures are in
`logs/mesa-display-{base,sysmem}-20261001T1328/`.

Camera root cause located in libcamera `CameraCapabilities::initializeStaticMetadata`:
the IPU3-specific fps<30 filter discards all PRIVATE/YUV modes for Hi-846,
but retains JPEG. New patch 0011 preserves those slower preview modes with
their existing durations and prevents an inverted integer FPS range for fixed
sensor timing. It is compiling in isolated `src-r6f-camera` / `build-hal-r6f-camera`
directories; no new camera binaries or ROM have been installed on the phone.
The isolated HAL build completed (`HAL_OK`). Packaging, strip/re-sign, IPA
signature and embedded-public-key verification, and prebuilt copy all passed.
Corrected camera prebuilts are staged in the repository for the next build.
An offline regression compiled the actual old/new metadata-generation block
against all seven Hi-846 durations from the physical inventory. The old code
produced JPEG-only modes and an inverted FPS range; the new code retained
PRIVATE/YUV/JPEG and valid [3,3] metadata without inventing 30-fps timing.
Evidence: `firmware/extracted/rom-r6e-20261001/camera-slow-preview-{build,regression}.json`.

Next power-transition isolation: no-flash switch from `persist.graphics.egl=mesa`
to `angle`, then restart SurfaceFlinger (its init onrestart restarts zygote).
Prepared at 13:47:34 UTC; streams are in laptop
`logs/live-angle-lock-20261001T134734Z/`. Original EGL choice is recorded there.
Radio remains 0. SurfaceFlinger confirms ANGLE over SwiftShader, rather than
Mesa/Adreno; SurfaceFlinger and zygote are running and boot_completed=1.
A five-minute passive power/backlight sampler (PID 1075121) records uptime,
bl_power, requested/actual brightness and Android wakefulness. Pierre pressed
Power, waited ten seconds and pressed it again: **backlight off and normal wake**.
The logs confirm power-button sleep and wake, and bl_power=4 while asleep.
The driver's actual_brightness still reports a nonzero value while blanked;
Pierre's physical observation confirms the backlight was off. No GPU fault or
kernel suspend-entry message was observed, so whole-SoC suspend remains unproven.
No lock command was sent by the agent. Restore the original renderer
with `adb -s 1e529013 shell 'setprop persist.graphics.egl mesa; setprop ctl.restart surfaceflinger'`
after the comparison. A full reboot's post-fs-data path also selects Mesa when
the GPU and libraries are present, overriding this diagnostic EGL property.

Hardware-rendering comparison prepared at 13:57:01 UTC, log tag
`sysmem-lock-20261001T135701Z`. Set `debug.mesa.fd.mesa.debug=sysmem` before
selecting Mesa and restarting SurfaceFlinger/zygote. Verified SurfaceFlinger
uses freedreno FD512 Mesa 26.1.0-devel, both services run, and radio remains 0.
Kernel/logcat streams and a ten-minute passive backlight sampler are running;
Pierre repeated the manual lock/wake and confirmed backlight off and normal
wake. Logcat records two sleep/wake cycles, with no GPU drain timeout, fault,
hangcheck or IO-stall report after the renderer switch. This is a provisional
workaround, not proof of long-term stability or a root-cause fix. The earlier
absence of FD_MESA_DEBUG logging is explained by the pinned Mesa release build:
debug_printf is compiled out unless MESA_DEBUG is enabled. The source maps
FD_MESA_DEBUG to debug.mesa.fd.mesa.debug and parses it independently of logging.
Snapshots are saved locally in `logs/live-angle-lock-20261001T134734Z/` and
`logs/live-sysmem-lock-20261001T135701Z/`. Three longer manual cycles plus a
Display-page inspection were requested while preparing the next offline build.

Backup retention: Pierre requested that test backups not accumulate. Removed
the four obsolete r6c rollback payloads from
`rom-r6d/capture-rom-update-update-20261001T063939Z/edl`, retaining its reports,
and the four duplicate image payloads in `rom-r6e.prev/images`, after comparing
them with the current kit. This freed 9.01 GiB. The stock restore capture and
the latest r6d rollback listed above remain complete. Historical directories
have explicit retirement notices; the laptop root has a cleanup inventory in
`backup-retention-20261001.json`. Development retention is stock plus one
previous-test rollback; no full-userdata backups.

Temporary 2:1 layout test at 14:43 UTC: original physical size was
1080x2340, density 400, with no overrides. At uptime 5278 s, requested
`wm size 1080x2160`; the command and logical screenshot succeeded.
SurfaceFlinger centred the viewport in the physical scanout at y=90..2250.
Pierre then reported a freeze while going to Display settings, without a
reported Power-button action. Radio remained disabled and Mesa sysmem was
active. This is a separate failure from the earlier default-Mesa lock crash;
sysmem is not sufficient evidence of graphics stability.

The new live streams yielded no output; bounded adb shell/exec-out probes
timed out although USB initially remained enumerated. The three-minute
fallback also timed out, so restoration of the original layout is **not
verified**. The override may survive a reboot: restore with `wm size reset`
at the next accessible Android boot. Preserve recovery pstore/metadata first.
Laptop evidence: `logs/layout-2to1-20261001T144330Z/`. No ROM was flashed and
no partition backup was made for this test. Root cause awaits recovered logs;
the timing alone does not prove the resolution change caused the freeze.

Recovery follow-up: pstore was empty. The metadata log was copied read-only
(10 MiB partition); its configured short boot capture stopped long before
the later layout freeze, so it provides no direct cause for this event.
Local copy: `logs/layout-freeze-evidence-20261001/`. Booted the same r6e,
verified radio=0, started four-hour kernel/logcat streams, selected ANGLE
over SwiftShader and restarted SurfaceFlinger/zygote. `wm size reset` then
succeeded: physical 1080x2340, density 400, no overrides, boot_completed=1.
Restoration is now verified. Laptop tag: `restore-layout-20261001T150100Z`.
Pierre was asked to repeat Display settings navigation at original geometry
with software rendering. No new build or partition backup was involved.
Pierre confirmed normal text and responsive navigation, but more lag than
before. Software rendering is a diagnostic baseline with a performance cost,
not an acceptable final renderer. No new GPU fault was present in the live
kernel stream at the feedback snapshot (uptime 259 s).
