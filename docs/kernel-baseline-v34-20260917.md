# V34: older SDM660 kernel comparison

Status: installation launched once at 2026-09-17T08:37:24.345358 UTC, PID205150. Awaiting completion and independent copied readbacks; do not retry. All captured-bootloader checks, six protocol checks, four transition checks, seven collector checks, nine staged tool hashes and offline inspector passed. Spare Android and clean laptop services were verified before launch.

The user approved trying an older kernel, then narrowing the regression range if it works. Linux 6.19.10 from sdm660-mainline revision `a587e4f18b483d0a17579e6325c861b303988bda` is the base of the hardware-tested Vsmart Active 1 report in [PR 185](https://github.com/sdm660-mainline/linux/pull/185). The four PR commits add that other phone's panel and board support; they do not change the storage implementation used here. This is a plausible comparison, not a known-good A6L kernel.

## Controlled harness

- Isolated checkout `/home/a6l/kernel/a6l-baseline-6.19`, output `/home/a6l/kernel/out-a6l-baseline-6.19`. The original 7.2.3 checkout and artifacts are preserved.
- Retain A6L boot-domain preservation, explicitly staged USB connect, bounded framebuffer console and board-gated optional storage LED bypass.
- Remove all experimental storage holds, command-store barriers, IRQ masking, paced driver checkpoints and register snapshots. MMC core and SDHCI core are unchanged from the 6.19.10 branch.
- Same verified RAM PID1 executable and source as V32/V33, with the matching older-kernel storage module. USB is established before the one module load. No shell, persistent filesystem mounts or userspace block-device opens.
- Same A6L wiring, regulator constraints, GPIO reservations, no-SDIO and omitted optional eMMC reset. eMMC and USB PHY nodes are byte-identical to V33. DT is built with the older branch's own includes.
- Audited inherited DT differences: fixed ADSP reservation instead of dynamic shared DMA pool, older timer CPU mask, and changed children under disabled DSP nodes. See `firmware/extracted/baseline-6.19-kernel-20260917/device-tree-audit.json`. These mean an eventual bisection must also control the DT, not just kernel version labels.
- Original command line preserved; the old USB/storage trace parameters are inactive in this minimal kernel.

## Build and checks

Kernel and matching module built. The first compile required a `const u8 *` cast for the older font API; DT compilation required `cdsp_pil` instead of the newer `remoteproc_cdsp` label. Both are recorded; failed build logs preserved.

Matching module load passed in diskless QEMU. Actual RAM PID1 and console passed with the A6L board, wrong-board guard, and short-reservation guard. The first short-reservation fixture mistakenly allowed ordinary RAM allocation in the sampled area; it failed correctly as a test fixture. The corrected fixture separately reserves that observation area. All three cases passed without changing the kernel. Final evidence: `baseline-6.19-probe-r2-20260917`; first-run evidence retained.

Candidate: `31e451b709ead424d93e4623922c9f6549f5229795853123be006611f7fa0706`.
Adapted kernel: `97ff3ff382f921bf6b04a03f6bef89e00bc67c36d1972a6a93ce6d7d784d9057`.
Module: `01f38f4a5ac8a46e27c36646888cc493428a5784bf2b66a0acd98be8965fe83d`.
RAM disk: `1524439edec85fef74621d94ae193dafcccb7a8a2200b84d6a0211a04de2c740`.
Base DT: `21e2c1640287016dc755076d7d203be2900c60847ce4219d15b1edef50f7a49c`.

## Interpretation of the physical trial

Useful success requires surviving storage-driver initialization, detecting the eMMC, and retaining USB heartbeats. Reaching a splash screen or PID1 alone is not enough. The diagnostic automatically proceeds through ordinary enumeration and logs `/proc/partitions`; it does not install or boot LineageOS.

If successful, first build 7.2.3 with this same minimal harness, then bisect the relevant source range if that matched comparison fails. Do not call V33 versus V34 a pure version-only comparison: V33 still contained command experiments. If 6.19 also fails, there is no good bisection endpoint yet; investigate older support or the shared A6L hardware description before assuming a recent regression.

No physical result is recorded yet.

## Verified installation

V34 installation completed 2026-09-17T08:37:55.881559+00:00. All twelve copied before/after readbacks passed independent desktop verification. Recovery changed from the exact V33 image to the pinned V34 candidate; other checked regions and BCB remained unchanged. Poweroff acknowledged, EDL gone, laptop services restored. User asked to start normal Android using only Power; awaiting reply. Capture and restore have not been launched.

## Capture launched

Capture started 2026-09-17T08:42:36.693641 UTC, PID205663, under GNOME suspend/idle inhibition. Exact fastboot identity and active logger confirmed. User instructed to select Recovery, leave connected about 50 seconds, no filming. Awaiting reply and physical result. Host-service pause is owned by the capture coordinator.

## Physical result

Failed to reach storage enumeration. Capture: 149005 bytes, SHA256 d7b3ef6baf9ee1c1693a5b3ba8c9432b62458848863d9eab23cb61ce3fef3885. USB configured and logged through heartbeat 20, followed by the storage module load announcement; no load return, card identification, mmcblk device or panic reached the logger. USB disconnected at 08:49:15.102192 UTC; stock USB returned 08:49:43.487828. Stock Android and clean services verified 2026-09-17T08:49:57.052185+00:00. Exact failure instruction is unknown. This does not establish a working bisection endpoint.
