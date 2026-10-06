// SPDX-License-Identifier: Apache-2.0
// r5 review F50 (29 Sep 2026): the UNCHANGED enforce_backlight() of a6l_dualux.c (extracted by run-tests.sh into
// backlight_method.inc) with controlled bl_power reads/writes. Reviewer case: first unblank write fails, writes recover,
// no further face switch -> the LCD must be unblanked on a following iteration.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "dualux_logic.h"
static struct dx_state S;
static char lcd_bl[640] = "/sys/class/backlight/backlight";
static int blanked_by_us, power_value = 4, fail_write, unreadable, sticky, attempts, finds, logs;
#define LOG(...) (logs++)
static void find_lcd_backlight(void) { finds++; }
static int rd_int(const char *p, int def) { (void)p; return unreadable ? def : power_value; }
static int wr_int(const char *p, int val) { (void)p; ++attempts; if (fail_write || unreadable) { errno = EACCES; return -1; } if (!sticky) power_value = val; return 0; }
#include "backlight_method.inc"
static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
int main(void) {
    struct dx_cfg cfg; dx_default_cfg(&cfg);
    // reviewer case: awake, LCD selected, bl_power=4 blanked by us, first write fails, then 100 healthy iterations
    dx_init(&S, &cfg, DX_LCD, 1); blanked_by_us = 1; fail_write = 1;
    enforce_backlight(); CHECK(power_value == 4 && attempts == 1 && blanked_by_us == 1);
    fail_write = 0; enforce_backlight(); CHECK(power_value == 0 && blanked_by_us == 0 && attempts == 2);
    for (int i = 0; i < 100; i++) enforce_backlight();
    CHECK(attempts == 2);                                  // done: no further writes
    // many failures: bounded logging, retried every iteration, recovers
    blanked_by_us = 1; power_value = 4; attempts = 0; logs = 0; fail_write = 1;
    for (int i = 0; i < 200; i++) enforce_backlight();
    CHECK(attempts == 200 && blanked_by_us == 1 && logs <= 4);
    fail_write = 0; enforce_backlight(); CHECK(power_value == 0 && blanked_by_us == 0);
    // write "succeeds" but readback still 4 -> stays pending
    blanked_by_us = 1; power_value = 4; sticky = 1; attempts = 0;
    enforce_backlight(); enforce_backlight(); CHECK(blanked_by_us == 1 && attempts == 2);
    sticky = 0; enforce_backlight(); CHECK(power_value == 0 && blanked_by_us == 0);
    // unreadable / missing node: pending, bounded re-discovery, recovers when readable again
    blanked_by_us = 1; power_value = 4; unreadable = 1; attempts = 0; finds = 0; logs = 0;
    for (int i = 0; i < 130; i++) enforce_backlight();
    CHECK(blanked_by_us == 1 && attempts == 0 && finds == 3 && logs <= 4);
    unreadable = 0; enforce_backlight(); CHECK(power_value == 0 && blanked_by_us == 0);
    // switch to the LCD while Android is asleep: no write (Android's blank respected), restore once awake
    dx_init(&S, &cfg, DX_LCD, 0); blanked_by_us = 1; power_value = 4; attempts = 0;
    for (int i = 0; i < 10; i++) enforce_backlight();
    CHECK(attempts == 0 && power_value == 4 && blanked_by_us == 1);
    dx_set_awake(&S, 1); enforce_backlight(); CHECK(power_value == 0 && blanked_by_us == 0);
    // composer already unblanked at wake-up: nothing written, pending cleared
    dx_init(&S, &cfg, DX_LCD, 0); blanked_by_us = 1; power_value = 4; attempts = 0;
    enforce_backlight(); power_value = 0; dx_set_awake(&S, 1); enforce_backlight();
    CHECK(attempts == 0 && blanked_by_us == 0);
    // normal Android blanking while on the LCD (not ours): never touched
    dx_init(&S, &cfg, DX_LCD, 1); blanked_by_us = 0; power_value = 4; attempts = 0;
    for (int i = 0; i < 10; i++) enforce_backlight();
    CHECK(attempts == 0 && power_value == 4);
    // e-ink selected: still blanks (unchanged path)
    dx_init(&S, &cfg, DX_EINK, 1); blanked_by_us = 0; power_value = 0; attempts = 0;
    enforce_backlight(); CHECK(power_value == 4 && blanked_by_us == 1);
    printf("%s %d\n", fails ? "BACKLIGHT_RETRY_TESTS_FAIL" : "BACKLIGHT_RETRY_TESTS_PASS", fails);
    return fails != 0;
}
