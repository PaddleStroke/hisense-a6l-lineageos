# Related SDM660 kernels and A6L storage identity

Research on 2026-09-17, after V33. No firmware changes during this comparison.

## Verified evidence

- Official LineageOS wiki source (`B2N.txt`, main branch) lists Nokia 7 plus, SDM660, LineageOS 22.2, kernel 4.4. This does not establish LineageOS 24 support. No verified SDM660 LineageOS 24 working build was found in the primary sources checked.
- LineageOS/android_kernel_xiaomi_sdm660 has a lineage-22.2 branch at 0eaf76b319b586a7974036cbcdd1c1e8f35364bd; its Makefile reports 4.19.325. Branch existence alone is not proof of a working device build.
- https://github.com/sdm660-mainline/linux/pull/186 reports Vsmart Active 1 / BQ Aquaris X2 Pro hardware testing with Linux 6.19.10 and working eMMC HS400, USB, graphics and other peripherals. It was subsequently rebased onto qcom-sdm660-7.0.y. Do not label the final PR head as the exact 6.19.10 tested revision without locating that revision.
- Existing firmware/raw-backup-20260914/resume.log and whole-transfer.log report eMMC product hDEaP3, manufacturer_id 144 (0x90), 244285440 blocks of 512 bytes. Linux drivers/mmc/core/card.h defines CID_MANFID_HYNIX as 0x90. Therefore SK Hynix, nominal 128 GB; exact package part number is not established.
- Stock Android denied the read-only CID/name/manufacturer sysfs queries. Existing backup logs provide the identity without another EDL session or teardown.
- Upstream drivers/mmc/core/quirks.h contains a Hynix eMMC 4.41 broken-HPI quirk, not an identified hDEaP3/CMD0 fix. Do not apply unrelated later-stage quirks blindly.

## Interpretation and possible next work

Modern Android does not inherently require the latest Linux kernel. The present Linux 7.2.3 bring-up is one approach; an adapted Qualcomm 4.19 source tree is a meaningful alternative to assess, but compatibility with LineageOS 24 and the A6L vendor/e-ink stack remains unproven.

Reuse source and compare known-working SDM660 configurations/device trees. Do not blindly flash another phone's boot image: A6L power supplies, pin assignments, memory reservations and packaging must be preserved and checked. A controlled older-kernel comparison requires identifying the exact tested revision first.

The diagnostic has no active e-ink driver or Android display stack. A direct e-ink software conflict is therefore less likely, while shared power rails, pins and bootloader state remain plausible board-specific factors. Failure around the first CMD0 is earlier than HS400 tuning, filesystem access or Android startup. Neither the chip identity nor V33 proves the cause.

V33 removed the optional eMMC controller reset property but did not resolve the USB loss. The delivered log does not prove the exact failing instruction. No V34 candidate has been selected or prepared as part of this research.
