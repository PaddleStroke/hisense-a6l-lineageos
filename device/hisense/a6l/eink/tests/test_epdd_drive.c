// SPDX-License-Identifier: Apache-2.0
/* r5 review round4 F35 host test: a6l_epdd's real drive()/run_update()/exec_cmd() with a fake rail switch and fake page
 * flips (A6L_EPDD_HOSTTEST hooks) and the fake TCON library. Rail values are never touched here: only the error handling
 * is checked. Rail-on failure -> no scanout + rails off + ERR; rail-off failure -> ERR (one retry); page-flip failure
 * after partial delivery -> rails off + ERR; after any failure the next picture is preceded by a full recovery clear
 * (white GC16 + INIT) and only then reported OK; a failed recovery keeps the panel state unknown. */
#define A6L_EPDD_HOSTTEST 1
#define main a6l_epdd_main
#include "../src/a6l_epdd.c"
#undef main

void *Init_Eink_SWTcon(struct buf *ring, int n, uint32_t *cfg, void *flash, uint32_t size, void *info);
int ModeDecision_MirrorMode(struct buf *img, void *handle, int t1, int t2, int force, int mode);
uint8_t Update_Display_Image(struct buf *b, void *handle);

static int on_err, off_err_count, flip_fail_at = -1, nflips, rails_on, n_on, n_off;
static int a6l_test_power(int on) {
    if (on) { n_on++; if (on_err) return -1; rails_on = 1; return 0; }
    n_off++; if (off_err_count > 0) { off_err_count--; return -1; }
    rails_on = 0; return 0;
}
static int modeset_err, n_modeset;
static int a6l_test_modeset(void) { n_modeset++; return modeset_err ? -1 : 0; }
static double fake_suspended, suspend_at_flip = -1;
static double a6l_test_suspended_s(void) { return fake_suspended; }
static int a6l_test_flip(uint32_t id) {
    (void)id;
    if (suspend_at_flip >= 0 && nflips >= suspend_at_flip) { fake_suspended += 40; suspend_at_flip = -1; }
    if (flip_fail_at >= 0 && nflips >= flip_fail_at) return -1;
    if (!rails_on) { printf("FAIL scanout with the rails off\n"); exit(1); }
    nflips++; return 0;
}

static int fails;
#define EXPECT(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)
static uint8_t px[IW * IH];
static char reply[512];
extern int fake_tcon_last_force, fake_tcon_last_mode;
static int frame(void) { return exec_cmd("frame 720 1440 quality", px, reply, sizeof reply); }

