#!/usr/bin/env python3
# kvoice: port alikates' drivers/net/ipa-legacy (msm8953-mainline alikates/6.5/ipa, "ipa legacy") to Linux 7.2.3 + SDM660.
# usage: a6l-ipa-legacy-fix.py <ipa-legacy dir>   (idempotent)
import sys, os, re
d = sys.argv[1]
def edit(fn, pairs):
    p = os.path.join(d, fn); s = open(p).read(); o = s
    for a, b in pairs:
        if b in s: continue
        assert a in s, (fn, a[:70])
        s = s.replace(a, b, 1)
    if s != o: open(p, 'w').write(s); print('patched', fn)
# 1. init_dummy_netdev() is gone (6.10+): allocate a dummy netdev
edit('ipa_dma.h', [('struct net_device dummy_dev;	/* needed for NAPI */',
                    'struct net_device *dummy_dev;	/* needed for NAPI (A6L: alloc_netdev_dummy, 7.x) */')])
edit('bam.c', [
    ('netif_napi_add_tx(&ipa_dma->dummy_dev, &channel->napi,', 'netif_napi_add_tx(ipa_dma->dummy_dev, &channel->napi,'),
    ('netif_napi_add(&ipa_dma->dummy_dev, &channel->napi,', 'netif_napi_add(ipa_dma->dummy_dev, &channel->napi,'),
    ('''	init_dummy_netdev(&ipa_dma->dummy_dev);

	ret = bam_channel_init(ipa_dma, count, data);
	if (ret)
		return ret;''', '''	ipa_dma->dummy_dev = alloc_netdev_dummy(0);
	if (!ipa_dma->dummy_dev)
		return -ENOMEM;

	ret = bam_channel_init(ipa_dma, count, data);
	if (ret) {
		free_netdev(ipa_dma->dummy_dev);
		ipa_dma->dummy_dev = NULL;
		return ret;
	}'''),
    ('#include <linux/dmaengine.h>', '#include <linux/dmaengine.h>\n#include "a6l_ipa_bam.h"	/* A6L: DMA_PREP_IMM_CMD, shared with a6l_ipa_bam.ko */'),
])
# free the dummy netdev in bam_exit
p = os.path.join(d, 'bam.c'); s = open(p).read()
if 'A6L: free dummy' not in s:
    s = re.sub(r'(static void bam_exit\(struct ipa_dma \*ipa_dma\)\n\{\n)(.*?)(\n\})',
               lambda m: m.group(1) + m.group(2) + '\n\tif (ipa_dma->dummy_dev) {	/* A6L: free dummy netdev */\n\t\tfree_netdev(ipa_dma->dummy_dev);\n\t\tipa_dma->dummy_dev = NULL;\n\t}' + m.group(3),
               s, count=1, flags=re.S)
    assert 'A6L: free dummy' in s
    open(p, 'w').write(s); print('patched bam.c exit')
