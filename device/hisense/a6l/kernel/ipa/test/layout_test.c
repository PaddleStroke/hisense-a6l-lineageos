#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
typedef uint64_t u64; typedef uint32_t u32;
#define __packed __attribute__((packed))
typedef uint16_t u16; typedef uint8_t u8; typedef uint32_t __le32; typedef uint16_t __le16;
#define __iomem
#define BIT(n) (1u<<(n))
#define WARN_ON(x) ({int _w=!!(x); if(_w) printf("WARN_ON %s\n", #x); _w;})
#define EINVAL 22
#define ALIGN(x,a) (((x)+(a)-1)&~((a)-1))
#define FIELD_GET(m,v) (((v)&(m))/((m)&-(m)))
static u32 shared_mem_val;
static inline u32 ioread32(void *p){ return shared_mem_val; }
#include "../src/ipa-hw.h"
#include "../src/ipa.h"
#define IPA_NUM_PIPES (20)
struct ipa { u32 version, smem_size, smem_restr_bytes; u32 *smem_uc_loaded; void *mmio; struct ipa_partition layout[MEM_END+1]; };
static u8 fake_mmio[0x50000];
#define FT4_EP0_OFF (2 + 3 * 0)
#define FT4_EP4_OFF (2 + 3 * 1)
#define RT4_EP0_OFF (2 + 3 * 2)
#define RT4_EP4_OFF (2 + 3 * 3)

static const u32 ipa_rules[] = {
	/* Default (zero) rules */
	0, 0,
	/* Rules for loopback */
	/* EP0 filter: dummy range16, routing index 1 */
	[FT4_EP0_OFF] = BIT(4) | (1 << 21),
	[FT4_EP0_OFF + 1] = 0xffff00,
	/* EP4 filter: dummy range16, routing index 2 */
	[FT4_EP4_OFF] = BIT(4) | (2 << 21),
	[FT4_EP4_OFF + 1] = 0xffff00,
	/* EP0 route: dummy range16, dest pipe 5, system hdr */
	[RT4_EP0_OFF] = BIT(21) | BIT(4) | (5 << 16),
	[RT4_EP0_OFF + 1] = 0xffff00,
	/* EP4 route: dummy range16, dest pipe 1, system hdr */
	[RT4_EP4_OFF] = BIT(21) | BIT(4) | (1 << 16),
	[RT4_EP4_OFF + 1] = 0xffff00,
	[RT4_EP4_OFF + 2] = 0,
};
static void ipa_partition_put(struct ipa *ipa, u32 *offset,
			      enum ipa_part_id id, u32 size_words, u32 align_words)
{
	u32 __iomem *ptr = ipa->mmio + REG_IPA_SRAM_SW_FIRST_v2_5 +
		(ipa->version < 25 ? ipa->smem_restr_bytes : 0) + *offset;
	bool first_canary = true;
	u32 canary = 0xdeadbeaf;

	if (id == MEM_DRV) {
		/* Keep uc_loaded status in SRAM and don't override it */
		ipa->smem_uc_loaded = ptr;
		if (*ptr == 0x10ADEDFF)
			canary = 0x10ADEDFF;
	}

	while ((first_canary || ALIGN(*offset, 4 * align_words) != *offset) &&
	       *offset < ipa->smem_size) {
		*(ptr++) = canary;
		*offset += 4;
		first_canary = false;
	}

	ipa->layout[id].offset = *offset + ipa->smem_restr_bytes;
	ipa->layout[id].size = size_words * 4;

