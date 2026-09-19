# V29: staged CMD0 experiment

V28 completed and returned to stock Android; host services were restored at 19:02:54.514400 UTC. Its final session is archived. Command setup and USB logging survived with CMD0 deliberately held before the command-register write. This narrows the fault to issuing CMD0 or the immediate response; it does not identify the failing instruction or prove storage initialization.

## Research before this test

Targeted searches covered SDHCI-MSM/SDM660 CMD0 crashes, command interrupts, interrupt signal masking and runtime-PM failures. No confirmed fix for this exact failure was found.

- [2024 SDHCI-MSM suspended-controller/LED patch discussion](https://patchew.org/linux/20240321-sdhci-mmc-suspend-v1-1-fbc555a64400%408devices.com/): a real related class of register-access crashes. The relevant runtime-suspended handling is already present, and this diagnostic disables the optional activity LED. V28 also survived that request setup, so this is not a demonstrated match.
- [2018 SDCC5 power-control crash report](https://lkml.indiana.edu/hypermail/linux/kernel/1810.0/05676.html), [Qualcomm reply](https://lists.openwall.net/linux-kernel/2018/10/09/159) and [reporter follow-up](https://lists.openwall.net/linux-kernel/2018/10/09/165): different failure point, no confirmed resolution in that exchange. Our merged DT mapping was checked during V26.
- [Qualcomm downstream SDHCI implementation](https://android.googlesource.com/kernel/msm.git/%2B/17d718ca1528f48b1853585fef11359aa899dae1/drivers/mmc/host/sdhci.c) and [upstream SDHCI definitions](https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci.h) provide the register/status model. V29 is a diagnostic inference from the V28 control, not a published workaround.

## One boot, conditional stages

The first CMD0 is held as in V28, with its driver timer canceled. Four-second delayed work gives the independent RAM logger time to deliver evidence. No sleep occurs under the host spinlock.

1. Read standard SDHCI controller registers and software clock/power/IRQ configuration. Reject stale command status or invalid values.
2. Mask interrupt **signaling** and verify readback. Leave status latching (`INT_ENABLE`) untouched.
3. After four seconds, recheck the state and issue CMD0 exactly once. Log before and after the write.
4. After four seconds, inspect latched status. An error or invalid registers ends the sequence with the request deliberately pending and logging alive. No completion gets one further status-only poll four seconds later, then stops.
5. A successful completion gets another four-second evidence window, then restores the original signal mask. The normal IRQ handler consumes the completion. No synthetic completion, status acknowledgement, retry or reset is introduced.
6. Bounded command issue/completion and IRQ traces follow subsequent ordinary initialization in the same boot. There are no controller reads or saved-command dereferences by the staged worker after restoring interrupts.

The old 250 ms pauses at already-validated setup checkpoints are removed; checkpoint text remains. This timing change must be considered when comparing against V28. The RAM init, device tree, regulator constraints and module remain unchanged. No persistent filesystem is mounted and no raw block-write path is added.

The singleton worker is deliberately limited to the opted-in A6L physical controller. Its host and pending request remain alive in this fixed RAM probe, which never unbinds the host or unloads its module. This is not production-quality hot-unplug support and must not be upstreamed as a fix.

## Interpretation

| Last evidence | What the next investigation targets |
|---|---|
| CMD0 issue begins, no returned marker despite previously streamed baseline | Command write or immediate hardware reaction; signal masking alone did not prevent it. Buffered output loss still limits exact localization. |
| Issue returned, readable command error or timeout | Controller/card/clock/power configuration; inspect saved status rather than repeating the same test. |
| Polled success, failure when interrupts are restored | Interrupt delivery/handler/completion path is the leading suspect. |
| IRQ completion and later commands appear | Follow the first later failing operation, using the additional command trace from this same boot. |
| Card identification/block-device messages appear | Storage discovery is a new milestone; it still does not prove Android compatibility. |

## Validation

Kernel and matching module build passed. Diskless QEMU module load passed. The actual staged C code passed 13 fake-register scenarios covering success, errors, timeout, delayed success, stale state, invalid reads, request lifetime changes and runtime suspension. The model verifies at most one CMD0 write, unchanged status-enable mask and no interrupt-status acknowledgement. It cannot validate physical register semantics, IRQ concurrency or electrical behavior.

Full RAM emulator, image packaging and captured bootloader checks follow before any physical installation.

## Workflow going forward

At the user's request, each diagnostic should include a brief search of primary sources, a written hypothesis and outcome branches, streamed evidence before hazardous transitions, and safe follow-on tests when an earlier stage succeeds. Retain only necessary short waits. A physical reset ends that boot; additional branches cannot execute afterward. Prepare the next branch in advance where evidence permits, without blindly trying multiple voltage or destructive recovery changes.

Full RAM QEMU and captured ABL checks passed. Packaged candidate `95b98dd09d5f12703553640ecb0cf6aa109e3ef6ad7bbd98698385d2dfc9d7b6`; adapted kernel `39f8a4b4958294650b5ab05d25829efc66e3168912d36e171e5833a5416cf179`. RAM and module byte-identical to V28. Six writer protocol tests, four recovery transition tests and seven collector timing/branch checks passed. Collector requires a stage outcome plus heartbeat at least 32 seconds after the actual module fork; menu wait remains independent of the 65-second capture window. All nine staged tools and candidate hashes match; remote offline inspector passed.

Installation launched 19:24:52.971522 UTC, PID 155143 under GNOME suspend inhibitor. Do not retry the one-shot installation. Await full readback completion before asking for Power/Android.

Installed 19:25:24.124491 UTC. All twelve copied readbacks passed independent desktop verification. Exact V28 predecessor and known boot-control message preserved; poweroff acknowledged, EDL absent, services restored. User asked Power to stock Android; waiting for reply. No filming needed. Installation USED; capture and restore UNUSED.

Android baseline confirmed. V29 capture launched 19:27:21.463191 UTC PID155665; exact fastboot 18d1:d00d/1e529013 and logger confirmed. User asked Recovery, no filming, ~60 seconds. Capture USED; await selection/result. Host services owned by capture coordinator until Android return.

## Physical result and cleanup

Capture finished 19:28:54.705014 UTC after USB disappeared at 19:28:28.374164. 197768 bytes, SHA256 `8f6f4625371949cc1bf907af433a56db149cc82d6de37cf5726dba49b8f22d09`. Baseline status=0, present=03f800f0, signal/enable=00ff1003, clock=0007, power=0d, command/argument=0; reported/actual clock400000, IRQ106. Mask readback passed at kernel31.229838s; USB heartbeat continued at33.031399s (userspace28). No before_issue snapshot, issue-return marker, IRQ or completion evidence arrived. No panic or block-device identification.

This is not proof CMD0 was never issued: the next worker callback reads the snapshot and then writes the command without a delivery interval. A reset during either operation can lose all that callback's text. The next image should split pre-issue snapshot/validation and command commit into separate delayed callbacks; preserve the same success/error branches. Also record runtime-PM software state before accessing registers. Core __mmc_claim_host already retains a runtime-PM reference while the rescan request is pending, so a missing PM reference is not established as the cause. Source inspection/search found no exact matching fix.

Stock Android return and service cleanup verified 19:30:11.206701 UTC; final session archived. V29 remains installed, no V30 changes/image yet.
