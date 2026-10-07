// SPDX-License-Identifier: Apache-2.0
/* eink-lockscreen host test: a6l_epdd's "lockframe" / "lock restore" (real exec_cmd()/run_update()/drive() with the fake
 * rails, page flips and TCON of test_epdd_drive.c). Checks: a lock picture is REGAL (mode 3) unless asked otherwise, keeps
 * the picture it covers (only the first one of a lock), switches the e-ink CRTC off and releases its no-suspend lock after
 * every lock frame; "lock restore" redraws the covered picture with its own mode only while the lock picture is still
 * on the panel (any later frame/clear cancels it), "off" switches the CRTC off again; failures leave the panel unknown and
 * the restore goes through the usual recovery clear. */
#define A6L_EPDD_HOSTTEST 1
#define main a6l_epdd_main
#include "../src/a6l_epdd.c"
#undef main

void *Init_Eink_SWTcon(struct buf *ring, int n, uint32_t *cfg, void *flash, uint32_t size, void *info);
int ModeDecision_MirrorMode(struct buf *img, void *handle, int t1, int t2, int force, int mode);
uint8_t Update_Display_Image(struct buf *b, void *handle);

static int on_err, rails_on, n_modeset;
static int a6l_test_power(int on) { if (on) { if (on_err) return -1; rails_on = 1; return 0; } rails_on = 0; return 0; }
static int a6l_test_modeset(void) { n_modeset++; return 0; }
static int a6l_test_flip(uint32_t id) { (void)id; return 0; }

static int fails;
#define EXPECT(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)
static uint8_t px[IW * IH];
static char reply[512];
extern int fake_tcon_last_force, fake_tcon_last_mode;
static int cmd(const char *line, int v) { memset(px, v, sizeof px); return exec_cmd(line, px, reply, sizeof reply); }

