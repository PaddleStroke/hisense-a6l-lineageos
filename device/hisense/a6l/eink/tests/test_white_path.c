// SPDX-License-Identifier: Apache-2.0
/* eink-round6d host test: pure white stays pure white and pure black stays pure black on every software stage between
 * the LCD planes and the panel image handed to the waveform library (user, 7 Oct round 7i: "the white is not white
 * anymore, almost white with gray speckles"). Stages: mirror plane composition (opaque, premultiplied, transparent and
 * half-transparent overlays), the round-2 tone curve (rc default 24,232,150), the round-4 fixed-point area resize
 * (1080x2340 -> 720x1440, stretch and letterbox geometry), a6l_epdd's input conversion (ordered table dither + direct
 * pack, Floyd-Steinberg, none, both orientations, rot180). Also documents what makes speckles: a near-white grey (254,
 * 250) that reaches the ordered dither becomes a fixed 8x8 dot pattern of the next lower panel level. */
#define A6L_EPDD_HOSTTEST 1
#define main a6l_epdd_main
#include "../src/a6l_epdd.c"
#undef main
#include "../src/eink_logic.h"

static int a6l_test_power(int on) { (void)on; return 0; }
static int a6l_test_flip(uint32_t id) { (void)id; return 0; }
static int a6l_test_modeset(void) { return 0; }

static int fails;
#define EXPECT(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)
enum { GW = 1080, GH = 2340 };
static uint8_t gray[GW * GH], outb[IW * IH];
static uint8_t fbw[GW * GH * 4], fbo[GW * 75 * 4];

static int all_eq(const uint8_t *p, size_t n, uint8_t v) { for (size_t i = 0; i < n; i++) if (p[i] != v) return 0; return 1; }
static int img_all(uint8_t v) {	/* every library pixel = v v v ff */
    const uint32_t *d = img.data; uint32_t want = 0xff000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | v;
    for (size_t i = 0; i < (size_t)IW * IH; i++) if (d[i] != want) return 0;
    return 1;
}
static double img_frac_below(uint8_t v) { const uint8_t *d = img.data; size_t n = 0; for (size_t i = 0; i < (size_t)IW * IH; i++) if (d[4 * i] < v) n++; return (double)n / ((double)IW * IH); }

