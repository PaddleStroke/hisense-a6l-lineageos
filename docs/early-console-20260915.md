# Early LCD console diagnostic — 2026-09-15

The corrected symbols/fixups image passed the captured bootloader but produced
no USB logs on the phone. Its boot logo disappeared; Android later returned
after the user pressed Power at an uncertain time. Stock recovery was restored
and Android/services verified at 13:41:59 UTC. This does not establish kernel
execution or an autonomous watchdog reset.

## Investigation

The captured stock kernel has no PSTORE or DEVMEM. Normal ADB cannot read
`/proc/last_kmsg`, framebuffer attributes or `/proc/iomem`. A fresh read-only
laptop capture is `capture-early-log-baseline-v1/report.json`.

The new kernel already includes DWC3, both Qualcomm glue implementations, QUSB2,
QFPROM, GCC, SPMI, RPM regulators and SMEM/hwspinlock. Its nested `snps,dwc3`
node binds the built-in `core.c` driver; `USB_DWC3_GENERIC_PLAT=m` is not the
missing binding. QCOM_WDT=m is not by itself a cause: this DT has no watchdog
device node. No speculative USB or watchdog changes were made.

## New console

`device/hisense/a6l/kernel/a6l_earlycon.c` adds `earlycon=a6lfb`. It requires the
exact Hisense board compatible and exact no-map framebuffer reservation before
mapping any memory. It writes only the first 1080*2340*4 bytes of the captured
reservation at 0x9d400000 (size 0x23ff000). It does not access display-controller,
PMIC, e-ink or storage registers. The runtime mapping lifecycle follows the
pinned kernel's EFI early console. A doubled 8x16 font yields 16x32 cells;
text rows wrap to the top instead of scrolling the full framebuffer.

The reservation and 1080x2340 video-mode LCD are from the captured A6L DT. The
32-bit pixels and 1080*4 stride are an explicit physical-test assumption, supported
by the same-SoC Xiaomi Jasmine and Motorola Beckham framebuffer descriptions in
the pinned kernel source. This is not yet measured on the A6L boot display.

`tools/build-linux-earlycon.sh` incrementally builds the prototype, adding this
one source to `drivers/tty/serial/Makefile`. Kernel configuration, device tree,
overlay and static RAM diagnostic are unchanged. The header retains the measured
0x80000 load offset. The command line enables the new console, `keep_bootcon`
and `initcall_debug`; `panic=0` retains a panic screen instead of automatically
rebooting. Physical return still uses the spare's Power/bootloader route.

## Verification and candidate

Diskless QEMU cases all passed: correct board/reservation produces readable
kernel text and running PID1, while wrong board and wrong reservation write
nothing. The 4 KiB immediately after the allowed pixel area remains untouched.
The actual emulated framebuffer was saved and visually inspected. These tests
do not emulate the A6L display scanout, PMIC or USB controller.

Artifacts: `firmware/extracted/earlycon-probe-20260915/`.
Adapted kernel SHA256:
`b96c37874a0585f7804886d09d68aafa790b80cc89088af689f4ac0f119153fd`.
Packaged recovery: `firmware/extracted/recovery-probe-earlycon-20260915/`, SHA256
`fa58e7102ed18480a8f248bbc5e0e97fc22e53600856120801d0bfde7747a79c`.
Body size remains 11,993,088 bytes, padded to the exact 64 MiB partition.

Captured ABL header, gzip decompression, DT selection/overlay/fixups and AVB
checks passed, with negative controls retained. Six write-boundary fault tests
passed. New v4 coordinators use `DiagnosticRecoveryProtocolV3.py`, preserve
the one-recovery-partition write boundary and full independent readback, and
reject earlier images. Launch with `Run-A6LDiagnosticV4.ps1 -Action Install`,
then Capture and Restore, only with fresh matching capture directories.

The candidate was installed and fully read back at 14:04:41 UTC. All twelve
before/after region files were independently verified on the desktop.

The first capture (`capture-probe-serial-v4`) did not observe fastboot or a
Recovery selection after the cold Volume Up + USB attempt. The user saw black,
then held Power as instructed; original Android and laptop services were verified
again at 14:09:30 UTC. This is an unobserved entry attempt, not evidence of a
kernel boot failure. The candidate remains installed in recovery.

Retry `capture-probe-serial-v4b` uses the verified original Android baseline and
one `adb reboot bootloader` command, with no image writes. Its observer started
at 14:15:05 UTC and recorded the exact spare in fastboot at 14:15:10 UTC. The
user selected Recovery and reported white text, then said it disappeared. The
text has not yet been captured or transcribed, and no diagnostic USB appeared.
After an instructed Power restart, stock Android USB reappeared at 14:17:17 UTC
(ADB configuration at 14:17:24 UTC); the user confirmed Android started.

