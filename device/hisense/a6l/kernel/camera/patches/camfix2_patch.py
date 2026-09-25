#!/usr/bin/env python3
# camfix2 (25 Sep 2026): applied AFTER camfix_patch.py to the same copies of the 7.2.3 camss sources.
#   usage: camfix2_patch.py <camss_dir>
# 1) NULL deref at stream-off (vfe_pm_domain_off+0x38 -> device_link_del(NULL)): ispif_reset() calls
#    camss_pm_domain_on/off(VFE0/VFE1) while VFE0 is already powered by vfe_get(); device_link_add() returns the
#    SAME link (kref++), the nested off does device_link_del() and sets vfe->genpd_link = NULL, so the final
#    vfe_put() -> vfe_pm_domain_off() passes NULL to device_link_del().  Fix: refcount the per-VFE link users.
# 2) sdm660 has no interconnect vote (no icc_res) although the DT carries interconnects = <&mnoc 5 &bimc 5>
#    "vfe-mem": add one so the VFE write masters have NoC bandwidth.
# 3) Diagnostics (module param a6l_dbg, default 1): first IRQs of CSIPHY/CSID/ISPIF/VFE with raw status,
#    VFE IRQ bit counters, VFE bus overflow / violation errors (were masked-in but silently ignored),
#    and register dumps of VFE/ISPIF/CSID/CSIPHY status at stream-off (while the sensor still streams).
# 4) a6l_vfe_min (Hz, default 0 = upstream) floor for the VFE core clock + log of the chosen rate.
import sys, re

def sub1(path, old, new):
    s = open(path).read()
    n = s.count(old)
    assert n == 1, f"{path}: anchor found {n}x: {old[:70]!r}"
    open(path, 'w').write(s.replace(old, new))
    print(f"PATCHED {path.split('/')[-1]}: {old.strip().splitlines()[0][:60]}")

def add_after_includes(path, text):
    s = open(path).read()
    last = list(re.finditer(r'^#include .*$', s, re.M))[-1]
    s = s[:last.end()] + "\n" + text + s[last.end():]
    open(path, 'w').write(s)
    print(f"PATCHED {path.split('/')[-1]}: header block")

def add_before(path, anchor, text):
    s = open(path).read()
    assert s.count(anchor) == 1, f"{path}: anchor {anchor!r}"
    open(path, 'w').write(s.replace(anchor, text + "\n" + anchor))
    print(f"PATCHED {path.split('/')[-1]}: block before {anchor.strip()[:40]}")

cs = sys.argv[1]

# ---- 1) refcounted VFE power-domain link ---------------------------------------------------------------
sub1(f"{cs}/camss-vfe.h", "\tstruct device_link *genpd_link;\n",
     "\tstruct device_link *genpd_link;\n\tunsigned int a6l_pd_users; /* A6L: nested pm_domain_on/off (ispif_reset) */\n")
sub1(f"{cs}/camss-vfe.c",
     "\tif (!vfe->genpd)\n\t\treturn;\n\n\tdevice_link_del(vfe->genpd_link);\n\tvfe->genpd_link = NULL;\n",
     "\tif (!vfe->genpd)\n\t\treturn;\n\n"
     "\tif (!vfe->genpd_link) {\n\t\tdev_warn(vfe->camss->dev, \"A6L_VFE%u pm_domain_off without link\\n\", vfe->id);\n\t\treturn;\n\t}\n"
     "\tif (vfe->a6l_pd_users > 1) {\n\t\tvfe->a6l_pd_users--;\n\t\treturn;\n\t}\n"
     "\tdevice_link_del(vfe->genpd_link);\n\tvfe->genpd_link = NULL;\n\tvfe->a6l_pd_users = 0;\n")
sub1(f"{cs}/camss-vfe.c",
     "\tif (!vfe->genpd)\n\t\treturn 0;\n\n\tvfe->genpd_link = device_link_add(",
     "\tif (!vfe->genpd)\n\t\treturn 0;\n\n"
     "\tif (vfe->genpd_link) {\n\t\tvfe->a6l_pd_users++;\n\t\treturn 0;\n\t}\n\n"
     "\tvfe->genpd_link = device_link_add(")