int main(void) {
    img.data = calloc(1, RGBA); img.size = RGBA;
    uint8_t lut[256]; tone_lut(lut, 0, TONE_DEFAULT_BLACK, TONE_DEFAULT_WHITE, TONE_DEFAULT_GAMMA);
    int top = 1, bot = 1; for (int i = TONE_DEFAULT_WHITE; i < 256; i++) top &= lut[i] == 255; for (int i = 0; i <= TONE_DEFAULT_BLACK; i++) bot &= lut[i] == 0;
    EXPECT(top && bot, "tone 24,232,150: 232..255 -> 255, 0..24 -> 0");
    for (int c = 0; c <= 100; c += 25) { uint8_t l2[256]; tone_lut(l2, c, TONE_DEFAULT_BLACK, TONE_DEFAULT_WHITE, TONE_DEFAULT_GAMMA);
        if (l2[255] != 255 || l2[0] != 0) { printf("FAIL contrast %d: lut[255]=%d lut[0]=%d\n", c, l2[255], l2[0]); fails++; } }
    EXPECT(1, "tone: every contrast 0..100 keeps 255 -> 255 and 0 -> 0 (above)");

    /* composition: opaque white app plane, then a status-bar overlay with transparent, opaque-white and 50 % white pixels */
    for (int c = 0; c < 2; c++) {
        uint8_t v = c ? 0 : 255;
        memset(gray, 0, sizeof gray);
        for (size_t i = 0; i < sizeof fbw / 4; i++) { fbw[4 * i] = fbw[4 * i + 1] = fbw[4 * i + 2] = v; fbw[4 * i + 3] = 255; }
        struct plane_geo q = {0, 0, GW, GH, 0, 0, GW, GH, PLANE_ROT_0, 0xffff, PLANE_BLEND_PREMULTI};
        struct plane_fb f = {fbw, GW * 4, GW, GH, 4, 0, 1};
        plane_compose(gray, GW, GH, &q, &f);
        for (int i = 0; i < GW * 75; i++) {	/* premultiplied: (0,0,0,0) transparent, (v,v,v,255), (v/2,v/2,v/2,128) */
            int k = i % 3; uint8_t a = k == 0 ? 0 : k == 1 ? 255 : 128, cv = k == 0 ? 0 : k == 1 ? v : (uint8_t)(v * 128 / 255);
            fbo[4 * i] = fbo[4 * i + 1] = fbo[4 * i + 2] = cv; fbo[4 * i + 3] = a; }
        struct plane_geo qs = {0, 0, GW, 75, 0, 0, GW, 75, PLANE_ROT_0, 0xffff, PLANE_BLEND_PREMULTI};
        struct plane_fb fs = {fbo, GW * 4, GW, 75, 4, 0, 1};
        plane_compose(gray, GW, GH, &qs, &fs);
        char m[96]; snprintf(m, sizeof m, "compose: %s app + transparent/opaque/half-alpha %s status bar -> all %d", v ? "white" : "black", v ? "white" : "black", v);
        EXPECT(all_eq(gray, sizeof gray, v), m);
        struct area_resizer r; memset(&r, 0, sizeof r);
        memset(outb, 255 - v, sizeof outb);
        area_resize(&r, gray, GW, GH, outb, IH, IW, 0, 0, IH, IW, lut);	/* stretch to 720x1440 (portrait: w=IH, h=IW) */
        snprintf(m, sizeof m, "resize + tone (stretch 1080x2340 -> 720x1440): all %d", v); EXPECT(all_eq(outb, sizeof outb, v), m);
        int ox, oy, dw, dh; fit_geometry(GW, GH, IH, IW, 0 /* letterbox */, &ox, &oy, &dw, &dh);
        memset(outb, 255, sizeof outb); area_resize(&r, gray, GW, GH, outb, IH, IW, ox, oy, dw, dh, lut);
        int ok = 1; for (int y = oy; y < oy + dh; y++) ok &= all_eq(outb + (size_t)y * IH + ox, (size_t)dw, v);
        snprintf(m, sizeof m, "resize + tone (letterbox %dx%d at %d,%d): picture all %d", dw, dh, ox, oy, v); EXPECT(ok, m);
        area_resizer_free(&r);
        /* a6l_epdd input conversion, every dither and orientation */
        for (int d = 0; d <= 2; d++) for (int rr = 0; rr < 2; rr++) for (int o = 0; o < 2; o++) {
            dither = d; rot180 = rr; int w = o ? IW : IH, h = o ? IH : IW;
            memset(outb, v, sizeof outb); memset(img.data, 0x55, RGBA);
            if (load_grey(outb, w, h)) { printf("FAIL load_grey %dx%d\n", w, h); fails++; continue; }
            if (!img_all(v)) { printf("FAIL epdd input: grey %d -> not all %d (dither %d rot180 %d %dx%d)\n", v, v, d, rr, w, h); fails++; }
        }
        snprintf(m, sizeof m, "epdd input (ordered/FS/none, rot180 0/1, portrait/landscape): grey %d -> panel %d everywhere", v, v); EXPECT(1, m);
    }
    /* how speckles are made: a near-white grey into the ordered dither */
    dither = 2; rot180 = 0;
    memset(outb, 254, sizeof outb); load_grey(outb, IH, IW); double f254 = img_frac_below(255);
    memset(outb, 250, sizeof outb); load_grey(outb, IH, IW); double f250 = img_frac_below(255);
    printf("info near-white into the ordered dither: grey 254 -> %.1f %% of pixels at level 238, grey 250 -> %.1f %%\n", f254 * 100, f250 * 100);
    EXPECT(f254 > 0.05 && f254 < 0.07 && f250 > 0.2, "ordered dither: 254 = 4/64 dots of 238, 250 more (a near-white grey that reaches epdd IS a speckle pattern)");
    printf("A6L_WHITE_PATH_TEST %s (%d failures)\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
}
