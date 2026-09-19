# V19: start USB before the storage driver

V18 produced early screen logs, then the user observed a reset, Android splash and shutdown. No diagnostic USB data arrived. Normal Android returned and laptop services were restored at 12:44:57 UTC. V18 remains installed in recovery; no precise storage failure location was established.

V19 retains the exact V18 device tree, regulator settings, USB fix, command line and load addresses. Its only kernel configuration change is CONFIG_MMC_SDHCI_MSM=y to =m. The matching sdhci-msm.ko is bundled in the RAM filesystem. After 20 seconds, and only after the USB serial path has accepted log bytes, RAM init forks a child to load that module once. The parent continues draining kernel logs. It records child completion and partition snapshots. There is no module unload/retry, shell, persistent filesystem mount or userspace block-device open.

The schedule remains USB configuration/bind/connect/serial at 0/4/8/12 seconds. Module loading starts at 20 seconds. Host capture waits for a heartbeat at 32 seconds, bounded by 45 seconds from fastboot departure. USB serial data accepted by the device is not itself an acknowledgment that the host has persisted every byte; physical host logging remains necessary.

Validated offline:

- Kernel config difference is exactly built-in to module; kernel and module compilation passed.
- A diskless ARM64 QEMU harness loaded the exact module into the matching kernel successfully, without missing symbols or a kernel panic. It does not emulate Qualcomm hardware. The first harness run was interrupted by a Windows shared-mount log read error; the second used Linux storage and passed. Both reports are retained.
- The actual RAM init booted and kept running in QEMU, with no storage load before USB was available. QEMU lacks the physical USB controller, so this test covers the gating path rather than the phone's complete module-loading path.
- Packaging and captured ABL checks passed. Six protocol and four transition guard tests passed.

Hashes:

- Candidate: `9c092eb41e8c894bff5aea6a1df442582cb4378d782e48d3883aafa1d04bed4e`
- Adapted kernel: `4a93a3ef95ec44c820fdacf24085c68ce64718f27cdc7140cf676c14a744673c`
- RAM: `8e561fae317ece3bcf4c072daa41fe803766c3752485c68351c3330c31d18ee7`
- Module: `4c74fb1ca69ab89a6662a7d8edbea46cf119cb0baa20483583f7dcb35676c41b`

The V19 guarded workflow accepts only exact stock or V18 recovery as predecessor and preserves the exact known boot message. Tools are pinned in tools/diagnostic-user-v19-tools.json. At this checkpoint no V19 transfer or phone write occurred: automatic approval review rejected the SCP staging call, citing missing explicit authorization for this specific payload transfer to the laptop. User approval for copying and installing this concrete candidate has been requested. Do not work around that rejection.

User explicitly authorized the concrete V19 transfer and installation. Staging hashes and remote preflight passed. Installation completed at 13:00:25 UTC; all twelve copied readbacks independently verified on desktop. Exact V18 predecessor and known boot message preserved, laptop services restored. V19 is now installed in recovery, phone powered off, awaiting manual normal Android startup. The V19 diagnostic has not yet been booted.

## Physical trial

USB enumeration and serial logging worked, yielding 176,747 bytes (SHA-256 53536bd1f3812fe84d9e1956453e306c720cc97dcaa861e6e86d5416a04b3f24). The final received heartbeat is seconds=20 with USB configured. The host lost USB at 13:13:33 UTC. Neither the fork-begin nor module-load-begin marker reached the host, and no kernel panic was captured. This locates the failure near the scheduled module-loading boundary but does not identify the precise failing operation or prove driver probe entry. The RAM loop sends its pending journal before forking; new markers can be lost if the device resets before the next loop. Future instrumentation should let boundary markers reach USB before advancing. Normal Android restart has been requested; cleanup is pending that return.

The coordinator completed at 13:17:50 UTC, after its Android-return window elapsed, with laptop services restored. An independent check at 13:42:14 UTC confirmed the exact spare fingerprint, boot-complete=1, unlocked/orange state and restored services. Both records are archived separately; the historic coordinator result is unchanged. V19 remains installed, stock Android is running.
