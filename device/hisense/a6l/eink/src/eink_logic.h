// SPDX-License-Identifier: Apache-2.0
/* eink_logic.h — pure (hardware-free, unit-tested) logic of the Hisense A6L rear e-ink stack (agent eink3, 24 Sep 2026):
 *   1. mirror refresh policy (which waveform to send when: auto modes, reading mode),
 *   2. e-ink side key (code 616) -> mode state machine,
 *   3. rear touch (ft5x06, 720x1440) -> front display (1080x2340) coordinate mapping.
 * Used by a6l_eink_mirror (vendor service); tested on the host by tests/test_eink_logic.c. No libc beyond string/math. */
#pragma once
#include <stdint.h>

/* ---------------- 1. refresh policy ---------------- */
/* waveform names understood by a6l_epdd ("show"/"frame" commands) */
#define POL_QUALITY "quality"	/* GC16 full, 39 frames */
#define POL_READING "reading"	/* REGAL 16-grey partial, 39 frames, little ghosting (stock "reading" = 3) */
#define POL_FASTEST "fastest"	/* A2 black/white, ~10 frames (stock "fast" = 6/8) */
#define POL_FAST    "fast"	/* DU-like, 23 frames */

enum pol_kind { POL_NONE = 0, POL_SHOW, POL_REFRESH, POL_CLEAN_SHOW };
struct pol_action { enum pol_kind kind; const char *mode; };

struct pol_cfg {
    int quiet_ms;		/* a discrete change is sent once the content is quiet this long (default 300) */
    int settle_ms;		/* after a burst: quiet this long -> one clean update (default 1000) */
    int min_gap_ms;		/* minimum gap between the end of one update and the next command (default 150) */
    int clear_every;		/* every N updates: force GC16 of the new page (0 = never; default 10) */
    int max_per_min;		/* rate limit (default 120); one in-flight update still bounds drive rate */
    const char *active_mode;	/* burst waveform (default fastest = A2) */
    int fixed_fast;		/* explicit fast/fastest: use that waveform for taps too, no GC16 settle */
    int reading;		/* 1 = reading mode: no bursts, REGAL partial updates, periodic forced GC16 */
    int reading_refresh_every;	/* reading: every N partial updates one forced GC16 "refresh" (default 8) */
    double reading_full_frac;	/* reading: a change bigger than this fraction of the tiles -> GC16 instead of REGAL (0.6) */
    int stock;			/* 1 = stock-like: every change = one REGAL update of the latest capture, no quiet wait, no A2,
				 * no GC16 settle; forced REGAL cleanup after clear_every updates, only once idle (settle_ms) */
};
struct pol_state {
    struct pol_cfg cfg;
    double last_change, win_t, done_t;
    int consec, burst, fast_on_panel, clean_n, win_n, reading_n, primed, fast_n;
};
void pol_default_cfg(struct pol_cfg *c);
void pol_init(struct pol_state *s, const struct pol_cfg *c, double now);
/* one step per capture. moving = changed-tile fraction vs the previous capture (1.0 if no previous);
 * vs_panel = changed-tile fraction vs the picture last sent to the panel (1.0 if none); busy = a command is outstanding.
 * Returns what to send now (kind POL_NONE = nothing). The caller then calls pol_sent() when it really sent it, and
 * pol_done() when the panel finished. */
struct pol_action pol_step(struct pol_state *s, double now, double moving, double vs_panel, int busy);
void pol_sent(struct pol_state *s, const struct pol_action *a, double now);
void pol_done(struct pol_state *s, double now);
int pol_in_burst(const struct pol_state *s);

/* dualux (25 Sep): user refresh modes (persist.vendor.eink.refresh) -> policy knobs. Returns 0 if the name is known.
 *   auto    = default policy (GC16 when still, A2 bursts while moving, one clean update when it settles)
 *   quality = GC16 only, once the content is still (no A2, no REGAL)
 *   partial = REGAL partial updates (stock "reading" 3), periodic GC16 refresh, GC16 on page turns
 *   fast    = DU-like fast waveform for all changes, without automatic GC16 settle
 *   fastest = A2 waveform for all changes, without automatic GC16 settle
 *   stock   = what the stock firmware was filmed doing (eink-stock-analysis-20261006): REGAL for every change,
 *             including while scrolling (latest frame each time the panel is free), never A2/GC16 in normal use
 * Explicit fast modes honour periodic forced GC16 cleanup (clear_every counts their page updates).
 * Manual full clears remain separate from this policy.
 * clear_every / max_per_min / min_gap_ms are kept. */
int pol_apply_refresh_mode(struct pol_cfg *c, const char *name);

