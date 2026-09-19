# V31 research and experimental rationale

V30 delivered the second SDHCI register snapshot with the device active, PM usage 1 and interrupt signaling masked, then disappeared around the later command-write callback. Buffered log loss prevents identifying the exact instruction.

Local stock binary evidence (verified raw-kernel ranges in the stock-storage-binary directory) shows dsb st before its command-register halfword store. The modern Qualcomm wrapper uses writew_relaxed, with no store barrier for CMD0 when req_type is zero. ARM64 wmb maps to dsb(st) in the actual build. This is a concrete ordering difference, not a diagnosed bug or a proven fix. Four-second gaps and successful register readbacks make simple pending-register-write explanations less compelling; failure of V31 should redirect investigation toward controller/card setup and hardware fault evidence, not more arbitrary delays.

The modern wrapper also conditionally accesses CDR/DLL before the command store. Its private data starts zeroed through mmc_alloc_host/kzalloc; use_cdr is assigned true only by tuning. V31 records the software flag at transfer setup (well before command issue) and preserves __sdhci_msm_check_write and all normal power side effects. No assertion that the path is inactive is made before physical evidence.

The bounded search found no exact known fix for this A6L/SDM660 CMD0 reset. A legacy 8-us CRC-transition workaround in other Qualcomm code concerns a subsequent CRC-bearing response, so it is not applicable to the first no-response CMD0. Do not add it speculatively.

Primary sources checked:
- https://docs.kernel.org/driver-api/device-io.html (ordered vs relaxed accessors and limits)
- https://kernel.googlesource.com/pub/scm/linux/kernel/git/rt/linux-rt-devel/+/fe35bf27a14ded5997d8ceee7f7b10a0982e41e4/drivers/mmc/host/sdhci-msm.c (Qualcomm command/CDR accessor)
- https://android.googlesource.com/kernel/msm.git/+/9671bc00773d7f73172f1cf38a7cd638091d0214/drivers/mmc/host/sdhci-msm.c (CRC-transition workaround, rejected as mismatched)
- Pinned Qualcomm 4.4 I/O header and stock disassembly already archived under research/stock-storage-binary-20260916.

V31 is an experiment: first-CMD0-only wmb, early software accessor trace, unchanged V30 staged success/error branches, unchanged clocks/voltages/DT/init. Physical success would need a follow-up causal check before calling the barrier a production fix.

## After V31: choose higher-information checks

V31 failed in the same delivery window; early use_cdr=0. This weakens CDR/DLL and store-barrier explanations but does not prove the precise failing instruction. Stock Android return and host cleanup are verified. No V32 changes are installed or prepared.

The extracted stock and packaged V31 DT comparison confirms matching host base, named IRQ lines, active/sleep pin biases and drive strengths, and regulator identities. Raw DT binding formats differ legitimately across kernel generations. Mainline's pinctrl node base differs from downstream; physical register addressing requires resolving their driver offsets, not merely comparing node names. Controller voltage constraints and requested/actual output must be distinguished (stock requested 2.950 V; modern representable lower point 2.944 V was previously checked).

Highest-value next diagnostic: a bounded read-only snapshot of documented Qualcomm vendor-control, clock and power registers before CMD0, with software clock/regulator state and stock binary/source comparisons. Select a functional change only from a specific mismatch. Preserve automatic completion/error branches. A stock-kernel RAM control would need its own USB logger: stock lacks DEVTMPFS and CONFIG_USB_CONFIGFS_ACM/SERIAL, so the current RAM init cannot simply be reused.

Reset properties currently say bootloader; /sys/fs/pstore is absent and /proc/last_kmsg is denied. They do not establish watchdog, brownout, secure fault or kernel panic. Better persistent fault logging would require a verified reserved-RAM layout, not guessing a RAM address. If the configuration comparison finds no mismatch, consider a contained comparison with a known-working SDM660 kernel branch, holding DT/init constant where compatible, to separate newer-driver behavior from A6L-specific setup.

An additional source comparison found upstream CORE_VENDOR_SPEC_POR_VAL=0xa9c versus public downstream 0xa1c. This is NOT yet verified in the A6L binary and the differing bit must be understood before any write or fix claim.

Follow-up search identifies the 0xa1c->0xa9c change as enabling the ADMA length-mismatch error interrupt, discussed at https://lkml.iu.edu/2004.2/00430.html. CMD0 is a no-data command, so this difference alone is not a justified next fix. Confirm the actual register state and whether DMA-related state is unexpectedly active before considering it.