sub1(f"{cs}/camss-vfe.c",
     "\tif (!vfe->genpd_link)\n\t\treturn -EINVAL;\n\n\treturn 0;\n}\n",
     "\tif (!vfe->genpd_link)\n\t\treturn -EINVAL;\n\n\tvfe->a6l_pd_users = 1;\n\n\treturn 0;\n}\n")

# ---- 4) VFE clock floor + log ---------------------------------------------------------------------------
add_after_includes(f"{cs}/camss-vfe.c",
    "\n#include <linux/moduleparam.h>\nstatic uint a6l_vfe_min;\nmodule_param(a6l_vfe_min, uint, 0644);\n"
    "MODULE_PARM_DESC(a6l_vfe_min, \"A6L: minimum VFE core clock in Hz (0 = upstream pixel-rate based)\");\n")
sub1(f"{cs}/camss-vfe.c",
     "\t\t\tcamss_add_clock_margin(&min_rate);\n\n\t\t\tfor (j = 0; j < clock->nfreqs; j++)\n\t\t\t\tif (min_rate < clock->freq[j])",
     "\t\t\tcamss_add_clock_margin(&min_rate);\n\t\t\tif (a6l_vfe_min && min_rate < a6l_vfe_min)\n\t\t\t\tmin_rate = a6l_vfe_min;\n\n"
     "\t\t\tfor (j = 0; j < clock->nfreqs; j++)\n\t\t\t\tif (min_rate < clock->freq[j])")
sub1(f"{cs}/camss-vfe.c",
     "\t\t\tret = clk_set_rate(clock->clk, rate);\n\t\t\tif (ret < 0) {\n\t\t\t\tdev_err(dev, \"clk set rate failed: %d\\n\", ret);\n\t\t\t\treturn ret;\n\t\t\t}\n\t\t}\n\t}\n\n\treturn 0;\n}\n",
     "\t\t\tret = clk_set_rate(clock->clk, rate);\n\t\t\tif (ret < 0) {\n\t\t\t\tdev_err(dev, \"clk set rate failed: %d\\n\", ret);\n\t\t\t\treturn ret;\n\t\t\t}\n"
     "\t\t\tdev_info(dev, \"A6L_VFE%u %s rate %ld (min %llu)\\n\", vfe->id, clock->name, rate, (unsigned long long)min_rate);\n"
     "\t\t}\n\t}\n\n\treturn 0;\n}\n")

# ---- 2) interconnect vote for sdm660 ------------------------------------------------------------------
sub1(f"{cs}/camss.c",
     "static const struct camss_resources sdm660_resources = {\n\t.version = CAMSS_660,\n",
     "/* A6L: DT has interconnects = <&mnoc 5 &bimc 5>, names \"vfe-mem\"; stock votes VFE AXI bandwidth dynamically */\n"
     "static const struct resources_icc icc_res_660[] = {\n\t{\n\t\t.name = \"vfe-mem\",\n"
     "\t\t.icc_bw_tbl.avg = 1000000,\n\t\t.icc_bw_tbl.peak = 2000000,\n\t},\n};\n\n"
     "static const struct camss_resources sdm660_resources = {\n\t.version = CAMSS_660,\n"
     "\t.icc_res = icc_res_660,\n\t.icc_path_num = ARRAY_SIZE(icc_res_660),\n")