	*offset = *offset + size_words * 4;
}
static int ipa_partition_mem(struct ipa *ipa)
{
	u32 offset, val;

	val = ioread32(ipa->mmio + REG_IPA_SHARED_MEM);

	ipa->smem_restr_bytes = FIELD_GET(IPA_SHARED_MEM_BADDR_BMSK, val);
	ipa->smem_size = FIELD_GET(IPA_SHARED_MEM_SIZE_BMSK, val);

	if (WARN_ON(ipa->smem_restr_bytes > ipa->smem_size ||
		    (ipa->smem_restr_bytes & 3) || ipa->smem_size & 3))
		return -EINVAL;

	ipa->smem_size -= ipa->smem_restr_bytes;
	offset = 0x280;

	ipa_partition_put(ipa, &offset, MEM_FT_V4, IPA_NUM_PIPES + 2, 2);
	ipa_partition_put(ipa, &offset, MEM_FT_V6, IPA_NUM_PIPES + 2, 2);
	ipa_partition_put(ipa, &offset, MEM_RT_V4, 7, 2);
	ipa_partition_put(ipa, &offset, MEM_RT_V6, 7, 2);
	ipa_partition_put(ipa, &offset, MEM_MDM_HDR, 80, 2);
	ipa_partition_put(ipa, &offset, MEM_DRV, sizeof(ipa_rules) / 4, 1);

	if (ipa->version == 25)
		ipa_partition_put(ipa, &offset, MEM_MDM_HDR_PCTX, 128, 2);
	else if (ipa->version == 26)
		ipa_partition_put(ipa, &offset, MEM_MDM_COMP, 128, 2);

	ipa_partition_put(ipa, &offset, MEM_MDM,
			  (ipa->smem_size - offset) / 4 - 2, 1);
	ipa_partition_put(ipa, &offset, MEM_END, 0, 2);

	return 0;
}
static const char *names[]={"DRV","FT_V4","FT_V6","RT_V4","RT_V6","MDM_HDR","MDM_COMP","MDM_HDR_PCTX","MDM","END"};
int main(int argc,char**argv){
  u32 vals[]={0x00002000,0x00003000,0x01002000,0x00004000};
  for(unsigned k=0;k<sizeof vals/4;k++){
    struct ipa ipa={.version=26,.mmio=fake_mmio}; shared_mem_val=vals[k]; memset(fake_mmio,0,sizeof fake_mmio);
    int r=ipa_partition_mem(&ipa);
    printf("SHARED_MEM=0x%08x rc=%d restr=0x%x size=0x%x\n",vals[k],r,ipa.smem_restr_bytes,ipa.smem_size);
    u32 prev_end=0; int bad=0;
    int order[]={MEM_FT_V4,MEM_FT_V6,MEM_RT_V4,MEM_RT_V6,MEM_MDM_HDR,MEM_DRV,MEM_MDM_COMP,MEM_MDM,MEM_END};
    for(int i=0;i<9;i++){int id=order[i]; struct ipa_partition p=ipa.layout[id];
      printf("  %-8s off=0x%04x size=%5u end=0x%04x %s\n",names[id],p.offset,p.size,p.offset+p.size,(p.offset<prev_end)?"OVERLAP":"");
      if(p.offset<prev_end) bad=1; if(p.size) prev_end=p.offset+p.size; if (id==MEM_FT_V4||id==MEM_FT_V6||id==MEM_RT_V4||id==MEM_RT_V6||id==MEM_MDM_HDR||id==MEM_MDM_COMP) if(p.offset&7) {printf("  MISALIGNED %s\n",names[id]);bad=1;}}
    if(prev_end > ipa.smem_size+ipa.smem_restr_bytes){printf("  PAST END\n");bad=1;}
    /* QMI view (as init_modem_driver_req builds it) */
    printf("  QMI: hdr %u..%u rt4 start %u end-idx %u rt6 start %u end-idx %u ft4 %u ft6 %u mdm %u+%u zip %u..%u\n",
      ipa.layout[MEM_MDM_HDR].offset, ipa.layout[MEM_MDM_HDR].offset+ipa.layout[MEM_MDM_HDR].size-1,
      ipa.layout[MEM_RT_V4].offset, ipa.layout[MEM_RT_V4].size/4-1, ipa.layout[MEM_RT_V6].offset, ipa.layout[MEM_RT_V6].size/4-1,
      ipa.layout[MEM_FT_V4].offset, ipa.layout[MEM_FT_V6].offset, ipa.layout[MEM_MDM].offset, ipa.layout[MEM_MDM].size,
      ipa.layout[MEM_MDM_COMP].offset, ipa.layout[MEM_MDM_COMP].offset+ipa.layout[MEM_MDM_COMP].size-1);
    /* downstream v2.6L (ipa_ram_mmap.h) for the same restricted base */
    u32 rb=ipa.smem_restr_bytes;
    printf("  downstream v2.6L: ft4 %u ft6 %u rt4 %u(idx 0..6 modem, 7..14 apps) rt6 %u mdm_hdr %u..%u comp 0x510+512 mdm %u+6376 end 0x2000\n",
       0x288+rb,0x2e8+rb,0x348+rb,0x388+rb,0x3c8+rb,0x3c8+rb+319,0x714+rb);
    printf("  %s\n", bad?"LAYOUT_FAIL":"LAYOUT_OK");
  }
  printf("ipa_rules words=%zu\n", sizeof(ipa_rules)/4);
  printf("sizeof fifo_desc %zu v4_rule_init %zu v6_rule_init %zu hdr_local %zu hdr_system %zu dma_smem %zu\n", sizeof(struct fifo_desc), sizeof(struct ipa_ip_v4_rule_init), sizeof(struct ipa_ip_v6_rule_init), sizeof(struct ipa_hdr_init_local), sizeof(struct ipa_hdr_init_system), sizeof(struct ipa_hw_imm_cmd_dma_shared_mem));
  return 0;
}
