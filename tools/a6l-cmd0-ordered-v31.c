/* Temporary A6L RAM diagnostic, not a production SDHCI workaround.
 * One outstanding CMD0 retains its normal MMC request/PM lifetime. The RAM
 * probe never unbinds the host or unloads its module. No card/block writes,
 * invented completions, or voltage changes are performed here.
 */
static struct sdhci_host *a6l_cmd0_host;
static struct mmc_command *a6l_cmd0_cmd;
static u32 a6l_cmd0_signal;
static u16 a6l_cmd0_word;
static unsigned int a6l_cmd0_step, a6l_cmd0_polls;
static bool a6l_cmd0_used;

static void a6l_cmd0_work(struct work_struct *work);
static DECLARE_DELAYED_WORK(a6l_cmd0_delayed, a6l_cmd0_work);

/* Called only while the original request is pending and host->lock is held.
 * These are standard SDHCI registers, not guessed vendor offsets. Reading
 * INT_STATUS does not acknowledge it; INT_ENABLE is deliberately unchanged.
 */
static u32 a6l_cmd0_snapshot(struct sdhci_host *host, const char *stage)
{
	u32 status = sdhci_readl(host, SDHCI_INT_STATUS);

	dev_info(mmc_dev(host->mmc),
		 "A6L_CMD0_SNAPSHOT %s status=%08x present=%08x signal=%08x enable=%08x clock=%04x power=%02x command=%04x argument=%08x\n",
		 stage, status, sdhci_readl(host, SDHCI_PRESENT_STATE),
		 sdhci_readl(host, SDHCI_SIGNAL_ENABLE),
		 sdhci_readl(host, SDHCI_INT_ENABLE),
		 sdhci_readw(host, SDHCI_CLOCK_CONTROL),
		 sdhci_readb(host, SDHCI_POWER_CONTROL),
		 sdhci_readw(host, SDHCI_COMMAND),
		 sdhci_readl(host, SDHCI_ARGUMENT));
	return status;
}

