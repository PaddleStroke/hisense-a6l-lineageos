#!/usr/bin/env python3
# ipa agent (24 Sep 2026): adapt Vladimir Lypak's ipa2-lite (msm8953-mainline 7.1.3/main, drivers/net/ipa2-lite)
# to the A6L (SDM660, IPA v2.6L) as an out-of-tree module for Linux 7.2.3. Idempotent; run on a pristine copy.
# Changes: driver name "ipa2-lite" (mainline ipa.ko also registers "ipa"), compatible "qcom,sdm660-ipa-lite",
# clock-rate module params, optional interconnect votes (stock msm-bus SVS vector), probe diagnostics,
# a read-only sysfs "a6l_diag" register dump, and QMI handshake log lines (A6L_IPA ...).
import sys, re, os
d = sys.argv[1]
def sub(path, old, new, count=1):
    p = os.path.join(d, path); s = open(p).read()
    if new in s:
        return
    if old not in s:
        sys.exit("FIX_FAIL %s: %r not found" % (path, old[:60]))
    s = s.replace(old, new, count); open(p, 'w').write(s)

# ---- ipa.c ----
sub('ipa.c', '#include <linux/if_rmnet.h>\n', '#include <linux/if_rmnet.h>\n#include <linux/interconnect.h>\n')
sub('ipa.c', 'static bool dump;\nmodule_param(dump, bool, 0644);\n',
'''static bool dump;
module_param(dump, bool, 0644);

/* A6L: msm8953 uses 40 MHz; downstream ipa_v2 (SDM660) SVS/NOMINAL/TURBO = 75/150/200 MHz */
static uint clk_hz = 40000000;
module_param(clk_hz, uint, 0444);
static uint clk_idle_hz = 9600000;
module_param(clk_idle_hz, uint, 0444);
/* A6L: stock msm-bus "ipa" SVS vector: IPA->EBI ab 80000 / ib 640000 KBps, APPS->IPA_CFG 80000 KBps */
static uint icc_mem_avg_kbps = 80000;
module_param(icc_mem_avg_kbps, uint, 0444);
static uint icc_mem_peak_kbps = 640000;
module_param(icc_mem_peak_kbps, uint, 0444);
static uint icc_cfg_kbps = 80000;
module_param(icc_cfg_kbps, uint, 0444);
''')
sub('ipa.c', '\tstruct ipa_dma_obj system_hdr;\n};\n',
    '\tstruct ipa_dma_obj system_hdr;\n\tstruct icc_path *icc_mem, *icc_cfg;\n};\n')

