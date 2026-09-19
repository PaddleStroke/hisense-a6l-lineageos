/* Temporary first-CMD0 ordering experiment for the opted-in spare A6L.
 * Keep Qualcomm's normal side effects. wmb() is dsb(st) on this ARM64 build,
 * matching the store barrier observed before the stock command write.
 */
static bool a6l_cmd0_ordered_used;
static bool a6l_accessor_state_logged;
static void sdhci_msm_writew(struct sdhci_host *host, u16 val, int reg)
{
	struct sdhci_pltfm_host *pltfm_host = sdhci_priv(host);
	struct sdhci_msm_host *msm_host = sdhci_pltfm_priv(pltfm_host);
	bool diagnostic = a6l_storage_hold_cmd0(mmc_dev(host->mmc));
	bool ordered = diagnostic && !a6l_cmd0_ordered_used &&
		reg == SDHCI_COMMAND && SDHCI_GET_CMD(val) == MMC_GO_IDLE_STATE;
	u32 req_type = 0;

	/* Transfer setup precedes the staged command by several USB flushes. */
	if (diagnostic && !a6l_accessor_state_logged &&
	    reg == SDHCI_TRANSFER_MODE) {
		a6l_accessor_state_logged = true;
		dev_info(mmc_dev(host->mmc), "A6L_MSM_ACCESSOR_V31 use_cdr=%u transfer=%04x next_cmd0=stock_store_barrier\n",
			 msm_host->use_cdr, val);
	}
	if (ordered) {
		a6l_cmd0_ordered_used = true;
		dev_info(mmc_dev(host->mmc), "A6L_MSM_CMD0_CHECK_BEGIN use_cdr=%u transfer=%04x word=%04x\n",
			 msm_host->use_cdr, msm_host->transfer_mode, val);
	}
	req_type = __sdhci_msm_check_write(host, val, reg);
	if (ordered) {
		dev_info(mmc_dev(host->mmc), "A6L_MSM_CMD0_BARRIER_BEGIN req_type=%u\n", req_type);
		wmb();
	}
	writew_relaxed(val, host->ioaddr + reg);
	if (ordered)
		dev_info(mmc_dev(host->mmc), "A6L_MSM_CMD0_STORE_RETURNED\n");
	if (req_type)
		sdhci_msm_check_power_status(host, req_type);
}
