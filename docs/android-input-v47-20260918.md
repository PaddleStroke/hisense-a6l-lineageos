# V47: interactive native Android input and rendering

Preparation only until the evidence below is updated. The phone retains the
verified V46 recovery and boots stock Android; no V47 recovery image is needed.

The test reuses the validated SurfaceFlinger/ANGLE/SwiftShader/DRM pipeline and
adds real EventHub, InputReader, InputDispatcher and InputChannel consumption.
A native client publishes a full-screen input window to SurfaceFlinger, displays
a yellow marker following the first finger, and marks each screen quadrant
green after a tap. Pointer counts and coordinates are logged, and every delivered
event is acknowledged. This is a native diagnostic UI, not SystemUI or a launcher.

The private RAM root exposes exactly one identified front-touch evdev device.
It does not import the host /dev tree or add storage nodes. The IDC explicitly
declares an internal touchscreen. Logical viewport and display layer stack both
use display 0. The actual pinned InputDispatcher registers its own SurfaceFlinger
window-info listener; a duplicate forwarding listener is unnecessary. Dispatch
must explicitly be enabled because its constructor starts disabled.

The emulator-only uinput fixture creates a two-slot direct touchscreen, sends
four quadrant taps and a two-finger gesture, then requests completion. It is not
part of the transferred phone payload and refuses non-QEMU device trees. Checks
require actual Android motion delivery, expected coordinates and multitouch,
touch-driven framebuffer pixels, service health, unchanged global mounts and
restored properties. The fixture uses the existing CONFIG_INPUT_UINPUT kernel
support, so no kernel rebuild is planned.

The supervised client has a deadline and an explicit `/logs/finish` completion
marker. On the physical phone, the host waits for `/logs/input-ready` before
asking for taps, swipes and two fingers; it sets the finish marker after the
attended test. No filming or repeated storage benchmark is needed.

Source basis: the pinned `InputReader`, `InputDispatcher`, and
`EndToEndNativeInputTest.cpp`, plus the
[AOSP input architecture](https://source.android.com/docs/core/interaction/input).
The independent API review is in
`research/controls-followup-20260918/input-api-review.md`; primary review corrected
header paths, dispatch enablement, layer cropping and duplicate listener setup.

Build r5 passed. Packaging attempt 1 stopped on the StatsD APEX dependency;
the dependency closure now checks the built `com.android.os.statsd/lib64` directory.
Emulator attempt 2 stopped at the 180-second client deadline before input readiness.
Its preserved Android log pinpoints `ACCESS_SURFACE_FLINGER` permission lookup
from UID 0 when InputDispatcher registers its window listener. The minimal RAM
root intentionally has no framework PermissionController service.

The correction runs only the interactive client as Android's system UID/GID 1000
with supplementary groups cleared. SurfaceFlinger explicitly accepts that identity
for listener registration. Its private evdev/DRM/Binder nodes and log socket are
owned by system; other services remain as previously configured. A separate root
helper performs the final SurfaceFlinger dump on request while the UI is still
alive. No production permission checks or system policies are patched.

Build r6 passed. Emulator attempt 3 exposed restrictive copied-payload permissions;
attempt 4 logged the client executable as mode 0700 before normalization and
reached `A6L_INPUT_START uid=1000` after correcting the payload permissions.
The supervisor also now uses explicit umask 022 for newly created private device
directories and restores PDEATHSIG after dropping client credentials. These are
RAM-test environment corrections; no phone hardware regression is established.

Build r7 passed and emulator attempt 5 reached EventHub construction. Its fatal
message precisely required `CAP_BLOCK_SUSPEND`. The client now retains only that
capability through exec (permitted/effective/inheritable/ambient), matching
EventHub's explicit requirement; supplementary groups remain empty. Standard
private null/zero/random devices also receive their intended mode 0666 explicitly.
Build r8 passed. Emulator attempt 6 proved that InputReader/Dispatcher received
the synthetic coordinates and both contacts, but no window was delivered to the
dispatcher. SurfaceFlinger gates window publication on its inputflinger service,
initialized by bootFinished. The native client now registers an actual
BnInputFlinger adapter forwarding to the real dispatcher and a window-owner death
token, then finishes graphics boot after posting its first real buffer. This is
not a full WindowManager or Android framework boot.

Build r9 and emulator attempt 7 PASSED all 17 checks: four quadrant taps,
two-finger delivery, actual touch-driven scanout pixels, system-UID input client,
root dump helper, ANGLE/SwiftShader composition, zero composer commit failures,
service cleanup and restored readiness property. Evidence is archived under
firmware/extracted/android-input-v47-20260918-r7. The 170-file RAM payload was
transferred to the laptop and all 178 pins plus nine shared tools verified.
Physical touch/display validation is next; no new recovery image is needed.
New code and harnesses use V47 names; the validated V44 graphics and V46 recovery
artifacts are preserved.

## Physical result

V47 passed on the spare, 2026-09-18T10:15:20Z–10:16:43Z, using the unchanged
V46 recovery. All 170 RAM payload files were hash-verified after transfer.
Android delivered 227 motion events, including two simultaneous pointers and
all four quadrant taps. The user confirmed that the yellow marker followed
their finger and all four corner squares turned green.

The real SurfaceFlinger/ANGLE/SwiftShader path reported client composition and
zero composer commit failures. The supervised services exited successfully;
global mounts were unchanged, the readiness property and console log level were
restored, and authenticated ADB remained alive. This proves the native Android
input/rendering path, not a full framework/SystemUI/launcher boot.

Evidence: `captures/capture-input-user-v47/input/report.json`, client log,
InputReader/Dispatcher dump, SurfaceFlinger dump and dmesg. Stock Android identity,
boot completion and host-service restoration were verified at
2026-09-18T10:18:09.917645Z. Final session, user visual result and SHA-256 index are
archived alongside the capture. V46 recovery remains installed and unchanged.