# probe: clock rate param, interconnects, diagnostics
sub('ipa.c', '''	clk_set_rate(ipa->clk, 40000000);

	dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));

	ipa_reset_hw(ipa);

	ret = ipa_partition_mem(ipa);
	if (ret)
		return ret;
''', '''	clk_set_rate(ipa->clk, clk_hz);

	/* A6L: optional bandwidth votes; the msm8953 original has none */
	if (of_property_present(dev->of_node, "interconnects")) {
		ipa->icc_mem = devm_of_icc_get(dev, "memory");
		if (IS_ERR(ipa->icc_mem)) {
			dev_warn(dev, "A6L_IPA no memory icc path: %ld\\n", PTR_ERR(ipa->icc_mem));
			ipa->icc_mem = NULL;
		} else if (ipa->icc_mem) {
			icc_set_bw(ipa->icc_mem, kBps_to_icc(icc_mem_avg_kbps),
				   kBps_to_icc(icc_mem_peak_kbps));
		}
		ipa->icc_cfg = devm_of_icc_get(dev, "config");
		if (IS_ERR(ipa->icc_cfg)) {
			dev_warn(dev, "A6L_IPA no config icc path: %ld\\n", PTR_ERR(ipa->icc_cfg));
			ipa->icc_cfg = NULL;
		} else if (ipa->icc_cfg) {
			icc_set_bw(ipa->icc_cfg, kBps_to_icc(icc_cfg_kbps),
				   kBps_to_icc(icc_cfg_kbps));
		}
	}

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		dev_warn(dev, "A6L_IPA dma mask: %d\\n", ret);

	dev_info(dev, "A6L_IPA hw ver 0x%08x comp_hw 0x%08x bam rev 0x%08x bam pipes 0x%08x clk %lu iommu %s\\n",
		 ioread32(ipa->mmio + REG_IPA_VERSION_OFST),
		 ioread32(ipa->mmio + REG_IPA_COMP_HW_VERSION_OFST),
		 ioread32(ipa->mmio + REG_BAM_REVISION),
		 ioread32(ipa->mmio + REG_BAM_NUM_PIPES),
		 clk_get_rate(ipa->clk), device_iommu_mapped(dev) ? "yes" : "no");

	ipa_reset_hw(ipa);

	ret = ipa_partition_mem(ipa);
	if (ret)
		return ret;

	dev_info(dev, "A6L_IPA sram restr 0x%x size 0x%x: ft4 %#x ft6 %#x rt4 %#x rt6 %#x mdm_hdr %#x drv %#x comp %#x+%#x mdm %#x+%#x\\n",
		 ipa->smem_restr_bytes, ipa->smem_size + ipa->smem_restr_bytes,
		 ipa->layout[MEM_FT_V4].offset, ipa->layout[MEM_FT_V6].offset,
		 ipa->layout[MEM_RT_V4].offset, ipa->layout[MEM_RT_V6].offset,
		 ipa->layout[MEM_MDM_HDR].offset, ipa->layout[MEM_DRV].offset,
		 ipa->layout[MEM_MDM_COMP].offset, ipa->layout[MEM_MDM_COMP].size,
		 ipa->layout[MEM_MDM].offset, ipa->layout[MEM_MDM].size);
''')
sub('ipa.c', '''	ret = ipa_init_sram(ipa);
	if (ret)
		return ret;

	rmw32(''', '''	ret = ipa_init_sram(ipa);
	if (ret) {
		dev_err(dev, "A6L_IPA sram init (immediate commands on pipe 3) failed: %d\\n", ret);
		return ret;
	}
	dev_info(dev, "A6L_IPA sram tables initialised via immediate commands\\n");

	rmw32(''')
sub('ipa.c', '''static int ipa_runtime_resume(struct device *dev)
{
	struct ipa *ipa = dev_get_drvdata(dev);

	clk_set_rate(ipa->clk, 40000000);''', '''static int ipa_runtime_resume(struct device *dev)
{
	struct ipa *ipa = dev_get_drvdata(dev);

	clk_set_rate(ipa->clk, clk_hz);''')
sub('ipa.c', '\tclk_set_rate(ipa->clk, 9600000);', '\tclk_set_rate(ipa->clk, clk_idle_hz);')

# sysfs diag dump
sub('ipa.c', 'static int ipa_modem_rx_id = EP_RX;', '''/* A6L: register snapshot for the attended test (cat /sys/bus/platform/devices/14780000.ipa/a6l_diag) */
static ssize_t a6l_diag_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct ipa *ipa = dev_get_drvdata(dev);
	void __iomem *m;
	int len = 0, p;

	if (!ipa || !ipa->mmio)
		return -ENODEV;
	m = ipa->mmio;
	len += sysfs_emit_at(buf, len, "irq_stts 0x%08x irq_en 0x%08x bam_irq_srcs 0x%08x bam_irq_stts 0x%08x uc_resp 0x%08x uc_loaded 0x%08x\\n",
			     ioread32(m + REG_IPA_IRQ_STTS_EE0), ioread32(m + REG_IPA_IRQ_EN_EE0),
			     ioread32(m + REG_BAM_IRQ_SRCS_EE0), ioread32(m + REG_BAM_IRQ_STTS),
			     ioread32(m + REG_IPA_UC_RESP),
			     ipa->smem_uc_loaded ? ipa->smem_uc_loaded[0] : 0);
	for (p = 0; p < 15; p++)
		len += sysfs_emit_at(buf, len,
			"pipe %2d bam_ctrl 0x%08x rd 0x%04x wr 0x%04x irq 0x%08x | ep_ctrl 0x%x hdr 0x%08x hdr_ext 0x%08x route 0x%x mode 0x%x status 0x%x aggr 0x%x holb %u dbg 0x%x\\n",
			p, ioread32(m + REG_BAM_P_CTRL(p)),
			ioread32(m + REG_BAM_P_RD_OFF_REG(p)) & 0xffff,
			ioread32(m + REG_BAM_P_WR_OFF_REG(p)) & 0xffff,
			ioread32(m + REG_BAM_P_IRQ_STTS(p)),
			ioread32(m + REG_IPA_EP_CTRL(p)), ioread32(m + REG_IPA_EP_HDR(p)),
			ioread32(m + REG_IPA_EP_HDR_EXT(p)), ioread32(m + REG_IPA_EP_ROUTE(p)),
			ioread32(m + REG_IPA_EP_MODE(p)), ioread32(m + REG_IPA_EP_STATUS(p)),
			ioread32(m + REG_IPA_EP_AGGR(p)), ioread32(m + REG_IPA_EP_HOL_BLOCK_EN(p)),
			ioread32(m + REG_IPA_EP_DBG_CNT_REG(p)));
	return len;
}
static DEVICE_ATTR_RO(a6l_diag);

static struct attribute *ipa_a6l_attrs[] = {
	&dev_attr_a6l_diag.attr,
	NULL
};

static const struct attribute_group ipa_a6l_group = {
	.attrs		= ipa_a6l_attrs,
};

static int ipa_modem_rx_id = EP_RX;''')
sub('ipa.c', '''	&ipa_modem_group,
	NULL
};''', '''	&ipa_modem_group,
	&ipa_a6l_group,
	NULL
};''')
sub('ipa.c', '''	{ .compatible	= "qcom,ipa-lite-v2.6", (void *)26 },''',
    '''	{ .compatible	= "qcom,ipa-lite-v2.6", (void *)26 },
	{ .compatible	= "qcom,sdm660-ipa-lite", (void *)26 },	/* A6L: SDM660 = IPA v2.6L */''')