A fresh v4c capture is prepared for the user-requested repeat with a camera.
It changes only the capture directory from v4b, repeats the same guarded
Android-to-bootloader restart, and performs no image writes. The existing v4b
coordinator must finish and restore services before v4c is launched. Final
physical results and stock restoration remain pending. The v4 Restore action
is still unused; completed capture directories may not be reused.

## Physical evidence and restoration

The v4c trial was explicitly selected at 14:21:11 UTC. The user supplied a
video frame and confirmed it is the last readable text before black. The image
is preserved at `captures/early-console-screen-20260915/frame-01.jpg` with a
transcription and source interpretation in `analysis.json`. It proves the
prototype kernel executes on the A6L and the early framebuffer console works.
The newest legible line is `[0.153186] probe of 1f40000.hwlock returned 0`;
older lines remain below it because the renderer wraps. SMEM's earlier -517
is EPROBE_DEFER, not a demonstrated fatal error. No panic is visible. No
diagnostic PID1 or USB gadget execution has yet been established physically.

The observer recorded Android USB return at 14:22:51 UTC and verified original
Android/services at 14:23:57 UTC. All three v4/v4b/v4c capture directories were
copied locally under `captures/` with their original names. The complete stock
recovery was restored/read back at 14:42:54 UTC; Android and services passed
at 14:43:18 UTC. Twelve local before/after files passed independent hashes.
The v4 Restore action is now USED. Current recovery is stock.

Misc contained `bootonce-bootloader` before and after this restoration (rather
than the earlier all-zero contents). The restore changed only recovery. This
new observation supersedes any claim that the Android-to-bootloader route leaves
the earlier BCB baseline unchanged. No misc write was issued by our tools.

## Next diagnostic: probe-entry logging

The pinned `drivers/base/dd.c` function `really_probe_debug` prints only after
`really_probe` returns. `tools/build-linux-probe-trace.sh` adds a single debug
line with the device and driver names before that call, under the existing
`initcall_debug` path. No driver behavior, DT wiring, or console renderer is
changed. This can expose the next entered operation without assuming the last
successful hardware-lock probe caused the blank screen.

The incremental build passed. The first diskless emulator test reached PID1 at
about 32 seconds with working trace text and intact framebuffer bounds, but
its 35-second timeout did not allow the two required 10-second heartbeats.
That failed test remains in `earlycon-trace-probe-20260915`. A second test in
`earlycon-trace-probe-v2-20260915` used a 75-second bound but encountered an
OS read error on the actively written Windows-shared log. The third run uses
Linux ext4 for live logs and archives completed results into
`earlycon-trace-probe-v3-20260915`; results are pending. No trace image has been
installed or staged for a physical write. The approval service initially rejected
this third invocation due to a reported usage limit; a fresh usage check allowed
execution and the same command was subsequently approved.


## Trace candidate ready offline

The third emulator run passed all three cases: probe-entry messages, PID1 READY,
two ALIVE messages, no panic, correct board/reservation gating, and untouched
framebuffer tail. Its archive initially hit Windows metadata-copy errors; all
26 files were subsequently compared byte-for-byte with the completed ext4
results and verified. See `earlycon-trace-probe-v3-20260915/archive-verification.json`.
No extra emulator run was needed for the archive-only failure.

Trace kernel SHA256:
`010467da4cf22105850d63d64ac5af5d48ad96132f1a98a1afb6eab17ddf3c68`.
Trace recovery SHA256:
`886baf8c66f85b199e3e0d8449731e5e98488d7c38641e7942f31ce54a216c38`.
Artifact directory: `firmware/extracted/recovery-probe-trace-20260915/`.
Packaging roundtrip and captured ABL header/decompression/DT/AVB checks all
passed. The DT, ramdisk and command line are unchanged from the photographed
candidate; only the kernel adds the probe-entry printk. All six fixed-write
fault tests passed with the new hash in DiagnosticRecoveryProtocolV4.

New v5 install/restore/capture coordinators and their manifest are prepared.
Install still requires a completely zero 4 KiB boot message. Before attempting
it, use original stock recovery's normal reboot route to clear the captured
bootonce-bootloader message; the install worker must verify this rather than
relaxing its guard. No manual misc write is prepared. Physical user readiness
was requested and remains pending. V5 capture expects Android running, so after
its install/poweroff the spare must be started normally before the capture
coordinator sends adb reboot bootloader. No v5 phone operation has run.

All seven staged v5 Python files match the local manifest, and the complete
trace image hash matches. Laptop `diagnostic-v5-preflight.json` passed its
no-USB import, image rejection and fixed-geometry checks. Phone operations
remain unstarted pending the stock-recovery reboot step and user readiness.
