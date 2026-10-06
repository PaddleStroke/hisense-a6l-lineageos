// Exact enforce_backlight body, controlled read/write results.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "dualux_logic.h"
static struct dx_state S;
static char lcd_bl[640]="/sys/class/backlight/backlight";
static int blanked_by_us=1,power_value=4,fail_write=1,attempts;
#define LOG(...) ((void)0)
static int rd_int(const char* p,int def){(void)p;(void)def;return power_value;}
static int wr_int(const char* p,int val){(void)p;++attempts;if(fail_write)return -1;power_value=val;return 0;}
#include "backlight_method.inc"
int main(void){
 struct dx_cfg cfg;dx_default_cfg(&cfg);dx_init(&S,&cfg,DX_LCD,1);
 enforce_backlight();assert(power_value==4 && attempts==1 && blanked_by_us==0);
 fail_write=0;for(int i=0;i<100;++i)enforce_backlight();
 assert(power_value==4 && attempts==1);
 printf("F50 first_unblank_failed=1 subsequent_healthy_iterations=100 write_attempts_total=%d bl_power=%d retry_state=%d\n",attempts,power_value,blanked_by_us);
 // Positive control: a retained retry flag makes the same write succeed.
 blanked_by_us=1;enforce_backlight();assert(power_value==0 && attempts==2);
 puts("F50 retained_retry_control bl_power=0");
}