sub('ipa.c', '\t\t.name\t\t= "ipa",\n', '\t\t.name\t\t= "ipa2-lite",\n')
sub('ipa.c', 'MODULE_DESCRIPTION("Qualcomm IP Accelerator v2.X driver");',
    'MODULE_DESCRIPTION("Qualcomm IP Accelerator v2.X driver (ipa2-lite, A6L SDM660 build)");\nMODULE_SOFTDEP("pre: qcom_q6v5_mss");')
# modem present log
sub('ipa.c', '''	(present ? netif_device_attach : netif_device_detach) (ipa->modem);''',
    '''	dev_info(dev, "A6L_IPA modem %s\\n", present ? "PRESENT (rmnet_ipa0 attached)" : "absent (rmnet_ipa0 detached)");
	(present ? netif_device_attach : netif_device_detach) (ipa->modem);''')
sub('ipa.c', '''	if (val & BIT(IPA_IRQ_UC_IRQ_1)) {
		val = ioread32(ipa->mmio + REG_IPA_UC_RESP);
		val &= IPA_UC_RESP_OP_MASK;
		if (ipa->qmi && val == IPA_UC_RESPONSE_INIT_COMPLETED) {''', '''	if (val & BIT(IPA_IRQ_UC_IRQ_1)) {
		val = ioread32(ipa->mmio + REG_IPA_UC_RESP);
		val &= IPA_UC_RESP_OP_MASK;
		if (ipa->qmi && val == IPA_UC_RESPONSE_INIT_COMPLETED) {
			dev_info(ipa->dev, "A6L_IPA uC INIT_COMPLETED\\n");''')

# ---- ipa-qmi.c ----
sub('ipa-qmi.c', '''	ipa_qmi->modem_sq.sq_port = svc->port;

	schedule_work''', '''	ipa_qmi->modem_sq.sq_port = svc->port;
	dev_info(ipa_qmi->dev, "A6L_IPA modem IPA QMI service up (node %u port %u), sending INIT_DRIVER\\n",
		 svc->node, svc->port);

	schedule_work''')
sub('ipa-qmi.c', '''	if (!ret) {
		ipa_qmi->modem_ready = true;''', '''	if (!ret) {
		dev_info(dev, "A6L_IPA modem INIT_DRIVER response OK (skip_uc_load=%u)\\n", req.skip_uc_load);
		ipa_qmi->modem_ready = true;''')
sub('ipa-qmi.c', '''		ipa_qmi->indication_sent = true;''', '''		ipa_qmi->indication_sent = true;
		dev_info(ipa_qmi->dev, "A6L_IPA INIT_COMPLETE indication sent to modem\\n");''')
print("A6L_IPA2_LITE_FIX_OK")