int main(void) {
    tc_init = Init_Eink_SWTcon; tc_decide = ModeDecision_MirrorMode; tc_update = Update_Display_Image;
    a6l_test_suspended_hook = a6l_test_suspended_s;
    for (int i = 0; i < RING; i++) { ring[i].data = calloc(1, FRAME); ring[i].size = FRAME; }
    img.data = calloc(1, RGBA); img.size = RGBA; last_img = malloc(RGBA);
    static uint8_t flash[FLASH]; memset(flash, 0x5a, sizeof flash);
    if (waveform_valid(flash)) { printf("FAIL fake tcon\n"); return 1; }
    fa.map = malloc(FRAME); fb2.map = malloc(FRAME); dfd = 100; started = 1; lead = 2; tail = 2;
    memset(px, 128, sizeof px);
    img_fill(0xff); run_update(1, 0, "clear"); img_fill(0xff); run_update(0, mode, "reset-to-white");
    EXPECT(!panel_unknown && !rails_on, "start-up: panel known, rails off");

    int u = updates; nflips = 0;
    EXPECT(frame() == 0 && !strncmp(reply, "OK", 2) && updates == u + 1 && !rails_on, "normal frame: OK, one update, rails off");
    u = updates;
    EXPECT(exec_cmd("frame 720 1440 clean", px, reply, sizeof reply) == 0 && updates == u + 1 && !rails_on,
           "periodic clean: one update of actual page, no white/INIT sequence");
    EXPECT(fake_tcon_last_force == 1 && fake_tcon_last_mode == 2 && !memcmp(last_img, img.data, RGBA),
           "periodic clean: forced GC16 with latest pixels remembered");
    u = updates;
    EXPECT(exec_cmd("frame 720 1440 fastest force", px, reply, sizeof reply) == 0 && updates == u + 1,
           "stock fast ghost refresh: one update of actual page");
    EXPECT(fake_tcon_last_force == 1 && fake_tcon_last_mode == 8 && last_mode == 8,
           "stock fast ghost refresh preserves selected A2 mode argument");
    u = updates;
    EXPECT(exec_cmd("refresh", NULL, reply, sizeof reply) == 0 && updates == u + 1 && !rails_on,
           "manual ghost refresh: one update, no white or INIT sequence");
    EXPECT(fake_tcon_last_force == 1 && fake_tcon_last_mode == 8,
           "manual refresh preserves last selected mode");
    EXPECT(exec_cmd("frame 720 1440 fastest wrong", px, reply, sizeof reply) != 0 && updates == u + 1,
           "unknown frame option rejected before any drive");

    on_err = 1; nflips = 0; n_off = 0;
    EXPECT(frame() != 0 && !strncmp(reply, "ERR", 3), "rail-on failure -> ERR (was OK shown)");
    EXPECT(nflips == 0, "rail-on failure: no waveform scanout");
    EXPECT(n_off >= 1 && !rails_on, "rail-on failure: rails switched off");
    EXPECT(panel_unknown, "rail-on failure: panel state unknown");

    on_err = 0; u = updates;
    EXPECT(frame() == 0 && !strncmp(reply, "OK", 2), "next frame after the failure: OK");
    EXPECT(updates == u + 3 && !panel_unknown, "  ... preceded by the full recovery clear (white + INIT), panel known");

    off_err_count = 2; u = updates;
    EXPECT(frame() != 0 && !strncmp(reply, "ERR", 3) && rails_off_fail == 1, "rail-off failure (after retry) -> ERR, counted");
    EXPECT(panel_unknown, "rail-off failure: panel state unknown");
    exec_cmd("status", NULL, reply, sizeof reply);
    EXPECT(strstr(reply, "rails_off_fail=1") && strstr(reply, "panel=unknown"), "status reports rails_off_fail and panel=unknown");
    off_err_count = 1;
    EXPECT(frame() == 0 && rails_off_fail == 1 && !rails_on, "single rail-off write failure: retried, OK, rails off");

    nflips = 0; flip_fail_at = 5;
    EXPECT(frame() != 0 && !strncmp(reply, "ERR", 3), "page-flip failure after partial delivery -> ERR");
    EXPECT(nflips == 5 && !rails_on && panel_unknown, "  ... rails switched off, panel state unknown");
    flip_fail_at = -1;

    on_err = 1;
    EXPECT(frame() != 0 && panel_unknown, "failed recovery clear -> ERR, panel still unknown");
    EXPECT(exec_cmd("refresh", NULL, reply, sizeof reply) != 0, "refresh while recovery impossible -> ERR");
    on_err = 0; u = updates;
    EXPECT(exec_cmd("refresh", NULL, reply, sizeof reply) == 0 && !panel_unknown && updates == u + 3, "refresh: recovery clear then redraw");
    on_err = 1; frame(); on_err = 0;
    EXPECT(exec_cmd("clear", NULL, reply, sizeof reply) == 0 && !panel_unknown, "explicit full clear also re-establishes a known panel");
    EXPECT(frame() == 0, "frame after recovery: OK");

    /* eink-round2 0007: --overlap-gen 1 generates in a worker thread during rails-on + lead scans. Same frames, same
     * order, rails only around the scanout, waveform scanout only after every frame exists. */
    nflips = 0; n_on = 0; int serial_frames = last_frames, serial_flips;
    EXPECT(frame() == 0 && !rails_on, "serial reference frame: OK"); serial_flips = nflips;
    uint8_t serial_last = ((uint8_t *)fa.map)[0];
    overlap_gen = 1; nflips = 0; n_on = 0; u = updates;
    EXPECT(frame() == 0 && !strncmp(reply, "OK", 2) && updates == u + 1 && !rails_on && n_on == 1, "overlap: OK, one update, one rail cycle, rails off");
    EXPECT(last_frames == serial_frames && ((uint8_t *)fa.map)[0] == serial_last, "overlap: same frame count and last waveform frame as serial");
    EXPECT(nflips >= serial_flips, "overlap: waveform + lead + tail scans all delivered (extra idle scans allowed)");
    on_err = 1;
    EXPECT(frame() != 0 && panel_unknown, "overlap: rail-on failure -> ERR, worker joined, panel unknown");
    on_err = 0;
    EXPECT(frame() == 0 && !panel_unknown && !rails_on, "overlap: recovery clear + frame OK");
    overlap_gen = 0;

    /* eink-round3: no system suspend while the (leased) e-ink CRTC is enabled: the lock follows the CRTC, not the update */
    crtc = 7; started = 0; n_modeset = 0; crtc_wl_held = 0;
    EXPECT(frame() == 0 && started && n_modeset == 1 && crtc_wl_held, "crtc lock: taken with the modeset of a cold CRTC");
    EXPECT(frame() == 0 && n_modeset == 1 && crtc_wl_held, "crtc lock: kept (refreshed) while the CRTC stays on between updates");
    EXPECT(exec_cmd("power off", NULL, reply, sizeof reply) == 0 && !started && !crtc_wl_held, "crtc lock: released by power off");
    modeset_err = 1;
    EXPECT(frame() != 0 && !started && !crtc_wl_held, "crtc lock: released when the modeset fails");
    modeset_err = 0;
    EXPECT(frame() == 0 && started && crtc_wl_held, "crtc lock: recovery frame takes it again");
    idle_off_s = 1; last_update_t = now() - 5;
    if (idle_off_s > 0 && started && last_update_t && now() - last_update_t > idle_off_s) crtc_off("idle");	/* = serve() */
    EXPECT(!started && !crtc_wl_held, "crtc lock: released by idle-off");
    idle_off_s = 0;
    EXPECT(frame() == 0 && crtc_wl_held, "crtc lock: held again before the lessee fd is dropped");
    { void *a = fa.map, *b = fb2.map; fa.map = fb2.map = NULL; drm_close(); dfd = 100; fa.map = a; fb2.map = b; }
    EXPECT(!crtc_wl_held && !started, "crtc lock: released when the lessee fd is dropped (drm_close)");
    crtc_wakelock_on = 0; crtc = 7;
    EXPECT(frame() == 0 && started && !crtc_wl_held, "crtc lock: --crtc-wakelock 0 keeps the old behaviour");
    crtc_wakelock_on = 1;

    /* eink-round4: a system suspend in the middle of the drive (6 Oct 17:01:56) -> ERR, picture unknown, next frame recovers */
    nflips = 0; suspend_at_flip = 6; u = updates;
    EXPECT(frame() != 0 && !strncmp(reply, "ERR", 3) && panel_unknown, "suspend during the drive -> ERR, panel state unknown");
    EXPECT(frame() == 0 && !panel_unknown && updates == u + 4, "  ... next frame: recovery clear (white + INIT) then the picture");
    /* eink-round4: kernel wakelock writes are checked and reported (EPERM without CAP_BLOCK_SUSPEND was silent) */
    { FILE *f = fopen("t_wake_lock", "w"); if (f) fclose(f); f = fopen("t_wake_unlock", "w"); if (f) fclose(f); }
    use_wakelock = 1; wl_lock_path = "t_wake_lock"; wl_unlock_path = "t_wake_unlock";
    EXPECT(frame() == 0 && wl_err == 0, "wakelock: lock write accepted");
    exec_cmd("status", NULL, reply, sizeof reply);
    EXPECT(strstr(reply, "kernel_wakelock=ok") != NULL, "status reports kernel_wakelock=ok");
    wl_lock_path = "/nonexistent-a6l/wake_lock";
    EXPECT(frame() == 0 && wl_err == ENOENT, "wakelock: failed lock write recorded (update still driven)");
    exec_cmd("status", NULL, reply, sizeof reply);
    EXPECT(strstr(reply, "kernel_wakelock=No such file") != NULL, "status reports the wakelock failure");
    wl_lock_path = "t_wake_lock";
    EXPECT(frame() == 0 && wl_err == 0, "wakelock: recovers when the write works again");
    use_wakelock = 0;

    /* eink-round4: the table-dither / direct-pack input path is byte-identical to img_from_grey() */
    {
        static uint8_t in[IW * IH]; static int16_t g16[IW * IH]; uint8_t *ref = malloc(RGBA); int same = 1;
        unsigned s = 7; for (int i = 0; i < IW * IH; i++) { s = s * 1103515245u + 12345u; in[i] = (uint8_t)(s >> 16); }
        int saved_dither = dither, saved_rot = rot180;
        for (int d = 0; d <= 2; d += 2) for (int r = 0; r < 2; r++) for (int o = 0; o < 2; o++) {
            int w = o ? IW : IH, h = o ? IH : IW;
            dither = d; rot180 = r;
            for (int i = 0; i < w * h; i++) g16[i] = in[i];
            img_from_grey(g16, w, h); memcpy(ref, img.data, RGBA);
            memset(img.data, 0, RGBA); img_from_u8(in, w, h);
            if (memcmp(ref, img.data, RGBA)) { same = 0; printf("  mismatch dither=%d rot180=%d %dx%d\n", d, r, w, h); }
        }
        dither = saved_dither; rot180 = saved_rot; free(ref);
        EXPECT(same, "fast input path == img_from_grey (ordered/none, rot180 0/1, portrait/landscape)");
    }

    /* eink-round4 --chain: reply after the waveform, rails held for the tail time, a frame arriving meanwhile starts warm */
    chain = 1; started = 1; crtc_wakelock_on = 0; n_on = 0; n_off = 0; nflips = 0; u = updates;
    EXPECT(frame() == 0 && !strncmp(reply, "OK", 2) && rails_held && rails_on && n_on == 1 && n_off == 0, "chain: OK reply with the rails still up (held)");
    exec_cmd("status", NULL, reply, sizeof reply);
    EXPECT(strstr(reply, "rails=held") != NULL, "chain: status reports rails=held");
    int flips_cold = nflips; nflips = 0;
    EXPECT(frame() == 0 && n_on == 1 && n_off == 0 && rails_held && nflips == flips_cold - lead, "chain: next frame within the tail time: no rails cycle, no lead scans");
    rails_hold_until = now() - 1;
    if (rails_held && now() >= rails_hold_until) rails_release("tail: no chained update");	/* = serve() */
    EXPECT(!rails_held && !rails_on && n_off == 1, "chain: rails switched off when no frame came within the tail time");
    EXPECT(frame() == 0 && n_on == 2 && rails_held, "chain: after the tail a frame is cold again (rails on + lead)");
    EXPECT(exec_cmd("power off", NULL, reply, sizeof reply) == 0 && !rails_held && !rails_on && !started, "chain: power off switches the held rails off first");
    started = 1;
    EXPECT(frame() == 0 && rails_held, "chain: held again");
    nflips = 0; flip_fail_at = 3;
    EXPECT(frame() != 0 && !rails_held && !rails_on && panel_unknown, "chain: a failed warm drive switches the rails off, panel unknown");
    flip_fail_at = -1;
    EXPECT(frame() == 0 && !panel_unknown, "chain: recovery clear + frame OK");
    off_err_count = 2; rails_hold_until = now() - 1;
    rails_release("test");
    EXPECT(rails_off_fail >= 2 && panel_unknown && !rails_held, "chain: a failed delayed rails-off is counted and makes the panel unknown");
    off_err_count = 0;
    EXPECT(frame() == 0 && !panel_unknown, "chain: ... and the next frame recovers");
    overlap_gen = 1; n_on = 0; rails_release("test");
    EXPECT(frame() == 0 && rails_held && n_on == 1, "chain + overlap-gen (rc defaults): cold frame OK, rails held");
    EXPECT(frame() == 0 && rails_held && n_on == 1 && !panel_unknown, "chain + overlap-gen: chained frame OK without a rails cycle");
    overlap_gen = 0;
    rails_release("test end"); chain = 0; crtc_wakelock_on = 1;

    printf("A6L_EPDD_DRIVE_TEST %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
