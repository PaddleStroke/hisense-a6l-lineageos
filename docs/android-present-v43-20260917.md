# V43: compositor-presented frames

V42 completed with the exact stock Android return and host cleanup verified
at 2026-09-17T15:59:53.656516Z. Its completed session report is archived.
The spare remains on stock Android with the existing V38 recovery.

V43 extends the successful private RAM graphics setup with actual HWC3
presentation: active configuration, display-on request, two client-target
slots, one CLIENT layer, validation/acceptance, four presentations and fence
checks. It paints red, green, blue, white and black bars above a gray gradient
and moves a white marker. The final image is held for eight seconds.

Before a phone run, diskless QEMU must pass the existing service/allocation
checks plus four presentations. QMP reads the fixed framebuffer while the
image is held; the harness compares every pixel with the expected final
image and checks the adjacent 4 KiB for overwrite. This is stronger than
accepting success return codes alone. The existing kernel and guarded V40
adapter are unchanged. The test has no persistent block-device access.

Files: diagnostic/present_services.c, diagnostic/present_client.cpp,
tools/build-android-present-v43.sh, tools/Test-AndroidPresentV43.py.
Initial build passed in 4m44s. QEMU r1 rejected the RGBA client target at
ADDFB2: unsupported DRM AB24. R2 switched only presentation buffers to BGRA
(the independent allocation/mapper test still uses RGBA); the kernel then
rejected AR24. The pinned simpleDRM format-list builder deliberately exposes
opaque XRGB8888 only for this framebuffer. Both failed attempts are archived.

The composer is being adapted to import only its fully composed BGRA
client-target view as XRGB on simpleDRM, with blending disabled for that
opaque primary plane. Application metadata and other layer/backend paths
are unchanged. Source/patch evidence is in research/android-present-v43-20260917.
Build r3 is in progress; no V43 phone test or staging has happened.

SurfaceFlinger's pinned source supports
`ro.surface_flinger.default_composition_pixel_format`; the future software
product can select BGRA8888 through this existing setting (value 5), subject
to renderer validation. No SurfaceFlinger runtime has been tested yet.

## Related panel-driver lead

The stock merged device tree names the LCD `ft8719 tianma 1080x2340 video`.
Our pinned kernel contains `drivers/gpu/drm/panel/panel-ebbg-ft8719.c`.
Upstream describes its supported panel as the EBBG FT8719, mainly used on
the Poco F1, with resolution 1080x2246. This is a useful controller-family
reference, not evidence of an interchangeable panel. Compare DSI command
sequences, timing, reset GPIOs, supplies and backlight wiring with the A6L
stock tree before developing the full A6L panel driver. Current simpleDRM
tests intentionally keep the bootloader's existing display initialization.

Local comparison confirms concrete differences: A6L uses horizontal porches
36/32 and a 4-pixel sync pulse versus EBBG 28/16/4; both list vertical
120/12/4 at 60 Hz but differ in active height. A6L's stock on-sequence sends
FT8719 vendor-page/timing commands (FF 87 19, C0, CF), then sleep-out with a
120 ms wait. EBBG's driver sets brightness/control/power-save and waits 90 ms.
A6L uses PMIC WLED backlight control. These differences are reasons to port
the recovered A6L configuration, not just enable the other panel's compatible.

References:
- https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/panel/Kconfig
- https://github.com/torvalds/linux/blob/master/drivers/gpu/drm/panel/panel-ebbg-ft8719.c
- Pinned local HWC3 AIDL DisplayCommand, ClientTarget, Buffer, CommandResultPayload
  and IComposerClient definitions and drm_hwcomposer implementation.

R3 build passed (2m31s). Diskless QEMU passed all checks, including four presentations, all final scanout pixels and untouched adjacent 4 KiB. The package has been staged and verified on the laptop. V43 capture launched at 16:27:07 UTC; fastboot and logger confirmed, awaiting manual Recovery selection. No firmware writes. Physical frame presentation remains pending.

Physical V43 PASS at 2026-09-17T16:29:08.231050Z. All 74 transferred files verified; actual allocator/mapper/HWC3, native display configuration and four presentations passed. Command errors and fences checked. ADB alive, global mounts unchanged, readiness property and console verbosity restored. Logs archived in captures/capture-present-user-v43. Visual colour-order confirmation and stock Android return/host cleanup pending. No SurfaceFlinger runtime or GPU acceleration claim.

User confirmed correct physical colours and gradient. Exact stock Android return and laptop service cleanup verified at 16:30:51.560414 UTC; final session archived. User requested removing the repeated long startup capture. Prepared Wait-AndroidRamReady.py for the next coordinator: authenticated V38 identity/security/root/virtual-mount readiness sustained four seconds. Syntax checked only; not yet staged or runtime-tested. Existing full serial/storage collector remains available for regressions.
