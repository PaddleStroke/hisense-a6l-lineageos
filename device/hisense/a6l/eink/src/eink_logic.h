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
    int release_quiet_ms;	/* after a touch-drag release (pol_gesture_released): send nothing until the content has been
				 * unchanged this long (one identical capture pair) and this long has passed since the release
				 * (default 90; 0 = off). The app's settled frame, not the last drag position, is shown. */
    int release_max_ms;		/* ... bounded: after this long since the release the normal policy applies even if the
				 * content still moves (fling) (default 700) */
};
struct pol_state {
    struct pol_cfg cfg;
    double last_change, win_t, done_t;
    double release_t;		/* time of the last touch-drag release (0 = none) */
    int consec, burst, fast_on_panel, clean_n, win_n, reading_n, primed, fast_n;
};
void pol_default_cfg(struct pol_cfg *c);
void pol_init(struct pol_state *s, const struct pol_cfg *c, double now);
/* one step per capture. moving = changed-tile fraction vs the previous capture (1.0 if no previous);
 * vs_panel = changed-tile fraction vs the picture last sent to the panel (1.0 if none); busy = a command is outstanding.
 * Returns what to send now (kind POL_NONE = nothing). The caller then calls pol_sent() when it really sent it, and
 * pol_done() when the panel finished. */
struct pol_action pol_step(struct pol_state *s, double now, double moving, double vs_panel, int busy);
/* eink-round4: pol_step() = pol_observe() (motion history of one capture taken at time t) + pol_decide(). The mirror now
 * captures while a6l_epdd still drives the previous update (the capture is staged and decided at the ACK), so the two
 * halves run at different times. */
void pol_observe(struct pol_state *s, double t, double moving);
struct pol_action pol_decide(struct pol_state *s, double now, double vs_panel, int busy);
void pol_sent(struct pol_state *s, const struct pol_action *a, double now);
void pol_done(struct pol_state *s, double now);
int pol_in_burst(const struct pol_state *s);
/* The mirror held captures during a touch drag (sys.a6l.eink.no_animations) and the finger has just been lifted. */
void pol_gesture_released(struct pol_state *s, double now);

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

/* ---------------- 5. tone curve (mirror grey LUT, applied before the e-ink quantisation) ---------------- */
/* eink-round2-20261006: the filmed panel reaches the same black as stock on solid black content, but Material light
 * surfaces (grey cards ~230-245), onSurfaceVariant text and thin glyphs averaged by the 1080 -> 720 resize arrive as
 * mid greys (filmed Settings text: ink/paper 1.7 vs stock 2.4-4.6). The curve: black point = max(contrast * 60 / 100,
 * black_clip), white point = min(255 - contrast * 60 / 100, white_clip), then out = 255 * x^(gamma_x100 / 100).
 * contrast/black 0, white 255, gamma 100 = identity; contrast c with clips 0/255 and gamma 100 = the old linear LUT. */
#define TONE_DEFAULT_BLACK 24
#define TONE_DEFAULT_WHITE 232
#define TONE_DEFAULT_GAMMA 150
void tone_lut(uint8_t lut[256], int contrast, int black_clip, int white_clip, int gamma_x100);

/* ---------------- 6. capture integrity and CRTC following (eink-round3-20261006) ---------------- */
#include <stddef.h>
/* A scanout buffer is copied out of write-combined memory in 60-180 ms per 1080x2340 plane (logged "copy=" stage), i.e.
 * across 4-11 vblanks. Comparing the plane set (FB ids, geometry) only before and after cannot see a BufferQueue slot
 * that flips away and comes back (A -> B -> A): the copy then mixes rows of two different frames and is accepted
 * ("capture tuple stable ... producer reuse unproved"). guarded_copy() copies in chunks and calls still_same(ctx)
 * after every chunk; a displayed FB stays committed for at least one refresh (16.7 ms at 60 Hz), so with chunks shorter
 * than that every flip is observed. Returns n when the whole buffer was copied with every check passing, -1 as soon as a
 * check fails (dst then holds a partial copy and must be discarded). *checks = number of checks made. */