# ---- 3a) VFE 4.8 IRQ counters, error log, register dump ----------------------------------------------
add_after_includes(f"{cs}/camss-vfe-4-8.c", r'''
#include <linux/moduleparam.h>
int a6l_dbg = 1;
module_param(a6l_dbg, int, 0644);
MODULE_PARM_DESC(a6l_dbg, "A6L: 0 off, 1 first IRQs + stop dumps, 2 also periodic VFE IRQ lines");

static u32 a6l_vfe_bits[2][64];
static u32 a6l_vfe_nirq[2];

static void a6l_vfe_count(struct vfe_device *vfe, u32 s0, u32 s1)
{
	unsigned int v = vfe->id & 1, b;
	u32 n;

	if (!a6l_dbg)
		return;
	n = ++a6l_vfe_nirq[v];
	for (b = 0; b < 32; b++) {
		if (s0 & BIT(b))
			a6l_vfe_bits[v][b]++;
		if (s1 & BIT(b))
			a6l_vfe_bits[v][32 + b]++;
	}
	if (n <= 12 || (a6l_dbg > 1 && !(n % 64)))
		dev_info(vfe->camss->dev, "A6L_VFE%u irq#%u s0 0x%08x s1 0x%08x pp 0x%08x\n",
			 vfe->id, n, s0, s1, readl_relaxed(vfe->base + 0x338));
	/* s1: bit0 camif error, bit7 violation, bits 9..15 image master bus overflow */
	if (s1 & (GENMASK(15, 9) | BIT(7) | BIT(0)))
		dev_err_ratelimited(vfe->camss->dev, "A6L_VFE%u ERR s1 0x%08x (ovf wm 0x%02x) violation 0x%08x\n",
				    vfe->id, s1, (s1 >> 9) & 0x7f, readl_relaxed(vfe->base + 0x07c));
}

void a6l_vfe_dump(struct vfe_device *vfe, const char *tag)
{
	static const u16 rng[][2] = { { 0x000, 0x160 }, { 0x300, 0x360 }, { 0x400, 0x4c0 } };
	unsigned int v = vfe->id & 1, i, r;
	char buf[400];
	int len = 0;
	u32 o;

	if (!a6l_dbg)
		return;
	buf[0] = 0;
	for (i = 0; i < 64; i++)
		if (a6l_vfe_bits[v][i])
			len += scnprintf(buf + len, sizeof(buf) - len, " %s%u:%u",
					 i < 32 ? "s0b" : "s1b", i % 32, a6l_vfe_bits[v][i]);
	dev_info(vfe->camss->dev, "A6L_VFE%u_%s irqs %u bits%s\n", vfe->id, tag, a6l_vfe_nirq[v],
		 len ? buf : " none");
	for (r = 0; r < ARRAY_SIZE(rng); r++)
		for (o = rng[r][0]; o < rng[r][1]; o += 32) {
			u32 w[8], nz = 0;

			for (i = 0; i < 8; i++) {
				w[i] = readl_relaxed(vfe->base + o + 4 * i);
				nz |= w[i];
			}
			if (nz)
				dev_info(vfe->camss->dev, "A6L_VFE%u_REG %03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
					 vfe->id, o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
		}
	memset(a6l_vfe_bits[v], 0, sizeof(a6l_vfe_bits[v]));
	a6l_vfe_nirq[v] = 0;
}
''')
sub1(f"{cs}/camss-vfe-4-8.c",
     "\tdev_dbg(vfe->camss->dev, \"VFE: status0 = 0x%08x, status1 = 0x%08x\\n\",\n\t\tvalue0, value1);\n",
     "\tdev_dbg(vfe->camss->dev, \"VFE: status0 = 0x%08x, status1 = 0x%08x\\n\",\n\t\tvalue0, value1);\n\ta6l_vfe_count(vfe, value0, value1);\n")
add_after_includes(f"{cs}/camss-vfe-gen1.c",
    "\nvoid a6l_vfe_dump(struct vfe_device *vfe, const char *tag); /* camss-vfe-4-8.c */\n")
sub1(f"{cs}/camss-vfe-gen1.c",
     "\tvfe_disable_output(line);\n\n\tvfe_put_output(line);\n",
     "\ta6l_vfe_dump(vfe, \"STOP\");\n\tvfe_disable_output(line);\n\n\tvfe_put_output(line);\n")

