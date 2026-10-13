# r6f installation — 2 October 2026

Pierre authorized installation while present and connected the test A6L in
diagnostic recovery. Verified serial HLTE730T-PROBE on laptop USB port 3-2.
The other phone is excluded by explicit serial guards.

The final r6f build, image inspection, fresh QEMU boot/stability and
keep-data update/rollback simulations passed. The corrected QEMU boot had
one zygote start, boot_completed at 1458.28 s and 126 s of post-boot stability.
Laptop transfer, all staged-file hashes, full kit verification and update
dry run passed; full kit verification was repeated immediately before launch.

Kit: `/home/pierrelouis/A6L-usb-20260915/rom-r6f`.
Launch PID: 1439983, via Launch-RomUpdateV1 with the desktop suspend inhibitor.
Capture: `capture-rom-update-update-20261002T063336Z` (r6e rollback).
Started at 08:33 Europe/Paris; completed at 08:49:58 with worker exit 0.
System/vendor readback hashes match the pinned r6f images. All protected-region
hashes stayed unchanged, and coordinator cleanup completed successfully.

| Image | SHA-256 prefix | Update |
| --- | --- | --- |
| boot | b28ef87c | unchanged |
| dtbo | 69251122 | unchanged |
| system | bf38bcad | changed |
| vendor | 17d0a6b6 | changed |

The updater preserved userdata and checked the protected regions before and
after writing. Its default full ROM snapshot completed before the backup
policy was clarified. No full-userdata backup was made.

Recovery remained reachable after the updater's power-off request. Started
the guarded boot observer, then rebooted through sysrq from recovery.
Physical Android boot completed at 76.70 s. Renderer is freedreno FD512,
OpenGL ES 3.1 Mesa 26.1.0-devel, with sysmem active. Radio remains 0.
Original 1080x2340 / density 400 geometry has no override. Live kernel/logcat
streams are bounded to four hours. Settings validation is pending; initial
kernel scan had no GPU hang/fault/panic matches at uptime 129 s.
Stay-awake was already 7 before this test and remains 7.
Camera preview/capture and e-ink latency/ghosting require physical validation;
modem startup with the corrected rmtfs path is a separate recorded test.

Pierre clarified the backup policy during this run: routine development ROM
partition snapshots are unnecessary because the verified stock restore and
previous development image kits already exist. The current updater had
already completed the r6e snapshot by 08:44 Europe/Paris; continue the pinned
run, then retire development snapshot payloads after successful readback.
Keep stock captures, verified ROM images, manifests and diagnostic reports.
Future development updates should identify installed-image heads with hashes
and preserve protected-region/readback checks without saving full old ROM
partitions. The existing pinned updater still implements full snapshots;
this tooling change remains to be made and verified for the next update.

At 08:54:38, verified the retained r6d/r6e/r6f image kits against the install
manifests and confirmed stock backup file sizes. Removed only boot.bin,
dtbo.bin, system.bin and vendor.bin from the two development snapshot captures
(r6e capture 20261001T121848Z and r6f capture 20261002T063336Z).
Freed 15,342,764,032 bytes (14.29 GiB); stock captures untouched.
Retirement records, manifests and installation reports are retained.
The workspace copy of the r6f readback report is
`logs/r6f-install-20261002/flash-report.json`.

Attended baseline: Pierre reports scrolling Settings → Display is responsive,
but some text remains cropped. Requested an immediate screenshot; the captured
screen was the notification shade. Quick-setting labels and the Settings
notification have missing glyph regions, while the Trust notification is intact.
The corruption is present in screencap, so it occurs before panel scanout.
Mesa sysmem alone does not resolve text corruption. Evidence:
`logs/r6f-install-20261002/settings-cropped-20261002.png`.
Camera, e-ink and modem tests remain pending.

Further text clue: Pierre observes the shade clock is correct at its starting
size, but becomes garbled at some intermediate sizes while dragging the shade
open and enlarging it. Affected glyph regions seem random. This suggests a
scale-sensitive rendering problem; the failing graphics stage is unconfirmed.
At uptime 658.80 s, adb and camera service were responsive (three cameras,
no active client). Continuous kernel log had no GPU hang/fault/panic matches.

Camera launch immediately invalidated the graphics baseline: first ringbuffer
drain timeout / a5xx GPU fault at uptime 659.94 s, followed by repeated
hangcheck recovery and more faults (status C30001C1). Aperture stayed on its
splash; camera service still had no active camera client. SystemUI and
launcher input ANRs followed. This test did not reach camera preview, so it
does not establish whether the camera metadata fix succeeds or fails.
The screenshot request timed out. Adb shell remained responsive.
Evidence copied to `logs/r6f-install-20261002/camera-stuck-20261002T070616Z/`.

Requested ANGLE + SurfaceFlinger restart, but compositor could not fully exit:
process 1132 was a zombie with two remaining threads (one RenderEngine),
init.svc.surfaceflinger=stopping; system_server was also a zombie.
Saved fault/ANR evidence, then requested adb reboot with a guarded observer
to wait for a fresh r6f/radio=0 boot and select ANGLE before the next camera
test. Software renderer verification is pending. No flash or partition backup.

Reboot succeeded. Fresh boot ID b2060ced-e861-4947-b4f4-4ace9535bd27.
Guarded observer selected ANGLE after initial boot and restarted SurfaceFlinger
successfully; SwiftShader confirmed, services running at uptime 98.22 s.
Logs: laptop `rom-r6f/logs/software-camera-20261002/`.
Attended camera retry requested with this renderer. GPU acceleration remains
unfixed; software rendering is a diagnostic workaround.

Software-renderer camera retry: Aperture passes splash but reports stream
preparation error and closes. Rear camera 0 opens; HAL rejects Adjusted
1600x1200 NV12 + mapped JPEG configuration. New patch 0012 and prebuilts are
compiled/regression/signature verified for the next ROM, not installed.
See `docs/camera-streamfix-20261002.md` and
`logs/r6f-install-20261002/camera-stream-error-20261002T072214Z/`.

E-ink attended feedback with software rendering: each UI swipe takes about
two seconds already, so Pierre cannot reliably separate UI lag from e-ink
refresh latency. Does not look good; white side bars remain. Fastest/mirror
verified in properties, but refresh fix is not physically validated by this
confounded test. Full-area layout changes were not included in r6f.

Separate modem test with SIM inserted: corrected rmtfs partition path works,
modem firmware runs for about 400 recorded seconds without the prior fatal
error. Android radio HAL instead loops on an unset-cell-identity abort. Guard
compiled and targeted regression passed, ready for next ROM but not installed.
Phone entered sleep; Pierre reports a roughly 20-second wake and responsive,
very laggy UI. USB enumeration did not return after cable reconnect. Normal
Android restart and guarded radio-off/software-renderer recovery pending.
Details and evidence: `docs/radio-cellinfo-fix-20261002.md`.
