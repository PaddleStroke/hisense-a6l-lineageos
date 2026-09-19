# E-ink library ABI preparation

The stock 64-bit `libtcon_eink.so` successfully loads against the prepared modern
Android library closure in diskless AArch64 QEMU. Its version function returns
2.2 and preserves surrounding output-buffer canaries. Four checks passed.

Evidence lives in `firmware/extracted/eink-abi-20260917`: manifest, copied
original library and modern dependencies, QEMU log and report. Source:
`device/hisense/a6l/diagnostic/eink_abi_probe.c`; build/QEMU helpers are
`tools/build-android-eink-abi.sh` and `tools/Test-EinkAbiQemu.py`.

Disassembly of `ReportEinkSWTconLibVersion` at 0x2e30 shows two 32-bit stores of
the value 2 through its two argument pointers, followed by return. The harness
uses this verified ABI, resolves four exports, and calls only that version
function after `dlopen(..., RTLD_NOW)`. Neither panel initialization nor waveform
conversion nor panel output is invoked. The new binary built successfully and
the QEMU report records success, clean exit, completion and no panic.

This proves a small but useful part of proprietary software reuse: dependency
resolution and one verified function work in the new userspace environment.
It does not establish compatibility of the entire TCON library or driver.

Next offline work is to recover the initialization and frame-conversion ABI
and their buffer/format requirements from the original callers. A bounded
guarded-memory harness can then exercise pure conversion functions, if their
side effects can be established. Physical work must also address panel power,
DSI/bridge transport, and the separate SPI waveform/calibration data: the eMMC
backup does not contain that SPI storage and it has not yet been read back.
