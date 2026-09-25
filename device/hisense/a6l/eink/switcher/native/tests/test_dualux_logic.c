// SPDX-License-Identifier: Apache-2.0
/* Host unit tests for dualux_logic.c (agent dualux). cc -o t test_dualux_logic.c ../dualux_logic.c -lm && ./t */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "../dualux_logic.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static struct dx_state S; static struct dx_cfg C;
static void reset(int screen, int awake) { dx_default_cfg(&C); dx_init(&S, &C, screen, awake); }
/* a press of `ms` milliseconds on key k (0 = e-ink, 1 = power), ticking every 50 ms; returns the OR of all outputs */
static struct dx_out press(int k, int ms, double *t) {
    struct dx_out acc; dx_out_init(&acc);
#define ACC(o) do { struct dx_out _o = (o); if (_o.set_screen >= 0) acc.set_screen = _o.set_screen; acc.wake |= _o.wake; acc.sleep |= _o.sleep; \
        acc.clear |= _o.clear; acc.power_down |= _o.power_down; acc.power_up |= _o.power_up; dx_apply(&S, &_o); } while (0)
    ACC(k ? dx_power_key(&S, 1, *t) : dx_eink_key(&S, 1, *t));
    for (int e = 50; e < ms; e += 50) { ACC(dx_tick(&S, *t + e / 1000.0)); }
    *t += ms / 1000.0;
    ACC(k ? dx_power_key(&S, 0, *t) : dx_eink_key(&S, 0, *t));
    *t += 1;
    return acc;
}

static void test_eink_key(void) {
    double t = 100; struct dx_out o;
    reset(DX_LCD, 1);
    o = press(0, 120, &t); CHECK(o.set_screen == DX_EINK && S.screen == DX_EINK, "LCD awake + e-ink key -> e-ink");
    CHECK(!o.sleep && !o.clear, "no sleep/clear on the switch");
    CHECK(dx_power_grabbed(&S) && dx_front_touch_grabbed(&S) && dx_lcd_blank(&S) && dx_mirror_on(&S), "e-ink: grab power + front touch, blank LCD, mirror on");
    CHECK(!strcmp(dx_state_name(&S), "eink"), "state name eink (%s)", dx_state_name(&S));
    o = press(0, 120, &t); CHECK(!o.sleep && S.ek_pending, "e-ink awake + e-ink key: waits for a double press");
    o = dx_tick(&S, t - 1 + 0.2); CHECK(!o.sleep, "not before 300 ms");
    o = dx_tick(&S, t); CHECK(o.sleep && o.set_screen < 0 && S.screen == DX_EINK, "then sleep (stock)");
    /* double press on the e-ink = clear, no sleep */
    dx_eink_key(&S, 1, t); dx_eink_key(&S, 0, t + 0.1); o = dx_eink_key(&S, 1, t + 0.25); CHECK(o.clear && !S.ek_pending, "double press -> clear");
    o = dx_tick(&S, t + 0.3); CHECK(!o.sleep && !o.clear, "no extra action while the 2nd press is held");
    o = dx_eink_key(&S, 0, t + 0.35); CHECK(!o.sleep && !o.clear && o.set_screen < 0, "2nd release: nothing");
    o = dx_tick(&S, t + 2); CHECK(!o.sleep && !o.clear, "nothing pending"); t += 3;
    dx_set_awake(&S, 0); CHECK(!dx_power_grabbed(&S) && dx_front_touch_grabbed(&S), "asleep: power released, front touch still dropped");
    CHECK(!strcmp(dx_state_name(&S), "eink-asleep"), "state eink-asleep");
    o = press(0, 120, &t); CHECK(o.wake && S.screen == DX_EINK && !o.sleep, "e-ink asleep + e-ink key -> wake on e-ink");
    reset(DX_LCD, 0);
    o = press(0, 120, &t); CHECK(o.wake && S.screen == DX_EINK, "LCD asleep + e-ink key -> wake on the e-ink (stock wakeup source 4)");
    /* configurable: e-ink key on e-ink = clear */
    reset(DX_EINK, 1); S.cfg.ekey_in_eink = DX_EK_CLEAR;
    o = press(0, 120, &t); o = dx_tick(&S, t); CHECK(o.clear && !o.sleep && S.screen == DX_EINK, "ekey_in_eink=clear");
    /* long press = clear, in every state, never a switch */
    int st[4][2] = {{DX_LCD, 1}, {DX_LCD, 0}, {DX_EINK, 1}, {DX_EINK, 0}};
    for (int i = 0; i < 4; i++) { reset(st[i][0], st[i][1]); o = press(0, 1200, &t);
        CHECK(o.clear && o.set_screen < 0 && !o.sleep && !o.wake && S.screen == st[i][0], "long press %d -> clear only", i); }
    /* long fires while held (at >= 800 ms), exactly once */
    reset(DX_LCD, 1); dx_eink_key(&S, 1, 0); o = dx_tick(&S, 0.7); CHECK(!o.clear, "no clear at 700 ms");
    o = dx_tick(&S, 0.85); CHECK(o.clear, "clear at 850 ms"); o = dx_tick(&S, 0.9); CHECK(!o.clear, "only once");
    o = dx_eink_key(&S, 0, 1.5); CHECK(!o.clear && o.set_screen < 0, "release after long: nothing");
    /* released late without a tick */
    reset(DX_LCD, 1); dx_eink_key(&S, 1, 0); o = dx_eink_key(&S, 0, 2.0); CHECK(o.clear && o.set_screen < 0, "late release = clear");
    /* autorepeat ignored, stray release ignored */
    reset(DX_LCD, 1); o = dx_eink_key(&S, 0, 1); CHECK(o.set_screen < 0 && !o.clear, "stray release");
    dx_eink_key(&S, 1, 0); o = dx_eink_key(&S, 2, 0.1); CHECK(o.set_screen < 0 && !o.clear, "repeat ignored");
}

