#include <stdio.h>
#include <string.h>
/* SPDX-License-Identifier: Apache-2.0 — dualux: tests of pol_apply_refresh_mode() (eink_logic.c) */
#include "../../../src/eink_logic.h"
int main(void){ int f=0; struct pol_cfg c; pol_default_cfg(&c); c.clear_every=7;
 f|=pol_apply_refresh_mode(&c,"quality")!=0||!c.reading||c.reading_full_frac!=0.0||c.clear_every!=7;
 f|=pol_apply_refresh_mode(&c,"partial")!=0||!c.reading||c.reading_full_frac!=0.6;
 f|=pol_apply_refresh_mode(&c,"fast")!=0||c.reading||strcmp(c.active_mode,"fast");
 f|=pol_apply_refresh_mode(&c,"fastest")!=0||c.reading||strcmp(c.active_mode,"fastest")||c.quiet_ms!=150;
 f|=pol_apply_refresh_mode(&c,"auto")!=0||c.reading||c.quiet_ms!=300||c.clear_every!=7;
 f|=pol_apply_refresh_mode(&c,"bogus")==0||pol_apply_refresh_mode(&c,"")==0||c.quiet_ms!=300;
 /* quality: every still change is GC16, never REGAL/A2 */
 struct pol_state s; pol_apply_refresh_mode(&c,"quality"); pol_init(&s,&c,0);
 struct pol_action a=pol_step(&s,2.0,0,0.05,0); f|=a.kind==POL_NONE||strcmp(a.mode,"quality"); pol_sent(&s,&a,2.0); pol_done(&s,2.5);
 a=pol_step(&s,4.0,0,0.02,0); f|=a.kind==POL_NONE||strcmp(a.mode,"quality");
 printf(f?"REFRESH_TESTS_FAIL\n":"REFRESH_TESTS_PASS\n"); return f; }
