"""Check actual V31 wrapper: one opted-in barrier, original side effects retained."""
from pathlib import Path
import subprocess,json
R=Path(__file__).resolve().parents[1]
O=R/'firmware/extracted/storage-ordered-accessor-v31-20260916'
O.mkdir(exist_ok=False)
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint16_t u16; typedef uint32_t u32;
struct sdhci_msm_host { bool use_cdr; u16 transfer_mode; } msm;
struct sdhci_pltfm_host { int unused; } plt;
struct sdhci_host { void *mmc; unsigned char *ioaddr; };
static bool opted; static unsigned request_type, checks, stores, barriers, power_checks, cdr_checks;
static char order[40]; static unsigned n;
static void mark(char c) { order[n++]=c; order[n]=0; }
#define sdhci_priv(h) (&plt)
#define sdhci_pltfm_priv(p) (&msm)
#define mmc_dev(m) (m)
#define SDHCI_COMMAND 14
#define SDHCI_TRANSFER_MODE 12
#define SDHCI_HOST_CONTROL2 62
#define SDHCI_GET_CMD(v) (((v)>>8)&63)
#define MMC_GO_IDLE_STATE 0
#define dev_info(...) do {} while(0)
static bool a6l_storage_hold_cmd0(void *dev) { return opted; }
static unsigned __sdhci_msm_check_write(struct sdhci_host *h,u16 v,int reg) {
 checks++;mark('C');if(reg==SDHCI_TRANSFER_MODE)msm.transfer_mode=v;
 if(reg==SDHCI_COMMAND && msm.use_cdr){cdr_checks++;mark('D');}
 return request_type;
}
static void wmb(void) { barriers++;mark('B'); }
static void writew_relaxed(u16 v,void *a) { stores++;mark('S'); }
static void sdhci_msm_check_power_status(struct sdhci_host *h,unsigned r) { assert(r==request_type);power_checks++;mark('P'); }
'''
post=r'''
static unsigned char io[128];static struct sdhci_host host={0,io};
static void reset(void){opted=true;request_type=checks=stores=barriers=power_checks=cdr_checks=n=0;order[0]=0;memset(&msm,0,sizeof(msm));a6l_cmd0_ordered_used=false;a6l_accessor_state_logged=false;}
int main(void){
 reset();sdhci_msm_writew(&host,0,SDHCI_COMMAND);assert(!strcmp(order,"CBS") && barriers==1 && stores==1);
 sdhci_msm_writew(&host,0,SDHCI_COMMAND);assert(!strcmp(order,"CBSCS") && barriers==1 && stores==2);
 reset();opted=false;sdhci_msm_writew(&host,0,SDHCI_COMMAND);assert(!strcmp(order,"CS") && !a6l_cmd0_ordered_used);
 reset();sdhci_msm_writew(&host,1<<8,SDHCI_COMMAND);assert(!strcmp(order,"CS") && !a6l_cmd0_ordered_used);
 reset();sdhci_msm_writew(&host,16,SDHCI_TRANSFER_MODE);assert(!strcmp(order,"CS") && msm.transfer_mode==16 && a6l_accessor_state_logged && !barriers);
 reset();msm.use_cdr=true;sdhci_msm_writew(&host,0,SDHCI_COMMAND);assert(!strcmp(order,"CDBS") && cdr_checks==1);
 reset();request_type=2;sdhci_msm_writew(&host,0,SDHCI_COMMAND);assert(!strcmp(order,"CBSP") && power_checks==1);
 reset();request_type=2;sdhci_msm_writew(&host,0,SDHCI_HOST_CONTROL2);assert(!strcmp(order,"CSP") && !barriers && power_checks==1);
 reset();opted=false;sdhci_msm_writew(&host,0,SDHCI_TRANSFER_MODE);assert(!a6l_accessor_state_logged && !barriers);
 puts("V31 accessor checks passed: first-only, opt-in, opcode, transfer state, CDR and power side effects");
}
'''
actual=(R/'tools/a6l-msm-ordered-v31.c').read_text()
kernel=Path('/home/a6l/kernel/a6l-mainline/drivers/mmc/host/sdhci-msm.c').read_text()
assert actual in kernel
(O/'accessor-test.c').write_text(pre+actual+post)
subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-parameter',str(O/'accessor-test.c'),'-o',str(O/'test')],check=True)
p=subprocess.run([str(O/'test')],check=True,capture_output=True,text=True)
(O/'report.json').write_text(json.dumps({'passed':True,'output':p.stdout,'limits':'Fake MMIO validates selection/order/side effects, not physical device behavior.'},indent=2))
print(p.stdout,end='')
