/* Read-only subset of registers already used by the SDCC5 driver.
 * Do not read newer DLL config3/user-control registers on older hardware.
 * Called with the SDHCI lock held; no clock/regulator API that may sleep.
 */
static void a6l_msm_state_snapshot(struct sdhci_host *host)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	const struct sdhci_msm_offset *o = msm_host->offset;
	struct device *dev = mmc_dev(host->mmc);
	u32 value;

	if (!msm_host->mci_removed) {
		dev_info(dev, "A6L_MSM_STATE_V32 refused_non_v5\n");
		return;
	}
	dev_info(dev, "A6L_MSM_STATE_V32 software clk_rate=%lu curr_pwr=%x curr_io=%x vqmmc_enabled=%u use_cdr=%u tuning=%u calibration=%u caps0=%08x transfer=%04x pwr_irq=%d\n",
		 msm_host->clk_rate, msm_host->curr_pwr_state,
		 msm_host->curr_io_level, msm_host->vqmmc_enabled,
		 msm_host->use_cdr, msm_host->tuning_done,
		 msm_host->calibration_done, msm_host->caps_0,
		 msm_host->transfer_mode, msm_host->pwr_irq);
	/* Print each address before its read to retain partial fault evidence. */
#define A6L_STATE_READ(label, offset) do { \
	dev_info(dev, "A6L_MSM_STATE_V32 read_begin %s offset=%03x\n", label, (unsigned int)(offset)); \
	value = readl_relaxed(host->ioaddr + (offset)); \
	dev_info(dev, "A6L_MSM_STATE_V32 %s=%08x\n", label, value); \
} while (0)
	A6L_STATE_READ("core_version", o->core_mci_version);
	A6L_STATE_READ("vendor_spec", o->core_vendor_spec);
	A6L_STATE_READ("vendor_func2", o->core_vendor_spec_func2);
	A6L_STATE_READ("vendor_caps", o->core_vendor_spec_capabilities0);
	A6L_STATE_READ("pwr_status", o->core_pwrctl_status);
	A6L_STATE_READ("pwr_mask", o->core_pwrctl_mask);
	A6L_STATE_READ("pwr_control", o->core_pwrctl_ctl);
	A6L_STATE_READ("dll_config", o->core_dll_config);
	A6L_STATE_READ("dll_status", o->core_dll_status);
	A6L_STATE_READ("dll_config2", o->core_dll_config_2);
#undef A6L_STATE_READ
	dev_info(dev, "A6L_MSM_STATE_V32 snapshot_done\n");
}