static void test_power_key(void) {
    double t = 10; struct dx_out o;
    reset(DX_EINK, 1);
    o = press(1, 150, &t); CHECK(o.set_screen == DX_LCD && S.screen == DX_LCD && !o.power_down && !o.power_up, "e-ink awake + power short -> LCD, Android does not see it");
    CHECK(!dx_power_grabbed(&S) && !dx_front_touch_grabbed(&S) && !dx_lcd_blank(&S) && !dx_mirror_on(&S), "LCD: nothing grabbed");
    reset(DX_EINK, 1);
    o = press(1, 1500, &t); CHECK(o.power_down && o.power_up && S.screen == DX_EINK && o.set_screen < 0, "e-ink + power long -> re-injected (power menu), stays e-ink");
    reset(DX_EINK, 1); dx_power_key(&S, 1, 0); o = dx_tick(&S, 0.40); CHECK(!o.power_down, "no injection at 400 ms");
    o = dx_tick(&S, 0.46); CHECK(o.power_down, "injected at 460 ms"); o = dx_tick(&S, 0.6); CHECK(!o.power_down, "once");
    o = dx_power_key(&S, 0, 0.9); CHECK(o.power_up && o.set_screen < 0, "release -> injected up only");
    reset(DX_LCD, 1);
    o = press(1, 150, &t); CHECK(o.set_screen < 0 && !o.power_down && !o.power_up && S.screen == DX_LCD, "LCD + power: untouched (Android sleeps)");
    reset(DX_EINK, 0);
    o = press(1, 150, &t); CHECK(o.set_screen == DX_LCD && !o.power_down, "e-ink asleep + power -> Android wakes, we go LCD (stock)");
    /* grab state is latched at key-down: the screen turning on while held must not turn the release into a switch */
    reset(DX_EINK, 0); dx_power_key(&S, 1, 0); S.screen = DX_EINK; dx_set_awake(&S, 1); o = dx_power_key(&S, 0, 0.2);
    CHECK(o.set_screen < 0 && !o.power_up, "latched: not grabbed at down -> nothing at up");
}

