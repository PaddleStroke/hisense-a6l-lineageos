// Exact enforcement method, real brightness logic, controlled sysfs reads/writes.
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "dualux_logic.h"
#define LOG(...) ((void)0)
static struct dx_state S;
static struct dx_fl_cfg FL;
static char lcd_bl[640]="/fake/lcd",fl_dir[640]="/fake/epd-backlight";
static int lcd_bl_max=100,lcd_bl_linear=1,fl_max=100,last_fl=-1;
static int request=50,actual=0,present=1,writes=0;
static int rd_int(const char* p,int def){return strstr(p,"/lcd/")?request:(present?actual:def);}
static int wr_int(const char* p,int v){(void)p;++writes;if(!present){errno=ENOENT;return -1;}actual=v;return 0;}
#include "frontlight_method.inc"
int main(void){
    struct dx_cfg cfg;dx_default_cfg(&cfg);dx_init(&S,&cfg,DX_EINK,1);dx_fl_default(&FL);
    enforce_frontlight();assert(actual==50 && last_fl==50 && writes==1);
    // LED backend disappears and reappears at the same name, initialized off.
    present=0;for(int i=0;i<20;i++)enforce_frontlight();
    present=1;actual=0;for(int i=0;i<100;i++)enforce_frontlight();
    assert(actual==0 && last_fl==50 && writes==1);
    puts("F63 backend_recreated desired=50 actual=0 cached=50 recovery_writes=0 iterations=100");
    request=60;enforce_frontlight();assert(actual==60 && writes==2);
    puts("F63 changed_slider_positive_control desired=60 actual=60");
    // Failed writes remain retryable: this is not the existing generic write-failure bug.
    request=70;present=0;enforce_frontlight();assert(last_fl==60);
    present=1;enforce_frontlight();assert(actual==70 && last_fl==70);
    puts("F63 rejected_write_positive_control recovery=1");
}
