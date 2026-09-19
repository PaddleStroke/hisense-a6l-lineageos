# Small storage search, 2026-09-16

Qualcomm upstream fix: https://github.com/torvalds/linux/commit/20a0c37e44063997391430c4ae09973e9cbc3911
"mmc: sdhci-msm: Correctly set the load for the regulator" describes failed eMMC/SD initialization when regulators remain in low-power mode without a proper load request. Our exact driver contains msm_config_vmmc_regulator, msm_config_vqmmc_regulator and regulator_set_load calls (around lines1444-1498 before V20). Commit ancestry cannot be established from the shallow local tree; content is present. This is a relevant mechanism, not a proven cause of this reset.

Saved GitHub search results cover sdm660-mainline storage/MMC/crash issues. PR48 removes an SD-slot supply for a clover external-SD problem; not appropriate evidence for blindly removing the A6L eMMC supply. PR186 reports working storage on Vsmart Active1, confirming related SoC hardware can work but not explaining this reset. No exact match was established in this small search.

V20 changes diagnostic timing only, keeping V19 regulator constraints and DT identical. Next use actual checkpoint logs to choose any functional fix.
