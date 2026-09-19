# V40: live simpleDRM display integration

Final result: the physical test passed and the user confirmed that colour bars
and the gradient appeared. Individual red/blue ordering was not explicitly
confirmed. Normal stock Android and host-service cleanup were verified at
14:30:07 UTC. The original coordinator had timed out while the diagnostic was
still running; its report is preserved, and the later successful cleanup is
recorded separately in `post-return-verification.json`. V38 recovery remains
installed; the RAM-loaded display adapter disappeared on reboot.

V39 established Android Binder communication and read-only stock filesystem
access. V40 tests the next LCD interface without replacing V38 recovery or
flashing anything. The phone starts the existing diagnostic, and authenticated
ADB transfers two files into RAM.

The small `a6l_simplefb` module registers the existing reserved boot framebuffer
with the kernel's built-in simpleDRM driver. It accepts only the A6L compatible
string and exact reserved region at 0x9d400000, size 0x23ff000, with `no-map`.
The exposed resource is limited to 1080 × 2340 × 4 bytes, stride 4320. The module
offers no address/size parameters and does not program display-controller,
PMIC, e-ink or storage registers. The assumed colour layout remains to be
checked on the physical LCD.

The static Android/libdrm probe requires V38 root, tmpfs staging and exactly
one simpleDRM device. It creates its device node in a private mount namespace
and device-enabled tmpfs, since the diagnostic's outer /tmp is mounted nodev.
It checks two dumb buffers, PRIME export/import between two DRM clients,
atomic test-only/modeset and eight framebuffer updates. The expected image has
red, green, blue, white and black bars above a gradient and a moving white
marker. It holds the final image for twelve seconds, restores the previous
CRTC and releases its buffers/private mount. The host temporarily lowers the
console log level to keep diagnostic text off the image, then restores it.

## Offline verification

`firmware/extracted/android-display-v40-20260917-r1/report.json` passed all
three diskless ARM QEMU cases with the exact installed kernel:

- Matching board/reservation: actual simpleDRM buffer allocation, PRIME,
  atomic updates and cleanup pass. Sampled colour and gradient pixels match;
  the memory immediately beyond the exposed framebuffer remains unchanged.
- Wrong board: module rejected, framebuffer untouched.
- Undersized reservation: module rejected, framebuffer untouched.

Both payload source copies match their build inputs. The original V38 kernel
Image is unchanged. Source hashes for the relevant simpleDRM and Android
graphics code are recorded in `research/android-display-v40-20260917`.

## Physical run

Capture coordinator started at 13:49:49 UTC, PID 216165. Fastboot identity and
collector PID 216247 were confirmed before asking for Recovery selection.
Two payload hashes, the QEMU report and both runner scripts were checked on
the laptop, along with the nine preserved V38 tools. Physical probe pending.

## Following graphics work

The checked-out Android tree includes `vulkan.pastel` (SwiftShader CPU Vulkan),
ANGLE, `android.hardware.composer.hwc3-service.drm` and minigbm. The current
minigbm backend list does not recognize simpleDRM, although its common dumb
buffer backend can be adapted. Its Android allocator already supports trying
primary card nodes when no render node is available and recognizes pastel as
software rendering. These are source findings, not a working Android GUI.

Next integration must add the allocator backend and product/vendor service
configuration, verify rendering, and bring up core framework/APEX services.
LCD power management, touch, hardware acceleration and e-ink remain separate.

Reference checked: [Linux DRM userspace API](https://cdn.kernel.org/doc/html/latest/gpu/drm-uapi.html).

Physical software result at 14:06:00 UTC: simpleDRM card0/fb0 registered; two buffers, PRIME export/import, atomic modeset and eight updates passed. Cleanup, unchanged global mounts, continued ADB and console-loglevel restoration passed. Evidence: captures/capture-display-user-v40/display. User visual confirmation and normal-Android return are pending. The adapter remains loaded in RAM until reboot.