# ---- 3b) CSID 4.7 (sdm660 uses csid_ops_4_7) ----------------------------------------------------------
add_after_includes(f"{cs}/camss-csid-4-7.c", r'''
#include <linux/moduleparam.h>
extern int a6l_dbg;
static uint a6l_csid_irqmask;
module_param(a6l_csid_irqmask, uint, 0644);
MODULE_PARM_DESC(a6l_csid_irqmask, "A6L: value written to CSID IRQ_MASK at stream-on (0 = leave)");
static u32 a6l_csid_nirq[4];

static void a6l_csid_dump(struct csid_device *csid, const char *tag)
{
	u32 o, w[8], nz;
	unsigned int i;

	if (!a6l_dbg)
		return;
	dev_info(csid->camss->dev, "A6L_CSID%u_%s irqs %u\n", csid->id, tag, a6l_csid_nirq[csid->id & 3]);
	for (o = 0; o < 0x100; o += 32) {
		nz = 0;
		for (i = 0; i < 8; i++) {
			w[i] = readl_relaxed(csid->base + o + 4 * i);
			nz |= w[i];
		}
		if (nz)
			dev_info(csid->camss->dev, "A6L_CSID%u_REG %03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 csid->id, o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	}
	a6l_csid_nirq[csid->id & 3] = 0;
}
''')
sub1(f"{cs}/camss-csid-4-7.c",
     "\t\twritel_relaxed(val, csid->base + CAMSS_CSID_CID_n_CFG(cid));\n",
     "\t\twritel_relaxed(val, csid->base + CAMSS_CSID_CID_n_CFG(cid));\n"
     "\t\tif (a6l_csid_irqmask)\n\t\t\twritel_relaxed(a6l_csid_irqmask, csid->base + CAMSS_CSID_IRQ_MASK);\n"
     "\t\tif (a6l_dbg)\n\t\t\ta6l_csid_dump(csid, \"START\");\n")
sub1(f"{cs}/camss-csid-4-7.c",
     "\t} else {\n\t\tif (tg->enabled) {\n\t\t\tval = CAMSS_CSID_TG_CTRL_DISABLE;",
     "\t} else {\n\t\ta6l_csid_dump(csid, \"STOP\");\n\t\tif (tg->enabled) {\n\t\t\tval = CAMSS_CSID_TG_CTRL_DISABLE;")
sub1(f"{cs}/camss-csid-4-7.c",
     "\tvalue = readl_relaxed(csid->base + CAMSS_CSID_IRQ_STATUS);\n\twritel_relaxed(value, csid->base + CAMSS_CSID_IRQ_CLEAR_CMD);\n",
     "\tvalue = readl_relaxed(csid->base + CAMSS_CSID_IRQ_STATUS);\n\twritel_relaxed(value, csid->base + CAMSS_CSID_IRQ_CLEAR_CMD);\n"
     "\tif (a6l_dbg && ++a6l_csid_nirq[csid->id & 3] <= 16)\n"
     "\t\tdev_info(csid->camss->dev, \"A6L_CSID%u irq#%u status 0x%08x\\n\", csid->id, a6l_csid_nirq[csid->id & 3], value);\n")