static void a6l_cmd0_work(struct work_struct *work)
{
	struct sdhci_host *host = a6l_cmd0_host;
	struct device *dev = mmc_dev(host->mmc);
	unsigned long irqflags;
	u32 status, present;
	bool again = false;
	const char *result = NULL;

	spin_lock_irqsave(&host->lock, irqflags);
	dev_info(dev, "A6L_CMD0_CALLBACK step=%u runtime_suspended=%u pm_status=%d pm_usage=%d request_same=%u\n",
		 a6l_cmd0_step, host->runtime_suspended,
		 READ_ONCE(dev->power.runtime_status),
		 atomic_read(&dev->power.usage_count),
		 host->cmd == a6l_cmd0_cmd);
	/* No dereference of the saved command if the normal path released it. */
	if (host->cmd != a6l_cmd0_cmd || host->runtime_suspended ||
	    host->flags & SDHCI_DEVICE_DEAD) {
		result = "hold_lifetime_or_power_changed";
		goto out;
	}
	switch (a6l_cmd0_step) {
	case 0:
		a6l_cmd0_signal = sdhci_readl(host, SDHCI_SIGNAL_ENABLE);
		status = a6l_cmd0_snapshot(host, "baseline");
		dev_info(dev, "A6L_CMD0_SOFTWARE clock=%u actual=%u ios_clock=%u vdd=%u width=%u timing=%u power_mode=%u irq=%d quirks=%08x quirks2=%08x\n",
			 host->clock, host->mmc->actual_clock,
			 host->mmc->ios.clock, host->mmc->ios.vdd,
			 host->mmc->ios.bus_width, host->mmc->ios.timing,
			 host->mmc->ios.power_mode, host->irq,
			 host->quirks, host->quirks2);
		if (status == ~0U || a6l_cmd0_signal == ~0U ||
		    status & (SDHCI_INT_CMD_MASK | SDHCI_INT_ERROR) ||
		    !(a6l_cmd0_signal & SDHCI_INT_RESPONSE)) {
			result = "hold_invalid_or_stale_baseline";
			break;
		}
		dev_info(dev, "A6L_CMD0_MASK_BEGIN\n");
		sdhci_writel(host, 0, SDHCI_SIGNAL_ENABLE);
		if (sdhci_readl(host, SDHCI_SIGNAL_ENABLE) != 0) {
			result = "hold_mask_readback_failed";
			break;
		}
		dev_info(dev, "A6L_CMD0_MASKED next=preissue_check delay_ms=4000\n");
		a6l_cmd0_step = 1;
		again = true;
		break;
	case 1:
		status = a6l_cmd0_snapshot(host, "before_issue");
		present = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (status == ~0U || present == ~0U ||
		    status & (SDHCI_INT_CMD_MASK | SDHCI_INT_ERROR) ||
		    present & SDHCI_CMD_INHIBIT ||
		    sdhci_readl(host, SDHCI_SIGNAL_ENABLE) != 0) {
			result = "hold_preissue_state_changed";
			break;
		}
		dev_info(dev, "A6L_CMD0_PREISSUE_CHECKED next=command_write delay_ms=4000\n");
		a6l_cmd0_step = 2;
		again = true;
		break;
	case 2:
		/* V31: deliberately no new controller reads between the delivered
		 * pre-issue snapshot and this isolated command-register write.
		 * The pending-request/power software guard above still applies.
		 */
		dev_info(dev, "A6L_CMD0_ISSUE_BEGIN word=%04x signal=0\n", a6l_cmd0_word);
		sdhci_writew(host, a6l_cmd0_word, SDHCI_COMMAND);
		dev_info(dev, "A6L_CMD0_ISSUE_RETURNED next=status delay_ms=4000\n");
		a6l_cmd0_step = 3;
		again = true;
		break;
	case 3:
		status = a6l_cmd0_snapshot(host, "after_issue");
		present = sdhci_readl(host, SDHCI_PRESENT_STATE);
		if (status == ~0U || present == ~0U) {
			result = "hold_unreadable_registers";
		} else if (status & ((SDHCI_INT_CMD_MASK & ~SDHCI_INT_RESPONSE) |
				    SDHCI_INT_ERROR)) {
			result = "hold_command_error";
		} else if ((status & SDHCI_INT_RESPONSE) &&
			   !(present & SDHCI_CMD_INHIBIT)) {
			dev_info(dev, "A6L_CMD0_POLLED_SUCCESS next=unmask delay_ms=4000\n");
			a6l_cmd0_step = 4;
			again = true;
		} else if (++a6l_cmd0_polls < 2) {
			dev_info(dev, "A6L_CMD0_PENDING retry_status_only delay_ms=4000\n");
			again = true;
		} else {
			result = "hold_no_completion";
		}
		break;
	case 4:
		status = a6l_cmd0_snapshot(host, "before_unmask");
		if (status == ~0U || !(status & SDHCI_INT_RESPONSE) ||
		    status & ((SDHCI_INT_CMD_MASK & ~SDHCI_INT_RESPONSE) |
			      SDHCI_INT_ERROR) ||
		    sdhci_readl(host, SDHCI_SIGNAL_ENABLE) != 0) {
			result = "hold_completion_changed";
			break;
		}
		dev_info(dev, "A6L_CMD0_UNMASK_BEGIN signal=%08x\n", a6l_cmd0_signal);
		/* No status acknowledgement or synthetic completion: let the usual
		 * IRQ handler consume the latched success and continue discovery.
		 */
		sdhci_writel(host, a6l_cmd0_signal, SDHCI_SIGNAL_ENABLE);
		dev_info(dev, "A6L_CMD0_UNMASK_RETURNED\n");
		result = "released_to_normal_irq";
		break;
	default:
		result = "hold_invalid_stage";
	}
out:
	spin_unlock_irqrestore(&host->lock, irqflags);
	/* After unmasking, the request can finish immediately. No further
	 * command dereferences or controller reads are allowed here.
	 */
	if (result)
		dev_info(dev, "A6L_CMD0_STAGE_FINISHED outcome=%s\n", result);
	if (again)
		schedule_delayed_work(&a6l_cmd0_delayed, msecs_to_jiffies(4000));
}

static bool a6l_cmd0_stage(struct sdhci_host *host, struct mmc_command *cmd,
			  u16 command)
{
	/* Called under host->lock; one fixed, opted-in physical host only. */
	if (cmd->opcode != MMC_GO_IDLE_STATE || a6l_cmd0_used ||
	    !a6l_storage_hold_cmd0(mmc_dev(host->mmc)))
		return false;
	a6l_cmd0_used = true;
	a6l_cmd0_host = host;
	a6l_cmd0_cmd = cmd;
	a6l_cmd0_word = command;
	sdhci_del_timer(host, cmd->mrq);
	dev_info(mmc_dev(host->mmc), "A6L_CMD0_STAGED_V31 next=baseline delay_ms=4000\n");
	schedule_delayed_work(&a6l_cmd0_delayed, msecs_to_jiffies(4000));
	return true;
}

static atomic_t a6l_follow_events = ATOMIC_INIT(0);
static void a6l_follow_command(struct sdhci_host *host, const char *stage,
			       struct mmc_command *cmd)
{
	if (a6l_storage_hold_cmd0(mmc_dev(host->mmc)) &&
	    atomic_inc_return(&a6l_follow_events) <= 128)
		dev_info(mmc_dev(host->mmc),
			 "A6L_FOLLOW_COMMAND %s op=%u arg=%08x flags=%08x error=%d response0=%08x\n",
			 stage, cmd->opcode, cmd->arg, cmd->flags,
			 cmd->error, cmd->resp[0]);
}
