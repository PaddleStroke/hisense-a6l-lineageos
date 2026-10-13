# Visible RAM userspace diagnostic (V10)

Active V10 trial (2026-09-16 05:39 UTC): diagnostic recovery installed and all twelve desktop readbacks verified. Original Android returned. V10 Capture USED/running (Windows PID 2060); fastboot confirmed 05:39:42 UTC. User asked to record and select Recovery once, then leave buttons alone. Await physical results; V10 Restore UNUSED. Stock recovery is not installed. Inspect capture-probe-serial-v10 before any subsequent phone operation.


Current state (2026-09-16 05:37 UTC): V10 diagnostic recovery installed; complete readback passed, spare powered off and laptop services restored. User asked to start normal Android with Power. V10 Install USED; Capture/Restore UNUSED. Stock recovery is NOT installed. Desktop readback transfer in progress.


V9 physically completed kernel initialization and attempted /init. The LCD
remained active for four minutes, with kernel messages through ~38 seconds;
USB did not enumerate. Status from the old PID1 was sent to /dev/console,
whose configured console was the UART. Actual console selection, blocking,
and successful old userspace execution are not established.

V10 changes only the static RAM diagnostic. Fresh status messages go to
/dev/kmsg, whose kernel console output reaches the existing early LCD.
Replayed kernel records only enter the bounded RAM journal, never kmsg.
The fallback /dev/console is opened only if kmsg cannot be opened. The
runtime printk_devkmsg setting is set to on to avoid dropping short bursts.
Debugfs is mounted read-only; devices_deferred snapshots are bounded to
64 lines and scheduled at startup, ~20 and ~60 loop-seconds. No block devices
are opened or persistent filesystems mounted by this PID1.

The final incremental build passed. Diskless QEMU passed init/READY/ALIVE,
read-only debugfs and dependency snapshots, no feedback or logging drops,
no panic, and framebuffer bounds. Decoding saved framebuffer pixels with
the kernel's VGA 8x16 font confirmed READY, ALIVE and deferred-device text
was rendered. QEMU does not emulate the A6L USB controller or PMIC.

The V9 kernel, DT, overlay, boot command line and header load addresses are
unchanged. The package roundtrip passed with only ramdisk size/digest and
following DTBO offset fields permitted to differ. Captured ABL header,
overlay, fixup, decompression, selection and unlocked AVB checks passed.
Six guarded-write protocol fault-injection tests passed.

Kernel SHA256: 4cc5b6b159bdcef531a4e2916d2e4b7a40598e666f11b61625901d8f41a62ca8
RAM disk SHA256: 67dd8512f380bb6dc53febddb39e2bb29424b88437bc0af4aa73c6c718a8447a
Recovery SHA256: 19027fa8401d368783e181376494a60761bd6805fb64688af970ccba6c3368c2

Artifacts are under firmware/extracted/visible-userspace-ramdisk-20260915,
visible-userspace-probe-20260915 and recovery-probe-visible-userspace-20260915.
Guarded physical tools use DiagnosticRecoveryProtocolV9 and V10 coordinators.
The preparation-checkpoint.json records the latest installation state.

V9 stock restoration and all twelve desktop readbacks passed; original
Android was verified after stock recovery Reboot at 19:31:06.828432 UTC.
V10 staging and USB-disabled laptop preflight passed. The installation launch
was interrupted before execution; no V10 capture/process/terminal exists.
All V10 coordinators remain unused; stock recovery remains installed.
The user is unavailable overnight. Resume from docs/resume-next-session.md.
