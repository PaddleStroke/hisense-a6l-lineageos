# V16: short USB connection trace

The saved V15 frame proves the disconnected, bound USB driver remained alive
through the 120-second heartbeat. The explicit connection returned zero and
the RAM program printed `A6L_USB_CONNECT_END success=1` at kernel125.067760.
The laptop saw no diagnostic device. This narrows the next investigation to
activity after connection; it does not identify an exact panic or hardware fault.

The user requested removal of long recording waits. V16 configures USB as soon
as RAM userspace is ready, binds at4seconds, connects at8seconds and opens serial
at12seconds. These times are relative to RAM userspace startup, after kernel boot.
Record approximately25–30seconds from selecting Recovery. Heartbeats remain at
two-second intervals. The passive host logger stops45seconds after departure
from fastboot, or earlier once the complete serial milestone is received; its
longer outer deadline is only an allowance for selecting the bootloader menu.

V16 retains V15's manual connection gate and adds `a6l_usb_trace=1`. The new
trace is also board gated to `hisense,hlte730t`. It logs the first eight IRQ and
thread-handler entries/exits, the existing event-count read and buffer copy,
and the first16 decoded event entries/exits. There are no extra MMIO reads,
no event suppression, no IRQ masking changes and no added delays. Printk can
nevertheless perturb interrupt timing; a changed outcome must be interpreted
with that limitation. A lack of printed IRQs would not alone prove no IRQ occurred.

The kernel configuration and device tree remain unchanged. No regulator values
or persistent data paths are changed. The shortened stage timings and bounded
logging are diagnostic changes, not a claimed USB fix.

Patch: `device/hisense/a6l/kernel/a6l-usb-event-trace.patch`.
Relocated kernel SHA256:
`7bb565ac584a8ec05fa95145e22912a018c240bef7ffd7665dc62113f6587b70`.

V15 remains installed while V16 is built and validated. Its second capture
finished with no USB bytes; normal Android and host service restoration were
verified at11:51:56UTC. Next replacement permits only exact stock or the known
complete V15 recovery and preserves the exact known boot message.

Final RAM program build passed. Ramdisk SHA256:
`74001cd3ac32ca6875fdfae532b6b43bee00467b6f9550d8258f7d2bcf0860a5`.
The archived source matches the workspace. QEMU validation is in progress.

## Validation and installed state

Diskless QEMU reached the22-second RAM heartbeat; packaging round-trip and
captured ABL checks passed. Six programming-protocol tests and four exact
predecessor/BCB checks passed, followed by laptop staging hashes and offline
preflight. These do not validate Qualcomm hardware interrupt handling.

V16 installed at12:05:16UTC from exact V15, with known bootloader BCB unchanged.
All12 copied readbacks passed independent desktop verification. Candidate:
`5b28bc987c7f0962662d5a7b72693d177b456405b20f998fc3235576225317f5`.
Phone powered off; awaiting normal Android startup before the physical test.
