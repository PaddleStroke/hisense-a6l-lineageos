// SPDX-License-Identifier: Apache-2.0
// r5 review F63 (28 Sep 2026): the UNCHANGED enforce_frontlight() of a6l_dualux.c (extracted by run-tests.sh into
// frontlight_method.inc) with the real dx_frontlight_level() and a controlled sysfs backend. Reviewer case: level applied,
// backend disappears, comes back at the same path with brightness 0 (default-state "off") -> the unchanged request must be
// re-applied without touching the slider.
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "dualux_logic.h"
static struct dx_state S; static struct dx_fl_cfg FL;
static char lcd_bl[640] = "/sys/class/backlight/backlight", fl_dir[640];
static int lcd_bl_max = 4095, lcd_bl_linear = 1, fl_max = 255, last_fl = -1;
static int lcd_v = 2048, present = 1, hw_max = 255, fl_value, writes, finds, logs, fail_write;
static double t_now;
static double now(void) { return t_now; }
#define LOG(...) (logs++)
static void find_frontlight(void) { finds++; fl_dir[0] = 0; if (present) { strcpy(fl_dir, "/sys/class/leds/epd-backlight"); fl_max = hw_max; } }
static int rd_int(const char *p, int def) {
    if (strstr(p, "/sys/class/backlight/backlight/brightness")) return lcd_v;
    if (!present) return def;
    if (strstr(p, "max_brightness")) return hw_max;
    if (strstr(p, "/brightness")) return fl_value;
    return def; }
static int wr_int(const char *p, int val) { (void)p; writes++; if (!present || fail_write) { errno = ENOENT; return -1; } fl_value = val; return 0; }
#include "frontlight_method.inc"
static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)
static double next_scan = 5;   // the daemon main loop: enforce every iteration, re-discover a missing frontlight every 5 s
static void iter(int n) { for (int i = 0; i < n; i++) { t_now += 0.1; enforce_frontlight(); if (t_now >= next_scan) { next_scan = t_now + 5; if (!fl_dir[0]) find_frontlight(); } } }
int main(void) {
    struct dx_cfg cfg; dx_default_cfg(&cfg); dx_fl_default(&FL);
    dx_init(&S, &cfg, DX_EINK, 1); find_frontlight(); finds = 0;
    iter(1); int want = dx_frontlight_level(&S, &FL, lcd_v, lcd_bl_max, lcd_bl_linear, fl_max);
    CHECK(want > 0 && fl_value == want && last_fl == want && writes == 1);
    iter(100); CHECK(writes == 1);                                    // steady state: no write churn
    // reviewer case: backend gone for 20 iterations, back at the same path with brightness 0
    present = 0; iter(20); CHECK(last_fl == -1 && finds >= 1);
    present = 1; fl_value = 0; iter(60);
    CHECK(fl_value == want && last_fl == want);                       // recovered without a slider change
    // recreated without disappearing (readback mismatch): re-applied within ~1 s
    fl_value = 0; writes = 0; iter(12); CHECK(fl_value == want && writes == 1);
    // max_brightness changes: level recomputed for the new range
    hw_max = 1023; iter(15); int want2 = dx_frontlight_level(&S, &FL, lcd_v, lcd_bl_max, lcd_bl_linear, 1023);
    CHECK(fl_max == 1023 && fl_value == want2 && want2 != want);
    // backend returns while Android is asleep: 0 applied (desired wins), never the obsolete nonzero level
    present = 0; iter(20); dx_set_awake(&S, 0); fl_value = 0; present = 1; writes = 0; iter(50);
    CHECK(fl_value == 0 && last_fl == 0);
    // backend returns while on the LCD with a stale nonzero value: forced to 0
    dx_set_awake(&S, 1); iter(20); CHECK(fl_value == want2);
    dx_init(&S, &cfg, DX_LCD, 1); iter(1); CHECK(fl_value == 0);
    fl_value = 77; iter(12); CHECK(fl_value == 0);
    // failing recovery writes: retried, bounded logging, eventually applied
    dx_init(&S, &cfg, DX_EINK, 1); iter(20); fl_value = 0; fail_write = 1; logs = 0; writes = 0; iter(300);
    CHECK(fl_value == 0 && writes >= 250 && logs <= 8);
    fail_write = 0; iter(12); CHECK(fl_value == want2);
    printf("%s %d\n", fails ? "FRONTLIGHT_RECOVER_TESTS_FAIL" : "FRONTLIGHT_RECOVER_TESTS_PASS", fails);
    return fails != 0;
}
