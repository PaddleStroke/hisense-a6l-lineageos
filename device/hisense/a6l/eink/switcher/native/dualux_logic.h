// SPDX-License-Identifier: Apache-2.0
/* dualux_logic.h — pure (hardware-free, host unit-tested) logic of a6l_dualux, the Hisense A6L LCD <-> rear e-ink
 * switcher (agent dualux, 25 Sep 2026; docs/dualux-20260925.md). Key semantics follow the STOCK firmware
 * (HmctPhoneWindowManager / PhoneWindowManager.powerPress, decompiled from services.vdex, see the doc §2):
 *   e-ink key (evdev 616, stock keycode EPD_POWER):
 *     LCD screen, awake   -> switch to the e-ink            (stock: EpdManager.setDisplayType(4))
 *     LCD screen, asleep  -> wake up on the e-ink           (stock: setWakeupSourceType(1,4) + wakeUp)
 *     e-ink,     awake    -> go to sleep                    (stock: PowerManager.goToSleep) [configurable: clear]
 *                            single press acts after double_ms (stock waits getDoubleTapTimeout() too);
 *     e-ink, awake, double press -> full clear             (stock: EpdKeyDoubleClickFunction -> HmctPolicyHandler
 *                            msg 118 on the e-ink; the handler has an epdForceClear path — mapping likely, not proven)
 *     e-ink,     asleep   -> wake up on the e-ink
 *     long press (>= long_ms, any state)  -> full e-ink clear (our addition; stock has no long-press action)
 *   power key:
 *     LCD (any)           -> untouched (Android: sleep / wake / long-press menu)
 *     e-ink, awake        -> GRABBED by the daemon: short press = switch to the LCD, no sleep (stock: "powerPress
 *                            switch to primary screen"); held >= power_long_ms = re-injected to Android as a power
 *                            press (so the long-press power menu still works)
 *     e-ink, asleep       -> not grabbed: Android wakes up, and we switch to the LCD (stock: power wakes the primary)
 * No libc beyond string/math. */
#pragma once

enum dx_screen { DX_LCD = 0, DX_EINK = 1 };
enum dx_ekey_policy { DX_EK_SLEEP = 0, DX_EK_CLEAR = 1 };

struct dx_cfg {
    int long_ms;		/* e-ink key long press -> clear (default 800) */
    int power_long_ms;		/* power key held this long while grabbed -> re-injected to Android (default 450) */
    int ekey_in_eink;		/* DX_EK_SLEEP (stock) or DX_EK_CLEAR */
    int mirror_in_lcd;		/* 1 = keep mirroring on the e-ink while the LCD is the active screen */
    int double_ms;		/* e-ink double press window while the e-ink is active (default 300 = Android double tap) */
};

struct dx_state {
    struct dx_cfg cfg;
    int screen;			/* enum dx_screen */
    int awake;			/* Android interactive (LCD CRTC active) */
    int ek_down, ek_long, ek_double, ek_pending; double ek_t, ek_pending_t;
    int pw_down, pw_grabbed_at_down, pw_injected; double pw_t;
};

/* what the daemon must do after an event (several may be set) */
struct dx_out {
    int set_screen;		/* -1 = no change, else enum dx_screen */
    int wake;			/* inject KEY_WAKEUP */
    int sleep;			/* inject KEY_SLEEP */
    int clear;			/* full e-ink clear + redraw */
    int power_down, power_up;	/* inject KEY_POWER down / up */
};

void dx_default_cfg(struct dx_cfg *c);
void dx_init(struct dx_state *s, const struct dx_cfg *c, int screen, int awake);
void dx_out_init(struct dx_out *o);
/* evdev key value: 1 down, 0 up, 2 repeat */
struct dx_out dx_eink_key(struct dx_state *s, int value, double now);
struct dx_out dx_power_key(struct dx_state *s, int value, double now);
struct dx_out dx_tick(struct dx_state *s, double now);		/* long presses while held */
struct dx_out dx_request(struct dx_state *s, const char *req);	/* app/QS: "eink" "lcd" "toggle" "clear" */
void dx_set_awake(struct dx_state *s, int awake);
void dx_apply(struct dx_state *s, const struct dx_out *o);	/* commit o->set_screen into the state */

/* derived outputs */
int dx_power_grabbed(const struct dx_state *s);		/* grab the power key device (e-ink + awake) */
int dx_front_touch_grabbed(const struct dx_state *s);		/* drop front touches (e-ink active) */
int dx_lcd_blank(const struct dx_state *s);			/* keep the LCD backlight off (bl_power=4) */
int dx_mirror_on(const struct dx_state *s);			/* persist.vendor.eink.mode = mirror */
const char *dx_state_name(const struct dx_state *s);		/* vendor.dualux.state: lcd | eink | eink-asleep */
int dx_screen_parse(const char *v, int def);			/* "eink"/"1" -> DX_EINK, "lcd"/"0" -> DX_LCD */

/* ---------------- brightness routing ---------------- */
/* The composer (drm_hwcomposer SysfsBacklightController) writes the LCD value v = round(max * HLG_OETF(b)) when the
 * backlight's `scale` is not "linear" (else max * b). dx_lcd_to_linear() undoes that -> b in [0,1]. */
double dx_hlg_oetf(double e);
double dx_hlg_inv(double s);
double dx_lcd_to_linear(int v, int max, int hw_linear);
struct dx_fl_cfg {
    int enable;			/* 0 = frontlight always off */
    int max_pct;		/* user cap, % of the frontlight max (default 100) */
    int min_level;		/* lowest non-zero level written (default 1) */
    double gamma;		/* duty = b^gamma (default 1.0: LCD linear light ~ frontlight duty) */
};
void dx_fl_default(struct dx_fl_cfg *c);
/* frontlight level (0..fl_max) for LCD request v/lcd_max; 0 unless e-ink active + awake + enabled */
int dx_frontlight_level(const struct dx_state *s, const struct dx_fl_cfg *c, int v, int lcd_max, int hw_linear, int fl_max);
