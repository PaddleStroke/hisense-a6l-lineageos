"""Run the actual V31 C state machine against bounded fake SDHCI registers."""
from pathlib import Path
import re, subprocess, json
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/storage-ordered-logic-v31-20260916'
source=ROOT/'tools/a6l-cmd0-ordered-v31.c'
header=Path('/home/a6l/kernel/a6l-mainline/drivers/mmc/host/sdhci.h').read_text().replace('\\\n', ' ')
defs={m.group(1):m.group(0) for m in re.finditer(r'^#define\s+(SDHCI_\w+)\b[^\n]*(?:\\\n[^\n]*)*',header,re.M)}
needed=set(re.findall(r'\bSDHCI_\w+',source.read_text()))|{'SDHCI_INT_TIMEOUT'}
for _ in range(8):
    needed.update(dep for name in list(needed) for dep in re.findall(r'\bSDHCI_\w+',defs[name]) if dep!=name)
OUT.mkdir(exist_ok=False)
pre=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef uint16_t u16;
struct device { struct { int runtime_status,usage_count; unsigned disable_depth; } power; }; struct work_struct { int unused; };
struct mmc_request { int unused; };
struct mmc_command { unsigned opcode,arg,flags,resp[4]; int error; struct mmc_request *mrq; };
struct mmc_ios { unsigned clock,vdd,bus_width,timing,power_mode; };
struct mmc_host { unsigned actual_clock; struct mmc_ios ios; };
struct sdhci_host { struct mmc_host *mmc; struct mmc_command *cmd; bool runtime_suspended; unsigned flags,clock,quirks,quirks2; int irq,lock; };
static struct device device;
#define mmc_dev(x) (&device)
#define MMC_GO_IDLE_STATE 0
#define READ_ONCE(x) (x)
static int atomic_read(int *v) { return *v; }
typedef int atomic_t;
#define ATOMIC_INIT(x) (x)
static int atomic_inc_return(int *v) { return ++*v; }
static bool opted=true, queued=false, locked=false;
static int writes=0,commands=0,signals=0,timer_deleted=0;
static u32 regs[256]; static char logs[30000]; static size_t logused;
#define spin_lock_irqsave(l,f) do { assert(!locked); locked=true; (f)=0; } while(0)
#define spin_unlock_irqrestore(l,f) do { assert(locked); locked=false; (void)(f); } while(0)
static void dev_info(struct device *d,const char *fmt,...) { va_list ap; va_start(ap,fmt); logused+=vsnprintf(logs+logused,sizeof(logs)-logused,fmt,ap); va_end(ap); assert(logused<sizeof(logs)); }
static bool a6l_storage_hold_cmd0(struct device *d) { return opted; }
static u32 sdhci_readl(struct sdhci_host *h,int reg) { assert(locked); return regs[reg]; }
static u16 sdhci_readw(struct sdhci_host *h,int reg) { return sdhci_readl(h,reg); }
static unsigned char sdhci_readb(struct sdhci_host *h,int reg) { return sdhci_readl(h,reg); }
#define DECLARE_DELAYED_WORK(n,f) int n
#define msecs_to_jiffies(x) (x)
static void schedule_delayed_work(int *work,int delay) { assert(delay==4000); assert(!queued); queued=true; }
static void sdhci_del_timer(struct sdhci_host *h,struct mmc_request *m) { assert(locked); timer_deleted++; }
'''
post=r'''
static void sdhci_writel(struct sdhci_host *h,u32 value,int reg) { assert(locked); assert(reg==SDHCI_SIGNAL_ENABLE); regs[reg]=value; writes++; signals++; }
static void sdhci_writew(struct sdhci_host *h,u16 value,int reg) { assert(locked); assert(reg==SDHCI_COMMAND); assert(regs[SDHCI_SIGNAL_ENABLE]==0); assert(value==0); regs[reg]=value; writes++; commands++; }
'''
test=r'''
static struct mmc_host mmc; static struct mmc_request mrq; static struct mmc_command cmd; static struct sdhci_host host;
static void reset(void) {
 memset(&cmd,0,sizeof(cmd)); memset(&host,0,sizeof(host)); memset(regs,0,sizeof(regs));
 host.mmc=&mmc; host.cmd=&cmd; cmd.mrq=&mrq; opted=true; queued=false; locked=false;
 writes=commands=signals=timer_deleted=0; logs[0]=0; logused=0;
 a6l_cmd0_used=false; a6l_cmd0_step=a6l_cmd0_polls=0;
 regs[SDHCI_SIGNAL_ENABLE]=0x00ff8001; regs[SDHCI_INT_ENABLE]=0x00ff8001;
}
static void start(void) { unsigned long f; spin_lock_irqsave(&host.lock,f); assert(a6l_cmd0_stage(&host,&cmd,0)); assert(!a6l_cmd0_stage(&host,&cmd,0)); spin_unlock_irqrestore(&host.lock,f); assert(timer_deleted==1); }
static void tick(void) { assert(queued); queued=false; a6l_cmd0_work(NULL); assert(!locked); }
static void issue(void) { start(); tick(); assert(signals==1 && commands==0); tick(); assert(commands==0 && queued); assert(strstr(logs,"PREISSUE_CHECKED")); tick(); assert(commands==1); }
static void outcome(const char *s) { assert(strstr(logs,s)); assert(!queued); assert(regs[SDHCI_INT_ENABLE]==0x00ff8001); }
int main(void) {
 reset(); opted=false; assert(!a6l_cmd0_stage(&host,&cmd,0)); assert(!writes&&!queued);
 reset(); cmd.opcode=8; assert(!a6l_cmd0_stage(&host,&cmd,0)); assert(!writes&&!queued);
 reset(); issue(); regs[SDHCI_INT_STATUS]=SDHCI_INT_RESPONSE; tick(); assert(commands==1&&signals==1); tick(); outcome("released_to_normal_irq"); assert(signals==2&&regs[SDHCI_SIGNAL_ENABLE]==0x00ff8001);
 reset(); issue(); regs[SDHCI_INT_STATUS]=SDHCI_INT_ERROR|SDHCI_INT_TIMEOUT; tick(); outcome("hold_command_error"); assert(signals==1&&commands==1);
 reset(); issue(); tick(); assert(queued); tick(); outcome("hold_no_completion"); assert(commands==1&&signals==1);
 reset(); issue(); tick(); regs[SDHCI_INT_STATUS]=SDHCI_INT_RESPONSE; tick(); tick(); outcome("released_to_normal_irq"); assert(commands==1);
 reset(); start(); regs[SDHCI_INT_STATUS]=SDHCI_INT_RESPONSE; tick(); outcome("hold_invalid_or_stale_baseline"); assert(writes==0);
 reset(); start(); tick(); regs[SDHCI_PRESENT_STATE]=SDHCI_CMD_INHIBIT; tick(); outcome("hold_preissue_state_changed"); assert(commands==0);
 reset(); issue(); regs[SDHCI_INT_STATUS]=~0U; tick(); outcome("hold_unreadable_registers"); assert(signals==1);
 reset(); issue(); regs[SDHCI_INT_STATUS]=SDHCI_INT_RESPONSE; tick(); regs[SDHCI_INT_STATUS]=SDHCI_INT_TIMEOUT; tick(); outcome("hold_completion_changed"); assert(signals==1);
 reset(); start(); host.cmd=NULL; tick(); outcome("hold_lifetime_or_power_changed"); assert(writes==0);
 reset(); start(); host.runtime_suspended=true; tick(); outcome("hold_lifetime_or_power_changed"); assert(writes==0);
 reset(); issue(); regs[SDHCI_INT_STATUS]=SDHCI_INT_RESPONSE; regs[SDHCI_PRESENT_STATE]=SDHCI_CMD_INHIBIT; tick(); tick(); outcome("hold_no_completion"); assert(signals==1);
 reset(); start(); tick(); tick(); host.runtime_suspended=true; tick(); outcome("hold_lifetime_or_power_changed"); assert(commands==0);
 reset(); start(); tick(); tick(); host.cmd=NULL; tick(); outcome("hold_lifetime_or_power_changed"); assert(commands==0);
 puts("PASS: 15 guarded stage and error-path scenarios; one command at most; no status acknowledgement or INT_ENABLE mutation");
}
'''
(OUT/'harness.c').write_text(pre+'\n'+'\n'.join(defs[n] for n in sorted(needed))+'\n'+post+'\n'+source.read_text()+'\n'+test)
binary=Path('/tmp/a6l-v31-stage-test')
subprocess.run(['gcc','-std=gnu11','-O1','-g','-fsanitize=undefined','-o',str(binary),str(OUT/'harness.c')],check=True)
r=subprocess.run([str(binary)],check=True,capture_output=True,text=True)
(OUT/'report.json').write_text(json.dumps({'passed':True,'scenarios':15,'result':r.stdout,'limits':'Actual C branch logic with fake registers; not hardware, IRQ concurrency or electrical validation.'},indent=2))
print(r.stdout)
