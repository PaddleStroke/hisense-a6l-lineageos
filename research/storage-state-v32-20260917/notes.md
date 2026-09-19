# V32 pre-CMD0 state comparison

The V31 barrier experiment did not resolve USB loss near CMD0; it confirmed use_cdr=0 before issue. Do not infer an exact instruction from the last delivered log. The next experiment adds observation only while retaining the existing write sequence and automatic branches.

Stock probe, clock and UHS functions were independently matched against original kernel bytes (three ranges; verified-setup-ranges.json), then disassembled in research/stock-storage-binary-20260916/setup-disassembly.txt. Stock probe at 0xffffff8008a2e720 loads 0xa1c and stores it at 0xffffff8008a2e724 in the vendor initialization sequence. Public Qualcomm sources explain this value. Modern 0xa9c adds an ADMA error-reporting bit; CMD0 carries no data, so the difference is not yet a plausible stand-alone fix.

V32 adds:
- VMMC/VQMMC regulator framework enabled and voltage reports, queried in delayed-work process context outside host->lock. These are reported software/framework values, not physical rail measurements.
- Standard host-control, host-control2, transfer, block geometry and capability registers.
- A ten-register SDCC5 subset: core version (0x318), vendor spec (0x20c), function2 (0x210), vendor caps (0x21c), power status/mask/control (0x240/244/24c), DLL config/status/config2 (0x200/208/254). No power-clear, testbus programming, newer config3 or DLL user-control register access.
- Cached clock rate, power/io state, VQMMC enable, CDR/tuning/calibration flags and IRQ number.

The standard vendor-dump callback is reused. One snapshot is placed in stage0 and is followed by the existing four-second precheck gap and four-second commit gap, allowing delivery without another long wait. The barrier and all voltage/clock/card writes remain exactly V31. Outcomes continue automatically to status polling and normal IRQ on success; errors hold with USB alive if the hardware permits.

Interpretation: check revision and intended 400 kHz initialization mode, 1.8-V IO and ~2.95-V VMMC reports, power acknowledgement versus software state, inactive transfer/tuning, and vendor register mode against stock. Only propose a functional change after finding a specific discrepancy. If configuration matches, further fault capture or a controlled kernel-version comparison is more useful than adding pauses.

Primary references: https://github.com/torvalds/linux/blob/master/drivers/mmc/host/sdhci-msm.c ; https://android.googlesource.com/kernel/msm/+/594d847d09a14b0b2e5db288024a0f2c4f64ec9a/drivers/mmc/host/sdhci-msm.c ; https://lkml.iu.edu/2004.2/00430.html . Bounded search again found no confirmed A6L first-CMD0 crash fix.
