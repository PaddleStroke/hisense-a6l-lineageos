# V35: eMMC current-vote permissions

V34 / Linux 6.19.10 booted RAM PID1 and USB but disconnected after announcing the storage module load. No module-return, card identification or panic was delivered. It does not establish the exact failing instruction, nor a good bisection endpoint.

Both eMMC supplies in the V34 (and previous V33) A6L DT omit `regulator-allow-set-load`. In the pinned 6.19.10 source:

- `drivers/regulator/of_regulator.c` maps that boolean to `REGULATOR_CHANGE_DRMS`.
- `drivers/regulator/core.c:drms_uA_update()` returns success without applying the load when the permission is absent.
- `drivers/mmc/host/sdhci-msm.c` calls `regulator_set_load()` during the supply setup. Before card identification its VMMC estimate is 800000 uA and its VQMMC estimate is 325000 uA; after eMMC identification the VMMC constant is 570000 uA. These are load votes to the regulator, not forced current through the chip.
- `drivers/regulator/qcom_smd-regulator.c:rpm_reg_set_load()` forwards the requested load to RPM.

Stock A6L controller DT records low/high requests of 200/570000 uA for VDD and 200/325000 uA for VDD-IO. See `research/storage-ordered-v31-20260916/stock-v31-storage-dt.json` and the original extracted DT. The related Xiaomi jasmine/lavender boards include the permission on these supplies and also have fixed system-load votes. V35 adds only the permissions; it does not add fixed loads, change the existing driver's estimates, alter voltages or rebuild the kernel.

Primary references:

- [Related SDM660 board regulator definitions](https://android.googlesource.com/kernel/common/+/refs/tags/android16-6.12-2025-09_r1/arch/arm64/boot/dts/qcom/sdm660-xiaomi-lavender.dts)
- [Pinned regulator core](https://github.com/sdm660-mainline/linux/blob/a587e4f18b483d0a17579e6325c861b303988bda/drivers/regulator/core.c)
- [Pinned SDHCI driver](https://github.com/sdm660-mainline/linux/blob/a587e4f18b483d0a17579e6325c861b303988bda/drivers/mmc/host/sdhci-msm.c)

This is a concrete omission in the port and a plausible explanation for failure when storage becomes active. Physical causality is unproven; do not claim a measured brownout or a confirmed fix.

The experiment preserves all V34 binaries and compares the entire parsed device tree, allowing exactly these two empty boolean properties:

- `/remoteproc/glink-edge/rpm-requests/regulators-0/l4/regulator-allow-set-load`
- `/remoteproc/glink-edge/rpm-requests/regulators-1/l8/regulator-allow-set-load`

Candidate SHA256: `129621db2ac07c8c955426a5a974a4ffc8ba587eb883d0e6c76e6c01a2f76c00`.
Base DT SHA256: `655f7abd94b72061d3f0f2b5593bb8c7b164e186d55dbcd39638123b2d14999c`.

At note creation: packaged, captured-ABL validation in progress, not staged or flashed. Existing emulator checks cover the byte-identical kernel/RAM/module. A physical boot remains necessary.

Validation completed: full DT exact-change assertion, package, captured ABL with negative controls, six protocol checks and four transition checks all passed. Nine manifest-pinned tools and the candidate were staged and remotely hash-verified. Stock restore payload/import/geometry guards and exact Android baseline/physical USB port/clean host were verified. Installation launched once at 2026-09-17T09:00:04.493011UTC, PID206559; awaiting final readback verification.

Installation completed 2026-09-17T09:00:35.869089UTC. Worker exit 0, full device readback passed, poweroff acknowledged and EDL disappeared; host services restored. All twelve copied before/after regions independently verified on Windows in `captures/capture-diagnostic-install-user-v35/desktop-verification.json`. User asked to boot normal Android before the recovery test. Physical V35 test not yet launched; capture and restore remain unused.

Normal Android return verified. Capture launched once 2026-09-17T09:08:18.823987UTC PID207080; collectorPID207162. Exact fastboot identity and live logger confirmed before asking user to select Recovery. Awaiting physical test result.

## Physical result: PASS

2026-09-17T09:09:53.612592UTC: module loaded with result=0, errno=0. MMC identified `hDEaP3`, 116 GiB, HS200; all 59 user-area partitions appeared. USB remained configured through ALIVE40 (20 seconds after module start), satisfying the collector success window. No panic captured. 159144 bytes, SHA256 `8a3123ed752d3304caf2c91fa358956acac3e56bd5c0dd4acb247817b568b176`. See capture analysis. This establishes a working 6.19.10 bring-up baseline; it does not yet validate sustained storage I/O or Android userspace. The only V34-to-V35 changes are the two supply permissions. Android return/host cleanup are pending.

Stock Android baseline return and host-service cleanup verified at 2026-09-17T09:12:16.547170+00:00. Final capture session archived. V35 remains installed as the known-good diagnostic. This result does not establish a version regression: the earlier 7.2 DT also lacked the same two permissions. Apply the correction in a matched minimal 7.2 comparison before considering bisection.