int main(void) {
    tc_init = Init_Eink_SWTcon; tc_decide = ModeDecision_MirrorMode; tc_update = Update_Display_Image;
    for (int i = 0; i < RING; i++) { ring[i].data = calloc(1, FRAME); ring[i].size = FRAME; }
    img.data = calloc(1, RGBA); img.size = RGBA; last_img = malloc(RGBA);
    static uint8_t flash[FLASH]; memset(flash, 0x5a, sizeof flash);
    if (waveform_valid(flash)) { printf("FAIL fake tcon\n"); return 1; }
    fa.map = malloc(FRAME); fb2.map = malloc(FRAME); dfd = 100; lead = 2; tail = 2; use_wakelock = 0;
    img_fill(0xff); run_update(1, 0, "clear"); img_fill(0xff); run_update(0, mode, "reset-to-white");
    crtc = 7; started = 0; crtc_wl_held = 0;

    /* lock frame before any picture: nothing to keep */
    int u = updates;
    EXPECT(cmd("lockframe 720 1440 reading", 50) == 0 && updates == u + 1 && lock_on_panel && !have_mirror, "first lock frame with no earlier picture: shown, nothing kept");
    EXPECT(exec_cmd("lock restore", NULL, reply, sizeof reply) == 0 && strstr(reply, "no earlier picture") && !lock_on_panel && updates == u + 1,
           "restore without an earlier picture: lock picture kept, no update");

    /* the mirror's page, then a lock */
    EXPECT(cmd("frame 720 1440 quality", 128) == 0 && started && crtc_wl_held, "mirror page shown (CRTC on, no-suspend lock held)");
    static uint8_t page[RGBA]; memcpy(page, last_img, RGBA);
    u = updates;
    EXPECT(cmd("lockframe 720 1440 reading", 40) == 0 && updates == u + 1 && !strncmp(reply, "OK lock picture", 15), "lock frame: OK, one update");
    EXPECT(fake_tcon_last_mode == 3 && fake_tcon_last_force == 0, "lock frame: REGAL (mode 3), not forced");
    EXPECT(!started && !crtc_wl_held, "lock frame: e-ink CRTC off and its no-suspend lock released right after");
    EXPECT(have_mirror && mirror_mode == 2 && !memcmp(mirror_img, page, RGBA), "lock frame: the covered page (and its mode) kept");
    EXPECT(cmd("lockframe 720 1440 reading", 41) == 0 && !memcmp(mirror_img, page, RGBA), "next minute: the kept page is not overwritten by a lock picture");
    EXPECT(cmd("lockframe 720 1440 reading force", 42) == 0 && fake_tcon_last_force == 1 && fake_tcon_last_mode == 3 && !started, "ghost cleanup: forced REGAL, CRTC off");
    EXPECT(cmd("lockframe 720 1440 force", 43) == 0 && fake_tcon_last_force == 1 && fake_tcon_last_mode == 3, "\"lockframe W H force\" = forced REGAL");
    EXPECT(cmd("lockframe 720 1440", 44) == 0 && fake_tcon_last_mode == 3 && fake_tcon_last_force == 0, "default mode = REGAL");
    EXPECT(cmd("lockframe 720 1440 reading bogus", 45) != 0 && !strncmp(reply, "ERR", 3), "unknown lockframe flag rejected");
    EXPECT(exec_cmd("lockframe 720 1440 reading", NULL, reply, sizeof reply) != 0, "lockframe without payload rejected");
    exec_cmd("status", NULL, reply, sizeof reply);
    EXPECT(strstr(reply, "lock=on") != NULL && strstr(reply, "crtc=off") != NULL, "status reports lock=on, crtc=off");

    u = updates;
    EXPECT(exec_cmd("lock restore", NULL, reply, sizeof reply) == 0 && updates == u + 1 && strstr(reply, "redrawn"), "restore: the covered page redrawn (one update)");
    EXPECT(!memcmp(last_img, page, RGBA) && last_mode == 2 && fake_tcon_last_mode == 2 && fake_tcon_last_force == 0, "restore: same raster and mode as the mirror's page, not forced");
    EXPECT(started && !lock_on_panel, "restore: CRTC stays on for the mirror (awake on the e-ink)");
    u = updates;
    EXPECT(exec_cmd("lock restore", NULL, reply, sizeof reply) == 0 && updates == u && strstr(reply, "nothing"), "second restore: nothing to do");

    /* the mirror replaced the lock picture first (e.g. keyguard after wake-up): no restore */
    cmd("lockframe 720 1440 reading", 60);
    EXPECT(cmd("frame 720 1440 reading", 200) == 0 && !lock_on_panel, "a mirror frame replaces the lock picture");
    u = updates;
    EXPECT(exec_cmd("lock restore", NULL, reply, sizeof reply) == 0 && updates == u, "restore after a mirror frame: no update (the mirror's newer page stays)");
    cmd("lockframe 720 1440 reading", 61);
    EXPECT(exec_cmd("clear", NULL, reply, sizeof reply) == 0 && !lock_on_panel, "a clear replaces the lock picture too");
    memcpy(page, last_img, RGBA);	/* white panel after the clear; the next lock keeps it */

    /* eink-round6: LCD display pipeline released (composer teardown) -> no cold e-ink modeset, nothing changed */
    {
        FILE *g = fopen("t_lcd_enabled", "w"); if (g) { fputs("disabled\n", g); fclose(g); }
        lcd_guard = "t_lcd_enabled";
        cmd("frame 720 1440 quality", 91); exec_cmd("power off", NULL, reply, sizeof reply);
        static uint8_t keep[RGBA]; memcpy(keep, last_img, RGBA); int u0 = updates, m0 = n_modeset, lf0 = lock_frames;
        EXPECT(cmd("lockframe 720 1440 reading", 71) != 0 && !strncmp(reply, "ERR lcd-released", 16) && updates == u0 && n_modeset == m0,
               "LCD released: lock frame refused, no modeset, no update");
        EXPECT(!lock_on_panel && lock_frames == lf0 && !memcmp(last_img, keep, RGBA) && !panel_unknown, "LCD released: picture, lock state and panel state untouched");
        EXPECT(cmd("frame 720 1440 quality", 92) != 0 && updates == u0 && n_modeset == m0, "LCD released: a cold mirror frame is refused too");
        exec_cmd("status", NULL, reply, sizeof reply);
        EXPECT(strstr(reply, "lcd_guard=released") && strstr(reply, "guard_refusals=2"), "status reports lcd_guard=released, guard_refusals=2");
        g = fopen("t_lcd_enabled", "w"); if (g) { fputs("enabled\n", g); fclose(g); }
        EXPECT(cmd("lockframe 720 1440 reading", 72) == 0 && lock_on_panel && updates == u0 + 1, "LCD pipeline kept: lock frame shown");
        g = fopen("t_lcd_enabled", "w"); if (g) { fputs("disabled\n", g); fclose(g); }
        u0 = updates;
        EXPECT(exec_cmd("lock restore off", NULL, reply, sizeof reply) != 0 && lock_on_panel && updates == u0 && !started, "LCD released: restore refused, lock picture kept (retried later), CRTC off");
        started = 1;	/* a running e-ink CRTC owns its CTL: no check */
        EXPECT(cmd("frame 720 1440 quality", 93) == 0, "LCD released but e-ink CRTC running: frame driven");
        exec_cmd("power off", NULL, reply, sizeof reply); lock_on_panel = 0;
        lcd_guard = "none"; unlink("t_lcd_enabled");
    }
    /* eink-round6d --lock-clean: the first page after a lock picture is GC16 (mode 2, forced), the next ones normal */
    {
        lock_clean = 1;
        cmd("frame 720 1440 quality", 95); cmd("lockframe 720 1440 reading", 73);
        EXPECT(cmd("frame 720 1440 reading", 96) == 0 && fake_tcon_last_force == 1 && fake_tcon_last_mode == 2 && !lock_on_panel,
               "lock-clean: first page after a lock picture = GC16 clean (forced, mode 2)");
        EXPECT(cmd("frame 720 1440 reading", 97) == 0 && fake_tcon_last_force == 0 && fake_tcon_last_mode == 3, "lock-clean: the following page is a normal REGAL update");
        cmd("frame 720 1440 quality", 98); cmd("lockframe 720 1440 reading", 74);
        EXPECT(exec_cmd("lock restore", NULL, reply, sizeof reply) == 0 && fake_tcon_last_force == 1 && fake_tcon_last_mode == 2, "lock-clean: restore of the covered page = GC16 clean");
        lock_clean = 0;
        cmd("lockframe 720 1440 reading", 75);
        EXPECT(cmd("frame 720 1440 reading", 99) == 0 && fake_tcon_last_force == 0 && fake_tcon_last_mode == 3, "--lock-clean 0: first page after a lock picture stays REGAL (round 6)");
        exec_cmd("power off", NULL, reply, sizeof reply);
    }
    /* restore off: woke up on the LCD */
    cmd("frame 720 1440 quality", 90); memcpy(page, last_img, RGBA);
    cmd("lockframe 720 1440 reading", 70);
    EXPECT(exec_cmd("lock restore off", NULL, reply, sizeof reply) == 0 && !memcmp(last_img, page, RGBA) && !started && !crtc_wl_held,
           "restore off: page redrawn, CRTC switched off again");
    cmd("frame 720 1440 quality", 91); memcpy(page, last_img, RGBA);
    cmd("lockframe 720 1440 reading", 71);
    EXPECT(exec_cmd("refresh", NULL, reply, sizeof reply) == 0 && lock_on_panel, "refresh redraws the lock picture and keeps the lock state");

    /* failures */
    on_err = 1; u = updates;
    EXPECT(cmd("lockframe 720 1440 reading", 72) != 0 && !strncmp(reply, "ERR", 3) && panel_unknown && !started && !crtc_wl_held,
           "rail failure during a lock frame: ERR, panel unknown, CRTC off");
    on_err = 0; u = updates;
    EXPECT(exec_cmd("lock restore", NULL, reply, sizeof reply) == 0 && !panel_unknown && updates == u + 3 && !memcmp(last_img, page, RGBA),
           "restore after the failure: recovery clear (white + INIT) then the covered page");

    /* eink-round11: "warm" = CRTC bring-up without a picture; the next update skips the cold modeset */
    {
        lock_clean = 1;
        cmd("frame 720 1440 quality", 120); memcpy(page, last_img, RGBA);
        cmd("lockframe 720 1440 reading", 76);
        int m0 = n_modeset, u0 = updates;
        EXPECT(!started && exec_cmd("warm", NULL, reply, sizeof reply) == 0 && !strncmp(reply, "OK warm: e-ink CRTC on", 22) && started && crtc_wl_held && n_modeset == m0 + 1 && updates == u0,
               "warm: CRTC brought up (one modeset), no update, no-suspend lock held");
        EXPECT(lock_on_panel && !memcmp(mirror_img, page, RGBA), "warm: the lock state and the covered page are kept");
        EXPECT(exec_cmd("warm", NULL, reply, sizeof reply) == 0 && strstr(reply, "already on") && n_modeset == m0 + 1, "warm again: nothing (already on)");
        EXPECT(cmd("frame 720 1440 reading", 121) == 0 && n_modeset == m0 + 1 && updates == u0 + 1 && fake_tcon_last_mode == 2,
               "first frame after the warm-up: no second modeset; still the GC16 clean after a lock picture");
        exec_cmd("power off", NULL, reply, sizeof reply);
        FILE *g = fopen("/tmp/a6l_epdd_lock_test_guard", "w"); if (g) { fputs("disabled\n", g); fclose(g); }
        lcd_guard = "/tmp/a6l_epdd_lock_test_guard"; m0 = n_modeset;
        EXPECT(exec_cmd("warm", NULL, reply, sizeof reply) != 0 && strstr(reply, "lcd-released") && !started && n_modeset == m0,
               "warm while the LCD pipeline is released: refused, no modeset");
        lcd_guard = "none"; unlink("/tmp/a6l_epdd_lock_test_guard");
    }

    printf("A6L_EPDD_LOCK_TEST %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
