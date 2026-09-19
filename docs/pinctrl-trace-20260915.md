# Pin-controller diagnostic — 2026-09-15

The v5 recording's newest line is `[0.156396] A6L probe begin 3100000.pinctrl
driver sdm660-pinctrl`. The user confirmed no subsequent button press, and USB
returned as Android 100.32 seconds after leaving fastboot. This confirms an
autonomous restart for this attempt, not whether a watchdog, secure firmware,
or another mechanism triggered it. The post-boot reason is `bootloader`.
No diagnostic USB or RAM PID1 execution has been observed on the phone.

Stock recovery was fully restored/read back at 16:27:02 UTC; original Android
and services were verified at 16:27:26 UTC. All twelve before/after region files
passed independent desktop hashes. Misc retained `bootonce-bootloader` across
restoration. The stock-recovery normal reboot path must clear this before the
next install; retain the zero-BCB guard. All v5 coordinators are USED.

## Pinned-source findings

The previous marker is outside `really_probe`, before supplier checks, pin
binding, DMA setup, and the driver callback. It therefore does not prove entry
to `msm_pinctrl_probe`. That callback maps controller resources, registers
restart handlers, sets up IRQ/pinctrl, then registers the GPIO chip.

`drivers/gpio/gpiolib.c` calls `get_direction` for every valid GPIO during
registration. SDM660 declares 114 GPIOs; `msm_gpio_get_direction` performs an
existing control-register read. A blocked or invalid register access is a
hypothesis to locate, not yet an established cause. No GPIO reservation mask
has been guessed or changed.

## Instrumentation

`tools/Instrument-A6LPinctrl.py` adds exact-board/initcall-debug-gated messages
at callback entry, setup stages, and before/after the existing GPIO control
read. It preserves all original code and proves reversing the additions
recovers the pinned source exactly. No extra MMIO access, register value, DT,
ramdisk, or console-renderer change is made. Source snapshots, hashes and the
reviewed diff are in `firmware/extracted/pinctrl-trace-source-20260915/`.

The first instrumentation invocation stopped before mutation because it expected
a GPL export marker instead of the actual export marker. The corrected script
ran successfully and the incremental build passed. A diskless QEMU boot
regression is running under `tools/Test-A6LPinctrlTrace.py`. This checks the
new kernel still reaches PID1/heartbeats; it does not emulate Qualcomm pinctrl
or exercise the newly instrumented hardware path. Earlier unchanged framebuffer
board/reservation negative tests remain available from the v5 build.


## V6 candidate

Build, diskless PID1/heartbeat regression, packaging roundtrip, captured ABL
header/decompression/DT/AVB checks and six fixed-write fault tests all passed.
Recovery SHA256: `ef81c365f79648b66257d2ce55f2553b829629157874c629d0bad7899abfb7e2`.
Kernel SHA256: `1ed2b850286a8024e5faa0393c4e2465cc9bcf30206c56089054327c8c0f59bb`.
The candidate is in `firmware/extracted/recovery-probe-pinctrl-20260915/`.
V6 physical tools use DiagnosticRecoveryProtocolV5, unchanged geometry/guards,
and fresh capture directories. They reject the prior trace image as well as
older candidates and cross-mode payloads. Staging is in progress; no v6 write
has started.

At 16:35:43 UTC one guarded adb restart into verified stock recovery succeeded.
`capture-stock-recovery-clear-v2/session.json` records the exact original Android
baseline and command. The user has been asked to enter recovery and select
Reboot. Android return and the install worker's zero-BCB check remain pending.