/* ---------------- 2. key / mode state machine ---------------- */
enum eink_mode { EINK_OFF = 0, EINK_MIRROR = 1 };
enum key_action { KA_NONE = 0, KA_TOGGLE, KA_CLEAR };
struct key_state { int down; double t_down; int long_fired; int long_ms; };
void key_init(struct key_state *k, int long_ms);	/* long press threshold (default 800 ms) */
/* value: 1 down, 0 up, 2 autorepeat (ignored). Short press (released before long_ms) -> TOGGLE on release;
 * long press -> CLEAR as soon as long_ms is reached (once), nothing on release. */
enum key_action key_event(struct key_state *k, int value, double now);
enum key_action key_tick(struct key_state *k, double now);	/* call periodically: fires the long press while held */
int mode_parse(const char *s, int def);			/* "off"/"0" -> OFF, "mirror"/"on"/"1" -> MIRROR, else def */
const char *mode_name(int m);

/* ---------------- 3. rear touch -> front mapping ---------------- */
struct tmap {
    int touch_w, touch_h;	/* rear touch axis ranges (720 x 1440) */
    int swap_xy, inv_x, inv_y;	/* raw rear touch -> panel portrait (camera on top, as the viewer sees it) */
    int panel_w, panel_h;	/* panel picture size (720 x 1440 portrait) */
    int img_x, img_y, img_w, img_h;	/* where the front picture sits on the panel (letterbox: 27,0,665,1440) */
    int front_w, front_h;	/* front display, physical orientation (1080 x 2340) */
};
/* raw rear coordinates -> front coordinates (always clamped into the front picture); returns 0 when the touch was
 * outside the mirrored picture (the white letterbox bars) */
int tmap_apply(const struct tmap *m, int rx, int ry, int *fx, int *fy);
enum fit_mode { FIT_LETTERBOX = 0, FIT_CROP = 1, FIT_STRETCH = 2 };
/* Same geometry for pixels and rear touch. Stretch retains every control and fills the panel. */
void fit_geometry(int gw, int gh, int pw, int ph, int fit, int *ox, int *oy, int *dw, int *dh);
int tmap_parse_transform(struct tmap *m, const char *s);	/* "", "swap", "invx", "invy", combos "swap,invx" */

/* ---------------- 4. KMS plane composition (drm capture source; r5 review round4 F41) ---------------- */
/* The mirror rebuilds the front picture from the LCD CRTC's planes. It must apply what the compositor programs per plane
 * (drm_hwcomposer DrmPlane: "rotation", "alpha", "pixel blend mode"), per the kernel's plane composition rules:
 * the source rectangle is reflected, then rotated counter-clockwise, then scaled to the CRTC rectangle; blending is
 *   None:     out = pa*fg + (1 - pa)*bg                 (pixel alpha ignored)
 *   Premulti: out = pa*fg + (1 - pa*fg.alpha)*bg        (fg already multiplied by its alpha)
 *   Coverage: out = pa*fg.alpha*fg + (1 - pa*fg.alpha)*bg
 * with pa = plane alpha / 0xffff. Unknown rotation bits / blend modes are rejected (no plausible wrong picture). */
#define PLANE_ROT_0 (1u << 0)
#define PLANE_ROT_90 (1u << 1)
#define PLANE_ROT_180 (1u << 2)
#define PLANE_ROT_270 (1u << 3)
#define PLANE_REFLECT_X (1u << 4)
#define PLANE_REFLECT_Y (1u << 5)
enum plane_blend { PLANE_BLEND_NONE = 2, PLANE_BLEND_PREMULTI = 0, PLANE_BLEND_COVERAGE = 1 };	/* DRM_MODE_BLEND_* ABI */
struct plane_geo {
    int cx, cy, cw, ch;		/* CRTC rectangle */
    double sx, sy, sw, sh;	/* source rectangle (pixels, from 16.16) */
    uint32_t rotation;		/* property value; default PLANE_ROT_0 */
    uint32_t alpha16;		/* plane alpha 0..0xffff; default 0xffff */
    int blend;			/* enum plane_blend; default PLANE_BLEND_PREMULTI */
};
struct plane_fb { const uint8_t *base; uint32_t pitch; int w, h; int bpp; int bgr; int has_alpha; };	/* bpp 4 = 8888, 2 = RGB565 */
int plane_supported(const struct plane_geo *q);	/* 0 = rotation/blend understood */
/* composite one plane over gray (gw x gh, luma); planes in ascending zpos order over a black background. -1 = unsupported */
int plane_compose(uint8_t *gray, int gw, int gh, const struct plane_geo *q, const struct plane_fb *fb);

/* A valid opaque 1:1 full-screen plane makes all lower z-order planes irrelevant. */
int plane_opaque_fullscreen(const struct plane_geo *q, int fw, int fh, int gw, int gh);