long guarded_copy(void *dst, const void *src, size_t n, size_t chunk, int (*still_same)(void *ctx), void *ctx, int *checks);
#define GUARDED_COPY_CHUNK (1u << 20)	/* 1 MiB: ~6 ms at the measured ~170 MB/s WC read rate */

/* The e-ink CRTC must follow the front (LCD) CRTC: when Android turns the LCD CRTC off (sleep, also while the e-ink is
 * the active screen) the e-ink CRTC is switched off too, so a system suspend never saves/restores an enabled lessee
 * CRTC (the 6 Oct 16:07 LCD scan-out corruption). front_follow_step() returns 1 exactly once per front-off period. */
struct front_follow { int off; };
int front_follow_step(struct front_follow *f, int front_off);

/* ---------------- 7. capture acceptance without starvation (eink-round4-20261006) ---------------- */
/* Round 3 (0014) discarded a capture when ANY plane of the KMS plane set changed during the copy. A plane that redraws
 * continuously (6 Oct 17:01: "changed plane 2" = the status bar layer, 75 discards in 15.4 s, 94 in 45.5 s) then
 * starved the mirror: a swipe back or a pulled shade never reached the e-ink. Round 4 classifies each capture:
 *   CAPK_OK     every plane's copy saw that plane's FB/geometry unchanged and the LCD layout is the same at the end;
 *               other planes may have flipped content (each one is still a consistent picture of itself) -> accept
 *   CAPK_LAYOUT the set of LCD planes or a plane's geometry/z-order/blend changed (a window appearing or a shade frame
 *               mid-animation) -> discard, retry at the next interval
 *   CAPK_TORN   a plane flipped during its own copy on every retry (A->B->A guard, the round-3 drawer artefact) -> discard
 * and never starves: once max_discards captures in a row were discarded, or max_ms passed since the first of them, the
 * newest capture is accepted anyway (CV_FORCE). The next consistent capture differs where the forced one was off and is
 * sent by the normal damage path (the cleanup update). max_discards 0 = accept everything (no discards). */
enum capk { CAPK_OK = 0, CAPK_LAYOUT = 1, CAPK_TORN = 2 };
enum cap_verdict { CV_ACCEPT = 0, CV_DISCARD = 1, CV_FORCE = 2 };
struct cap_guard { unsigned discards; double first_t; int max_discards, max_ms; unsigned forced; };
void cap_guard_init(struct cap_guard *g, int max_discards, int max_ms);
enum cap_verdict cap_guard_step(struct cap_guard *g, enum capk kind, double now);
void cap_guard_reset(struct cap_guard *g);	/* front screen off / mirror restart: a new streak starts */
#define CAP_GUARD_MAX_DISCARDS 3
#define CAP_GUARD_MAX_MS 400

/* ---------------- 8. fixed-point area resize (eink-round4-20261006) ---------------- */
/* The mirror's 1080x2340 -> 720x1440 area average cost ~100 ms per capture in doubles (logged "resize=100-104 ms"),
 * twice on the critical path after a finger lift (the release settle needs two captures). Same box filter with integer
 * weights (each output's source weights sum to exactly 4096 per axis): within 1 grey level of the double version
 * (host-tested against it), exact on flat areas. Weights are cached per geometry. */
struct area_axis { int first, n, off; };
struct area_resizer {
    int gw, gh, dw, dh;			/* cached geometry */
    struct area_axis *hx, *vy; uint16_t *hw, *vw; uint32_t *acc, *row; size_t acc_n;
};
/* src gw x gh grey -> dst (ow x oh, already filled with the letterbox colour) at ox,oy with size dw x dh, through lut.
 * Returns 0, -1 on allocation failure / bad geometry. */
int area_resize(struct area_resizer *r, const uint8_t *src, int gw, int gh, uint8_t *dst, int ow, int oh,
                int ox, int oy, int dw, int dh, const uint8_t lut[256]);
void area_resizer_free(struct area_resizer *r);
