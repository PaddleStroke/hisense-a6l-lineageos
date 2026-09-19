# Stock A6L storage binary investigation

The stock 4.4.153-perf kernel is useful reference evidence. The previously reconstructed ELF contains 140,166 recovered symbols, not original debug information or recovered C source. This investigation independently verified eight storage function ranges against the hash-checked raw kernel before interpreting their assembly. See `verified-storage-ranges.json` and `storage-disassembly.txt`.

Raw kernel SHA256: `44b8228173d70f0798a3ee2f29aec4c352d153e08a823abc529500dac33395b6`.
Reconstructed ELF SHA256: `bac80712bb318d2beba5b7d0e60e38862eaedd031071100f95c500fc35a84958`.

## Findings and limits

- The stock `sdhci_send_command` contains a 16-bit store (`strh`) to controller offset 0x0e, at link-time address 0xffffff8008a24c40. Its command construction combines flags with opcode shifted by eight, consistent with SDHCI. No special extra Hisense command is established at this boundary.
- The store is preceded by `dsb st` and `uncached_logk`, with a conditional logging barrier afterward. Stock code then reads argument, transfer mode and command registers. These are concrete observations, not proof the new failure is an ordering issue.
- Qualcomm's public ARM64 I/O header contains the same `uncached_logk`/conditional logging-barrier mechanism. It is therefore not evidence of a Hisense-specific customization by itself.
- Stock `sdhci_msm_readl_relaxed` selects between the host register base and an older core register base. Qualcomm 4.4 source has the same `mci_removed` selection. This is a useful source correspondence, not evidence our current DT mapping is wrong.
- A public Qualcomm 4.4.56 reference logs argument/transfer/command before the write; another Qualcomm 4.19.95 reference logs them after the write, as the stock binary does. Neither reference is established as the exact Hisense source ancestor. Simple binary differences must not be labelled OEM customizations without controlling for source version, compiler and configuration.

Pinned references and file hashes are in `source44-manifest.json` and `source-manifest.json`. The former is commit `594d847d09a14b0b2e5db288024a0f2c4f64ec9a` from android-msm-wahoo-4.4-oreo-dr1; it provides a Qualcomm 4.4 comparator, not an A6L source release. The latter is `1f946a612986353e1484dbde8e7ed2eb6b0b456b` (4.19.95).

Primary sources:

- [Qualcomm 4.4 SDHCI](https://android.googlesource.com/kernel/msm/+/594d847d09a14b0b2e5db288024a0f2c4f64ec9a/drivers/mmc/host/sdhci.c)
- [Qualcomm 4.4 MSM host accessors](https://android.googlesource.com/kernel/msm/+/594d847d09a14b0b2e5db288024a0f2c4f64ec9a/drivers/mmc/host/sdhci-msm.c)
- [Qualcomm ARM64 I/O implementation](https://android.googlesource.com/kernel/msm/+/594d847d09a14b0b2e5db288024a0f2c4f64ec9a/arch/arm64/include/asm/io.h)
- [Later Qualcomm command sequence](https://android.googlesource.com/kernel/msm/+/1f946a612986353e1484dbde8e7ed2eb6b0b456b/drivers/mmc/host/sdhci.c)

## Effect on the next test

Continue the already prepared V30 boundary isolation without also changing ordering, voltage or register mapping. If the command write is implicated, compare the actual stock no-data command path and ordered access semantics with the compiled modern path before choosing the next behavioral change. If a register read fails first, focus on power/clock availability and access mapping. Reuse the stock binary for targeted storage, power and eventually e-ink analysis; do not attempt whole-kernel decompilation as a prerequisite.