# ---- 3c) ISPIF --------------------------------------------------------------------------------------
add_after_includes(f"{cs}/camss-ispif.c", r'''
extern int a6l_dbg;
static u32 a6l_ispif_nirq;

static void a6l_ispif_dump(struct ispif_device *ispif, const char *tag)
{
	u32 o, w[8], nz;
	unsigned int i;

	if (!a6l_dbg)
		return;
	dev_info(ispif->camss->dev, "A6L_ISPIF_%s irqs %u\n", tag, a6l_ispif_nirq);
	for (o = 0; o < 0x500; o += 32) {
		nz = 0;
		for (i = 0; i < 8; i++) {
			w[i] = readl_relaxed(ispif->base + o + 4 * i);
			nz |= w[i];
		}
		if (nz)
			dev_info(ispif->camss->dev, "A6L_ISPIF_REG %03x: %08x %08x %08x %08x %08x %08x %08x %08x\n",
				 o, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
	}
	a6l_ispif_nirq = 0;
}
''')
sub1(f"{cs}/camss-ispif.c",
     "\twritel(0x1, ispif->base + ISPIF_IRQ_GLOBAL_CLEAR_CMD);\n\n\tif ((value0 >> 27) & 0x1)\n\t\tcomplete(&ispif->reset_complete[0]);\n\n\tif ((value3 >> 27) & 0x1)",
     "\twritel(0x1, ispif->base + ISPIF_IRQ_GLOBAL_CLEAR_CMD);\n\n"
     "\tif (a6l_dbg && ++a6l_ispif_nirq <= 16)\n"
     "\t\tdev_info(camss->dev, \"A6L_ISPIF irq#%u vfe0 %08x %08x %08x vfe1 %08x %08x %08x\\n\",\n"
     "\t\t\t a6l_ispif_nirq, value0, value1, value2, value3, value4, value5);\n\n"
     "\tif ((value0 >> 27) & 0x1)\n\t\tcomplete(&ispif->reset_complete[0]);\n\n\tif ((value3 >> 27) & 0x1)")
sub1(f"{cs}/camss-ispif.c",
     "\tu8 cid = vc * 4; /* id of Virtual Channel and Data Type set */\n\tint ret;\n\n\tif (enable) {",
     "\tu8 cid = vc * 4; /* id of Virtual Channel and Data Type set */\n\tint ret;\n\n"
     "\tif (!enable)\n\t\ta6l_ispif_dump(ispif, \"STOP\");\n\n\tif (enable) {")

# ---- 3d) CSIPHY 3ph-1.0 status --------------------------------------------------------------------------
add_before(f"{cs}/camss-csiphy-3ph-1-0.c", "static irqreturn_t csiphy_isr(int irq, void *dev)\n", r'''
extern int a6l_dbg;
static u32 a6l_phy_nirq[3];

static void a6l_phy_status(struct csiphy_device *csiphy, const char *tag)
{
	struct csiphy_device_regs *regs = csiphy->regs;
	u32 s[12];
	int i;

	for (i = 0; i < 12; i++)
		s[i] = readl_relaxed(csiphy->base +
				     CSIPHY_3PH_CMN_CSI_COMMON_STATUSn(regs->offset, regs->common_status_offset, i));
	dev_info(csiphy->camss->dev,
		 "A6L_CSIPHY%u_%s irqs %u status %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
		 csiphy->id, tag, a6l_phy_nirq[csiphy->id % 3], s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7],
		 s[8], s[9], s[10], s[11]);
}
''')
sub1(f"{cs}/camss-csiphy-3ph-1-0.c",
     "\tstruct csiphy_device_regs *regs = csiphy->regs;\n\tint i;\n\n\tfor (i = 0; i < 11; i++) {\n\t\tint c = i + 22;",
     "\tstruct csiphy_device_regs *regs = csiphy->regs;\n\tint i;\n\n"
     "\tif (a6l_dbg && ++a6l_phy_nirq[csiphy->id % 3] <= 8)\n\t\ta6l_phy_status(csiphy, \"IRQ\");\n\n"
     "\tfor (i = 0; i < 11; i++) {\n\t\tint c = i + 22;")
sub1(f"{cs}/camss-csiphy-3ph-1-0.c",
     "static void csiphy_lanes_disable(struct csiphy_device *csiphy,\n\t\t\t\t struct csiphy_config *cfg)\n{\n\tstruct csiphy_device_regs *regs = csiphy->regs;\n",
     "static void csiphy_lanes_disable(struct csiphy_device *csiphy,\n\t\t\t\t struct csiphy_config *cfg)\n{\n\tstruct csiphy_device_regs *regs = csiphy->regs;\n\n"
     "\tif (a6l_dbg) {\n\t\ta6l_phy_status(csiphy, \"STOP\");\n\t\ta6l_phy_nirq[csiphy->id % 3] = 0;\n\t}\n")
print("CAMFIX2_PATCH_PASS")