static void test_requests(void) {
    struct dx_out o;
    reset(DX_LCD, 1); o = dx_request(&S, "toggle"); dx_apply(&S, &o); CHECK(S.screen == DX_EINK && !o.wake, "toggle -> e-ink");
    o = dx_request(&S, "eink"); CHECK(o.set_screen < 0, "eink when e-ink: no-op");
    o = dx_request(&S, "lcd"); dx_apply(&S, &o); CHECK(S.screen == DX_LCD, "lcd");
    o = dx_request(&S, "clear"); CHECK(o.clear && o.set_screen < 0, "clear");
    o = dx_request(&S, "bogus"); CHECK(o.set_screen < 0 && !o.clear, "unknown request ignored");
    reset(DX_LCD, 0); o = dx_request(&S, "eink"); CHECK(o.wake && o.set_screen == DX_EINK, "request while asleep wakes");
    reset(DX_LCD, 1); S.cfg.mirror_in_lcd = 1; CHECK(dx_mirror_on(&S) && !dx_lcd_blank(&S), "mirror_in_lcd keeps the mirror in LCD mode");
    CHECK(dx_screen_parse("eink", DX_LCD) == DX_EINK && dx_screen_parse("lcd", DX_EINK) == DX_LCD && dx_screen_parse("x", DX_EINK) == DX_EINK, "parse");
}

static void test_brightness(void) {
    /* HLG round trip, and the composer's integer path: v = (int)(4095 * oetf(b)) */
    double worst = 0;
    for (int i = 0; i <= 1000; i++) { double b = i / 1000.0, r = dx_hlg_inv(dx_hlg_oetf(b)); if (fabs(r - b) > worst) worst = fabs(r - b); }
    CHECK(worst < 1e-9, "hlg round trip (worst %g)", worst);
    CHECK(fabs(dx_hlg_oetf(1.0 / 12) - 0.5) < 1e-12, "hlg knee");
    for (int i = 1; i <= 100; i++) { double b = i / 100.0; int v = (int)(4095 * dx_hlg_oetf(b)); double r = dx_lcd_to_linear(v, 4095, 0);
        CHECK(fabs(r - b) < 0.01 + b * 0.01, "lcd->linear b=%.2f v=%d r=%.4f", b, v, r); }
    CHECK(dx_lcd_to_linear(2048, 4096, 1) == 0.5, "linear scale passthrough");
    struct dx_fl_cfg f; dx_fl_default(&f);
    reset(DX_EINK, 1);
    CHECK(dx_frontlight_level(&S, &f, 4095, 4095, 0, 255) == 255, "full");
    CHECK(dx_frontlight_level(&S, &f, 0, 4095, 0, 255) == 0, "zero request -> off");
    int lo = dx_frontlight_level(&S, &f, 1, 4095, 0, 255); CHECK(lo == 1, "tiny request -> min level 1 (%d)", lo);
    int mid = dx_frontlight_level(&S, &f, (int)(4095 * dx_hlg_oetf(0.5)), 4095, 0, 255); CHECK(mid >= 126 && mid <= 129, "half linear -> ~128 (%d)", mid);
    f.max_pct = 50; CHECK(dx_frontlight_level(&S, &f, 4095, 4095, 0, 255) == 128, "cap 50%%");
    f.max_pct = 0; CHECK(dx_frontlight_level(&S, &f, 4095, 4095, 0, 255) == 0, "cap 0 = off");
    dx_fl_default(&f); f.gamma = 2.0; mid = dx_frontlight_level(&S, &f, (int)(4095 * dx_hlg_oetf(0.5)), 4095, 0, 255); CHECK(mid >= 62 && mid <= 66, "gamma 2 (%d)", mid);
    dx_fl_default(&f); f.enable = 0; CHECK(dx_frontlight_level(&S, &f, 4095, 4095, 0, 255) == 0, "disabled");
    dx_fl_default(&f); dx_set_awake(&S, 0); CHECK(dx_frontlight_level(&S, &f, 4095, 4095, 0, 255) == 0, "asleep -> off");
    reset(DX_LCD, 1); CHECK(dx_frontlight_level(&S, &f, 4095, 4095, 0, 255) == 0, "LCD mode -> frontlight off");
}

int main(void) {
    test_eink_key(); test_power_key(); test_requests(); test_brightness();
    printf("%d/%d checks passed\n", checks - fails, checks);
    printf(fails ? "TESTS_FAIL\n" : "TESTS_PASS\n");
    return fails != 0;
}