# 2. platform_driver.remove returns void since 6.11
edit('ipa_main.c', [
    ('static int ipa_remove(struct platform_device *pdev)', 'static int __ipa_remove(struct platform_device *pdev)'),
    ('''static void ipa_shutdown(struct platform_device *pdev)
{
	int ret;

	ret = ipa_remove(pdev);''', '''static void ipa_remove(struct platform_device *pdev)
{
	int ret = __ipa_remove(pdev);

	if (ret)
		dev_err(&pdev->dev, "remove returned %d\\n", ret);
}

static void ipa_shutdown(struct platform_device *pdev)
{
	int ret;

	ret = __ipa_remove(pdev);'''),
    # 3. SDM660 (IPA v2.6L per stock DT qcom,ipa-hw-ver = <6>; same downstream ep_mapping/SRAM tables as MSM8953)
    ('''		.compatible	= "qcom,msm8953-ipa",
		.data		= &ipa_data_v2_6l,
	},''', '''		.compatible	= "qcom,msm8953-ipa",
		.data		= &ipa_data_v2_6l,
	},
	{
		/* A6L: SDM630/SDM660, IPA v2.6L + BAM (stock: qcom,ipa-hw-ver = <6>, reg-names ipa-base/bam-base) */
		.compatible	= "qcom,sdm660-ipa",
		.data		= &ipa_data_v2_6l,
	},'''),
])
# module name: avoid clashing with mainline ipa.ko (CONFIG_QCOM_IPA=m in v67)
edit('Makefile', [('obj-$(CONFIG_QCOM_IPA_LEGACY)	+=	ipa.o', 'obj-$(CONFIG_QCOM_IPA_LEGACY)	+=	ipa_legacy.o'),
                  ('ipa-y			:=	ipa_main.o', 'ipa_legacy-y		:=	ipa_main.o'),
                  ('ipa-y			+=	$(IPA_VERSIONS:%=reg/ipa_reg-v%.o)', 'ipa_legacy-y		+=	$(IPA_VERSIONS:%=reg/ipa_reg-v%.o)'),
                  ('ipa-y			+=	$(IPA_VERSIONS:%=data/ipa_data-v%.o)', 'ipa_legacy-y		+=	$(IPA_VERSIONS:%=data/ipa_data-v%.o)')])
edit('ipa_main.c', [('		.name		= "ipa",', '		.name		= "ipa-legacy",')])
print('done')
# 4. SDM660: IPA sits behind anoc2_smmu (stock SID 0x19c0, stock ran it in S1 bypass). With iommus in DT the device gets a
#    translating DMA domain; map the modem SMEM item 1:1 (iova == phys) as mainline drivers/net/ipa/ipa_mem.c does.
edit('ipa_mem.c', [
    ('''static int ipa_smem_init(struct ipa *ipa, u32 item, size_t size)
{''', '''/* A6L: 1:1 IOMMU window over the SMEM item (mainline ipa_mem.c behaviour); module-local, one IPA instance */
static unsigned long a6l_smem_iova;
static size_t a6l_smem_size;

static void a6l_smem_map(struct device *dev, void *virt, size_t size)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t phys, addr;
	size_t len;
	int ret;

	if (!domain || domain->type == IOMMU_DOMAIN_IDENTITY)
		return;
	phys = qcom_smem_virt_to_phys(virt);
	addr = round_down(phys, PAGE_SIZE);
	len = PAGE_ALIGN(size + (phys - addr));
	ret = iommu_map(domain, addr, addr, len, IOMMU_READ | IOMMU_WRITE, GFP_KERNEL);
	if (ret) {
		dev_warn(dev, "A6L_IPA SMEM 1:1 iommu map %pa+%zx failed: %d\\n", &addr, len, ret);
		return;
	}
	a6l_smem_iova = addr;
	a6l_smem_size = len;
	dev_info(dev, "A6L_IPA SMEM mapped 1:1 at %pa size %zx\\n", &addr, len);
}

static int ipa_smem_init(struct ipa *ipa, u32 item, size_t size)
{'''),
    ('''	if (ret && actual != size) {
		dev_err(dev, "SMEM item %u has size %zu, expected %zu\\n",
			item, actual, size);
		return -EINVAL;
	}

	return 0;
}''', '''	if (ret && actual != size) {
		dev_err(dev, "SMEM item %u has size %zu, expected %zu\\n",
			item, actual, size);
		return -EINVAL;
	}

	a6l_smem_map(dev, virt, size);

	return 0;
}'''),
    ('''	// struct device *dev = &ipa->pdev->dev;
	// struct iommu_domain *domain;

	/* NOOP */''', '''	struct iommu_domain *domain = iommu_get_domain_for_dev(&ipa->pdev->dev);

	if (domain && a6l_smem_size)
		iommu_unmap(domain, a6l_smem_iova, a6l_smem_size);
	a6l_smem_size = 0;'''),
])
print('done4')
