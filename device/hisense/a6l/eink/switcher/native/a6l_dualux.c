// SPDX-License-Identifier: Apache-2.0
/* a6l_dualux — Hisense A6L LCD <-> rear e-ink switcher (vendor daemon; agent dualux, 25 Sep 2026; docs/dualux-20260925.md)
 *
 * Owns "which screen is the phone's screen". Framework-free: evdev + sysfs + properties only.
 *   inputs : e-ink key (evdev KEY 616, device "A6L side keys", read only, never grabbed — volume-up lives there too)
 *            power key ("pm8941_pwrkey", GRABBED only while the e-ink is the active screen and Android is awake)
 *            front touchscreen (the MT device with x range >= 1000 not created by us/the mirror; GRABBED = dropped in e-ink)
 *            LCD on/off = dpms of the DRM connector that has the 1080x2340 mode (atomic commits keep it up to date)
 *            sys.a6l.dualux.req "<seq> <eink|lcd|toggle|clear>" (app / Quick Settings), persist.sys.a6l.dualux.* settings
 *   outputs: persist.vendor.eink.mode = mirror|off (vendor.a6l_eink draws the UI on the e-ink + forwards rear touches)
 *            LCD backlight bl_power = 4 in e-ink mode (the composer keeps writing `brightness`, which the backlight core
 *              stores but does not apply while blanked — and which we read back as "what the brightness slider says")
 *            e-ink frontlight = the slider value (HLG-decoded, see dualux_logic.c) while the e-ink is active, else 0
 *            vendor.eink.clear_req = counter (the mirror clears + redraws), vendor.dualux.state = lcd|eink|eink-asleep
 *            uinput keyboard "a6l-dualux-keys": KEY_WAKEUP / KEY_SLEEP / KEY_POWER (re-injected long press)
 *
 * usage: a6l_dualux [--sysroot DIR] [--prop-dir DIR] [--key-dev P|auto|none] [--power-dev P|auto|none]
 *                   [--front-dev P|auto|none] [--no-uinput] [--awake-file P] [--lcd-bl NAME] [--fl PATH] [--exit-after S]
 *   --sysroot / --prop-dir / FIFOs: host tests only (tests/run-tests.sh). Properties then live as files in --prop-dir.
 *   --fake-uinput P: host tests only: the "uinput keyboard" is the file P (raw input_events appended, no ioctls).
 *   --watchdog-s N: host tests only: main-loop watchdog period (default 10 s; eink-round3 0016: suspend-aware).
 *   eink-round3 0016: after an unexpected death on the e-ink (vendor.dualux.state still eink*) the restarted daemon comes
 *   back on the e-ink, at most once per 60 s (vendor.dualux.restored_at); persist.sys.a6l.dualux.restore=0 disables it.
 *
 * r5 review pass2 F20 (28 Sep 2026) — the physical power key is FAIL-OPEN (Android keeps a working power key whenever
 * this daemon cannot deliver its replacement events):
 *   - the power key is grabbed only while the uinput keyboard exists (dx_state.no_inject); uinput creation is retried
 *     every 5 s; a failed uinput write destroys it, releases the grab and cancels the held press;
 *   - the grab actually held (EVIOCGRAB result) decides whether a press is ours (dx_state.pw_not_held);
 *   - SYN_DROPPED: events are discarded up to the next SYN_REPORT, then the key state is re-read with EVIOCGKEY
 *     (unreadable = up) and a press we no longer see is cancelled (an injected KEY_POWER down gets its up);
 *   - input device lost: its press is cancelled the same way;
 *   - daemon death: the kernel drops EVIOCGRAB and destroys the uinput device when the fds close; init restarts the
 *     service, which boots on the LCD with nothing grabbed. A stalled main loop (> WATCHDOG_S) is killed by SIGALRM.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "dualux_logic.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef A6L_ANDROID_LOG
#include <android/log.h>
#endif

#define KEY_EINK 616
static const char *fake_uinput;
#define WATCHDOG_S 10	/* r5 pass2 F20: main loop stalled this long (holding the power key grab) -> SIGALRM kills us */
static const char *sysroot = "", *prop_dir, *key_spec = "auto", *power_spec = "auto", *front_spec = "auto", *awake_file, *lcd_bl_name, *fl_spec;
static int use_uinput = 1; static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }
#ifdef A6L_ANDROID_LOG
/* Match Java elapsedRealtime across suspend; scheduling still uses now(). */
static double boot_now(void) { struct timespec ts; if (clock_gettime(CLOCK_BOOTTIME, &ts)) return now(); return ts.tv_sec + ts.tv_nsec / 1e9; }
#endif

static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("A6L_DUALUX %s\n", b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    char stamped[sizeof b + 128];
    snprintf(stamped, sizeof stamped, "boot_ms=%.3f mono_ms=%.3f %s", boot_now() * 1000, now() * 1000, b);
    __android_log_write(strncmp(b, "FAIL", 4) ? strncmp(b, "WARN", 4) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN : ANDROID_LOG_ERROR, "a6l_dualux", stamped);
#endif
}
#define LOG(...) logline(__VA_ARGS__)
/* eink-round5: display-transition markers in the kernel log ("<6>a6l_dualux: ..."; printk.devkmsg=on). eink-round6: /dev/kmsg is
 * 0600 root:root (first-stage init), so a system daemon cannot open it: /dev/kmsg_debug (0622, userdebug/eng) instead.
 * One timeline with the DPU/DSI/SMMU/PM messages for the round-5 repro (kmsg streamed to the laptop / fsync'ed on the
 * phone). Android builds only: host tests never write the host's kernel log. */
static void kmark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void kmark(const char *fmt, ...) {
#ifdef __ANDROID__
    static int fd = -2;
    if (fd == -2) { fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC); if (fd < 0) fd = open("/dev/kmsg_debug", O_WRONLY | O_CLOEXEC); }
    if (fd < 0) return;
    char b[300]; int n = snprintf(b, sizeof b, "<6>a6l_dualux: "); va_list ap; va_start(ap, fmt);
    int m = vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap); va_end(ap);
    if (m < 0) return;
    n += m; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n'; if (write(fd, b, (size_t)n) < 0) { /* best effort */ }
#else
    (void)fmt;
#endif
}
#define KLOG(...) do { LOG(__VA_ARGS__); kmark(__VA_ARGS__); } while (0)

/* ---------------- properties (real, or files in --prop-dir for host tests) ---------------- */
static int prop_get(const char *k, char *v, size_t n) {
    v[0] = 0;
    if (prop_dir) { char p[512]; snprintf(p, sizeof p, "%s/%s", prop_dir, k); FILE *f = fopen(p, "r"); if (!f) return 0;
        if (!fgets(v, (int)n, f)) v[0] = 0; fclose(f); v[strcspn(v, "\n")] = 0; return (int)strlen(v); }
#ifdef __ANDROID__
    char b[PROP_VALUE_MAX] = {0}; int l = __system_property_get(k, b); snprintf(v, n, "%s", b); return l;
#else
    return 0;
#endif
}
static void prop_set(const char *k, const char *v) {
    char cur[128]; if (prop_get(k, cur, sizeof cur) >= 0 && !strcmp(cur, v)) return;
    if (prop_dir) { char p[512], t[520]; snprintf(p, sizeof p, "%s/%s", prop_dir, k); snprintf(t, sizeof t, "%s.tmp", p);
        FILE *f = fopen(t, "w"); if (f) { fprintf(f, "%s\n", v); fclose(f); rename(t, p); } return; }
#ifdef __ANDROID__
    if (__system_property_set(k, v)) LOG("WARN setprop %s=%s refused (SELinux?)", k, v);
#endif
}
static int prop_int(const char *k, int def) { char v[96]; return prop_get(k, v, sizeof v) > 0 ? atoi(v) : def; }
static double prop_double(const char *k, double def) { char v[96]; return prop_get(k, v, sizeof v) > 0 ? atof(v) : def; }

/* ---------------- sysfs ---------------- */
static int rd_str(const char *path, char *v, size_t n) {
    char p[600]; snprintf(p, sizeof p, "%s%s", sysroot, path); int fd = open(p, O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
    ssize_t r = read(fd, v, n - 1); close(fd); if (r < 0) return -1; v[r] = 0; v[strcspn(v, "\n")] = 0; return (int)r;
}
static int rd_int(const char *path, int def) { char v[64]; return rd_str(path, v, sizeof v) > 0 ? atoi(v) : def; }
static int wr_int(const char *path, int val) {
    char p[600], b[32]; snprintf(p, sizeof p, "%s%s", sysroot, path); int fd = open(p, O_WRONLY | O_CLOEXEC | (*sysroot ? O_TRUNC : 0));
    if (fd < 0) return -1; int l = snprintf(b, sizeof b, "%d\n", val); int ok = write(fd, b, (size_t)l) == l; close(fd); return ok ? 0 : -1;
}

static char lcd_bl[640], fl_dir[640], lcd_dpms[640];
static int lcd_bl_max = 4095, lcd_bl_linear, fl_max = 255;
static void find_lcd_backlight(void) {	/* /sys/class/backlight/<first entry that is not our frontlight> */
    char d[300], v[64]; snprintf(d, sizeof d, "%s/sys/class/backlight", sysroot); DIR *dir = opendir(d); lcd_bl[0] = 0;
    if (dir) { struct dirent *e; while ((e = readdir(dir))) { if (e->d_name[0] == '.') continue;
            if (lcd_bl_name && strcmp(e->d_name, lcd_bl_name)) continue; if (strstr(e->d_name, "frontlight") || strstr(e->d_name, "epd")) continue;
            if (!lcd_bl[0] || strcmp(e->d_name, lcd_bl + strlen("/sys/class/backlight/")) < 0) snprintf(lcd_bl, sizeof lcd_bl, "/sys/class/backlight/%s", e->d_name); }
        closedir(dir); }
    if (!lcd_bl[0]) { static int w; if (!w++) LOG("WARN no LCD backlight in /sys/class/backlight"); return; }
    char p[700]; snprintf(p, sizeof p, "%s/max_brightness", lcd_bl); lcd_bl_max = rd_int(p, 4095);
    snprintf(p, sizeof p, "%s/scale", lcd_bl); lcd_bl_linear = rd_str(p, v, sizeof v) > 0 && !strcmp(v, "linear");
    LOG("LCD backlight %s max=%d scale=%s", lcd_bl, lcd_bl_max, lcd_bl_linear ? "linear (hw encodes)" : "non-linear (composer applies HLG)");
}
static void find_frontlight(void) {	/* stock name: LED class "epd-backlight"; the pwm-backlight candidate as fallback */
    const char *c[] = {fl_spec, "/sys/class/leds/epd-backlight", "/sys/class/backlight/a6l-eink-frontlight", NULL}; char p[300]; fl_dir[0] = 0;
    for (int i = 0; i < 3; i++) { if (!c[i]) continue; snprintf(p, sizeof p, "%s/max_brightness", c[i]); int m = rd_int(p, -1);
        if (m > 0) { snprintf(fl_dir, sizeof fl_dir, "%s", c[i]); fl_max = m; LOG("frontlight %s max=%d", fl_dir, fl_max); return; } }
    static int w; if (!w++) LOG("WARN no frontlight (/sys/class/leds/epd-backlight): brightness in e-ink mode does nothing (DT overlay a6l-eink-frontlight-v75)");
}
static void find_lcd_dpms(void) {	/* DRM connector whose mode list has 1080x2340 */
    char d[300]; snprintf(d, sizeof d, "%s/sys/class/drm", sysroot); DIR *dir = opendir(d); lcd_dpms[0] = 0; if (!dir) return;
    struct dirent *e; while ((e = readdir(dir))) { if (strncmp(e->d_name, "card", 4) || !strchr(e->d_name, '-')) continue;
        char p[600], m[256]; snprintf(p, sizeof p, "/sys/class/drm/%s/modes", e->d_name);
        if (rd_str(p, m, sizeof m) > 0 && strstr(m, "1080x2340")) { snprintf(lcd_dpms, sizeof lcd_dpms, "/sys/class/drm/%s/dpms", e->d_name); break; } }
    closedir(dir); if (lcd_dpms[0]) LOG("LCD on/off from %s", lcd_dpms); else { static int w; if (!w++) LOG("WARN no LCD connector with 1080x2340 in /sys/class/drm: using vendor.eink.state"); }
}
static int read_awake(void) {
    char v[64];
    if (awake_file) return rd_str(awake_file, v, sizeof v) > 0 && (!strcmp(v, "On") || !strcmp(v, "1"));
    if (lcd_dpms[0] && rd_str(lcd_dpms, v, sizeof v) > 0) return !strcmp(v, "On");
    return prop_get("vendor.eink.state", v, sizeof v) <= 0 || strcmp(v, "mirror-paused");	/* mirror pauses when the LCD CRTC is off */
}

/* ---------------- input ---------------- */
static int test_bit(const unsigned long *b, int n) { return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }
enum { DEV_KEY, DEV_POWER, DEV_FRONT, NDEV };
static const char *dev_what[NDEV] = {"e-ink key", "power key", "front touch"};
static int dev_fd[NDEV] = {-1, -1, -1}, dev_grabbed[NDEV]; static char dev_path[NDEV][640];
static int match_dev(int fd, int kind) {
    char name[128] = ""; ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    if (!strncmp(name, "a6l-", 4)) return 0;	/* our uinput + the mirror's rear-touch uinput */
    unsigned long kb[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = {0}, ab[(ABS_MAX + 1) / (8 * sizeof(long)) + 1] = {0};
    ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb); ioctl(fd, EVIOCGBIT(EV_ABS, sizeof ab), ab);
    if (kind == DEV_KEY) return test_bit(kb, KEY_EINK);
    if (kind == DEV_POWER) return !strcmp(name, "pm8941_pwrkey") || (test_bit(kb, KEY_POWER) && !test_bit(kb, KEY_VOLUMEUP) && !test_bit(ab, ABS_MT_POSITION_X));
    if (kind == DEV_FRONT && test_bit(ab, ABS_MT_POSITION_X)) { struct input_absinfo ai; return !ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ai) && ai.maximum >= 1000; }
    return 0;
}
static void open_dev(int kind, const char *spec) {
    if (!strcmp(spec, "none") || dev_fd[kind] >= 0) return;
    if (strcmp(spec, "auto")) {	/* explicit path (a FIFO in host tests) */
        int fd = open(spec, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) return;
        dev_fd[kind] = fd; snprintf(dev_path[kind], sizeof dev_path[kind], "%s", spec); LOG("%s: %s", dev_what[kind], spec); return; }
    char d[300]; snprintf(d, sizeof d, "%s/dev/input", sysroot); DIR *dir = opendir(d); if (!dir) return;
    struct dirent *e; while ((e = readdir(dir))) { if (strncmp(e->d_name, "event", 5)) continue;
        char p[600]; snprintf(p, sizeof p, "%s/%s", d, e->d_name); int fd = open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC); if (fd < 0) continue;
        if (match_dev(fd, kind)) { char name[128] = ""; ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
            dev_fd[kind] = fd; snprintf(dev_path[kind], sizeof dev_path[kind], "%s", p); LOG("%s: %s \"%s\"", dev_what[kind], p, name); break; }
        close(fd); }
    closedir(dir);
}
static void set_grab(int kind, int on) {
    if (dev_fd[kind] < 0 || dev_grabbed[kind] == on) return;
    struct stat st; int is_chr = !fstat(dev_fd[kind], &st) && S_ISCHR(st.st_mode);
    if (is_chr && ioctl(dev_fd[kind], EVIOCGRAB, on ? 1 : 0)) { LOG("WARN %s %s: %s", on ? "grab" : "ungrab", dev_what[kind], strerror(errno)); return; }
    dev_grabbed[kind] = on; LOG("%s %s", dev_what[kind], on ? "grabbed (Android does not see it)" : "released");
}

/* uinput keyboard for KEY_WAKEUP / KEY_SLEEP / KEY_POWER (a6l-dualux-keys.idc: internal) */
static int ui = -1;
static struct dx_state S;
static void apply_grabs(void);
static void uinput_open(void) {	/* r5 pass2 F20: S.no_inject follows the result; called again every 5 s while missing */
    static int warned;
    if (ui >= 0) return;
    S.no_inject = 1;
    if (!use_uinput) return;
    if (fake_uinput) { ui = open(fake_uinput, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (ui < 0) { if (!warned++) LOG("WARN fake uinput %s: %s (power key left to Android)", fake_uinput, strerror(errno)); return; }
        LOG("uinput a6l-dualux-keys created (fake %s)", fake_uinput); S.no_inject = 0; warned = 0; return; }
    ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC); if (ui < 0) { if (!warned++) LOG("WARN /dev/uinput: %s (no wake/sleep/power injection, power key left to Android; retrying)", strerror(errno)); return; }
    ioctl(ui, UI_SET_EVBIT, EV_KEY); ioctl(ui, UI_SET_EVBIT, EV_SYN);
    ioctl(ui, UI_SET_KEYBIT, KEY_POWER); ioctl(ui, UI_SET_KEYBIT, KEY_SLEEP); ioctl(ui, UI_SET_KEYBIT, KEY_WAKEUP);
    struct uinput_setup us; memset(&us, 0, sizeof us); us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x2a6c; us.id.product = 0x0d0a; us.id.version = 1;
    snprintf(us.name, sizeof us.name, "a6l-dualux-keys");
    if (ioctl(ui, UI_SET_EVBIT, EV_KEY) || ioctl(ui, UI_SET_KEYBIT, KEY_POWER) || ioctl(ui, UI_DEV_SETUP, &us) || ioctl(ui, UI_DEV_CREATE)) {
        if (!warned++) LOG("WARN uinput setup: %s (power key left to Android; retrying)", strerror(errno)); close(ui); ui = -1; return; }
    LOG("uinput a6l-dualux-keys created"); S.no_inject = 0; warned = 0;
}
static void uinput_lost(const char *why) {	/* r5 pass2 F20: fail open */
    LOG("WARN uinput %s: keyboard dropped, power key given back to Android", why);
    if (!fake_uinput) ioctl(ui, UI_DEV_DESTROY); close(ui); ui = -1; S.no_inject = 1;
    dx_power_cancel(&S); apply_grabs();
}
static void emit(int type, int code, int val) {
    if (ui < 0) return; struct input_event ev; memset(&ev, 0, sizeof ev); ev.type = (unsigned short)type; ev.code = (unsigned short)code; ev.value = val;
    if (write(ui, &ev, sizeof ev) != sizeof ev) { char w[96]; snprintf(w, sizeof w, "write: %s", strerror(errno)); uinput_lost(w); }
}
static void key_edge(int code, int val, const char *what) { LOG("inject %s %s", what, val ? "down" : "up"); emit(EV_KEY, code, val); emit(EV_SYN, SYN_REPORT, 0); }
static void key_tap(int code, const char *what) { LOG("stage synthetic %s start mono_ms=%.3f ui=%d", what, now() * 1000, ui >= 0); key_edge(code, 1, what); key_edge(code, 0, what); LOG("stage synthetic %s end mono_ms=%.3f", what, now() * 1000); }

/* ---------------- state application ---------------- */
static struct dx_fl_cfg FL; static unsigned clear_seq; static int last_fl = -1, blanked_by_us;
#define P_REQ "sys.a6l.dualux.req"
#define P_STATE "vendor.dualux.state"
#define P_PREPARE "vendor.dualux.prepare"
#define P_READY "sys.a6l.dualux.ready"
#define P_THEME_SYNC "sys.a6l.dualux.theme_sync"
static char appearance_req[96];
static double appearance_deadline;
static void publish(void) {
    { static char last[16]; const char *st = dx_state_name(&S);	/* eink-round5: state changes in the kernel log */
      if (strcmp(last, st)) { kmark("state %s -> %s", last[0] ? last : "-", st); snprintf(last, sizeof last, "%s", st); } }
    prop_set("vendor.dualux.lcd_blank", dx_lcd_blank(&S) ? "1" : "0");	/* read by the patched composer (0002 patch): no LCD flash at wake-up */
    prop_set(P_STATE, dx_state_name(&S)); prop_set("persist.vendor.eink.mode", dx_mirror_on(&S) ? "mirror" : "off");
    prop_set("vendor.dualux.awake", S.awake ? "1" : "0"); }	/* eink-round6: a6l_einklock waits for display transitions to settle (LCD mode too) */
static void enforce_backlight(void) {
    if (!lcd_bl[0]) return;
    char p[700]; snprintf(p, sizeof p, "%s/bl_power", lcd_bl); int pw = rd_int(p, -1);
    if (dx_lcd_blank(&S)) { if (pw != 4) { if (!wr_int(p, 4)) { if (!blanked_by_us) LOG("LCD backlight blanked (bl_power 4)"); else LOG("LCD backlight re-blanked (composer turned it on at wake-up)"); blanked_by_us = 1; } } }
    else if (blanked_by_us) {	/* back to the LCD: unblank only when Android is awake (asleep = the composer owns bl_power) */
        /* r5 review fix F50 (29 Sep 2026): the restore stays PENDING until it is known to be done - a failed or unverified
         * write (or an unreadable node) is retried on the next loop iteration (<= 250 ms) instead of being forgotten;
         * while Android is asleep it waits (never overrides a deliberate Android blank) and runs once Android is awake.
         * Logging is bounded (first failure, then every 64th); a node that stays unreadable is re-discovered. */
        static unsigned unblank_fail;
        if (!S.awake) return;
        if (pw < 0) {
            if (!(unblank_fail++ & 63)) { LOG("WARN LCD bl_power unreadable (%s): unblank pending, re-discovering the backlight", p); find_lcd_backlight(); }
            return; }
        if (pw != 4) { if (unblank_fail) LOG("LCD backlight unblanked (bl_power %d, after %u failed attempt(s))", pw, unblank_fail);
            blanked_by_us = 0; unblank_fail = 0; return; }	/* already on (earlier write, composer) */
        int wr = wr_int(p, 0), e = errno, rb = rd_int(p, -1);
        if (!wr && rb != 4) { LOG("LCD backlight unblanked (bl_power %d)", rb); LOG("stage LCD release mono_ms=%.3f awake=%d hold=%d", now() * 1000, S.awake, S.appearance_hold); blanked_by_us = 0; unblank_fail = 0; }
        else if (!(unblank_fail++ & 63)) LOG("WARN LCD unblank failed (%s, bl_power=%d): retrying", wr ? strerror(e) : "readback still 4", rb); }
}
static void enforce_frontlight(void) {
    if (!fl_dir[0]) return;
    char p[700]; int v = 0; if (lcd_bl[0]) { snprintf(p, sizeof p, "%s/brightness", lcd_bl); v = rd_int(p, 0); }
    int lvl = dx_frontlight_level(&S, &FL, v, lcd_bl_max, lcd_bl_linear, fl_max);
    if (lvl == last_fl) {
        /* r5 review fix F63 (28 Sep 2026): the cache hid a recreated backend (driver unbind/reprobe: the node comes back
         * with default-state "off" while the unchanged request stays cached as applied). At most once per second the
         * backend is re-checked: gone -> cache invalidated + re-discovered; max_brightness changed -> re-read and the
         * level recomputed; brightness readback != applied level -> the CURRENT desired level (0 when asleep / on the LCD)
         * is rewritten. Sysfs readback is software state, not optical proof. Logging is bounded. */
        static double next_check; static unsigned fl_recover; double t = now();
        if (t < next_check) return;
        next_check = t + 1.0;
        snprintf(p, sizeof p, "%s/max_brightness", fl_dir); int m = rd_int(p, -1);
        if (m <= 0) { if (!(fl_recover++ & 63)) LOG("WARN frontlight %s gone: re-discovering, level %d pending", fl_dir, lvl);
            last_fl = -1; find_frontlight(); return; }
        if (m != fl_max) { LOG("frontlight %s max_brightness %d -> %d: re-applying", fl_dir, fl_max, m); fl_max = m; last_fl = -1; next_check = 0; return; }
        snprintf(p, sizeof p, "%s/brightness", fl_dir); int rb = rd_int(p, -1);
        if (rb == lvl) { fl_recover = 0; return; }
        if (!(fl_recover++ & 63)) LOG("WARN frontlight readback %d != applied %d (backend recreated?): re-applying", rb, lvl);
        last_fl = -1;
    }
    snprintf(p, sizeof p, "%s/brightness", fl_dir);
    if (!wr_int(p, lvl)) { if (last_fl < 0 || (lvl == 0) != (last_fl == 0)) { LOG("frontlight %d/%d (LCD request %d/%d)", lvl, fl_max, v, lcd_bl_max); LOG("stage rear light level=%d mono_ms=%.3f awake=%d hold=%d", lvl, now() * 1000, S.awake, S.appearance_hold); } last_fl = lvl; }
    else { static int w; if (!w++) LOG("WARN frontlight write %s: %s", p, strerror(errno)); }
}
static void apply_grabs(void) { set_grab(DEV_POWER, dx_power_grabbed(&S)); set_grab(DEV_FRONT, dx_front_touch_grabbed(&S)); }
/* A sequence-tagged prepare keeps both lights off until the app has applied
 * the target configuration and WMS has committed its wallpaper transaction.
 * No sleep here: evdev, wake and fail-open power-key handling stay live. */
static void begin_appearance(void) {
    S.appearance_hold = prop_int(P_THEME_SYNC, 0) ? 1 : 0;
    if (!S.appearance_hold) { appearance_req[0] = 0; prop_set(P_PREPARE, ""); return; }
    static unsigned long long last_token;
    unsigned long long token = (unsigned long long)(now() * 1000000);
    if (token <= last_token) token = last_token + 1;
    last_token = token;
    snprintf(appearance_req, sizeof appearance_req, "%llu %s", token, S.screen == DX_EINK ? "eink" : "lcd");
    appearance_deadline = now() + 3.0;
    LOG("appearance %s: prepare begin, lights held", appearance_req);
}
static void finish_appearance(const char *why) {
    LOG("appearance %s: %s", appearance_req, why);
    S.appearance_hold = 0; appearance_req[0] = 0;
    publish(); enforce_backlight(); enforce_frontlight(); prop_set(P_PREPARE, "");
}
static void appearance_tick(void) {
    if (!S.appearance_hold) return;
    char ready[96];
    if (S.appearance_hold == 1 && prop_get(P_READY, ready, sizeof ready) > 0 && !strcmp(ready, appearance_req)) {
        if (S.screen == DX_LCD) { finish_appearance("configuration ready"); return; }
        S.appearance_hold = 2; appearance_deadline = now() + 8.0;
        publish(); LOG("appearance %s: prepared, waiting for first rear frame", appearance_req);
    }
    if (S.appearance_hold == 2 && prop_get("vendor.eink.ready", ready, sizeof ready) > 0 && !strcmp(ready, appearance_req)) {
        finish_appearance("first rear frame ready"); return;
    }
    if (now() >= appearance_deadline) finish_appearance("WARN readiness timeout, fail open");
}
static void handle(const struct dx_out *o, const char *why) {
    if (o->set_screen >= 0 || o->wake || o->sleep) LOG("stage decision %s mono_ms=%.3f old=%s target=%d awake=%d wake=%d sleep=%d", why, now() * 1000, dx_state_name(&S), o->set_screen, S.awake, o->wake, o->sleep);
    if (o->set_screen >= 0 && o->set_screen != S.screen) KLOG("%s: %s -> %s", why, S.screen == DX_EINK ? "e-ink" : "LCD", o->set_screen == DX_EINK ? "e-ink" : "LCD");
    int old = S.screen; dx_apply(&S, o);
    /* eink-round6f: prepare BEFORE the new state. The Dualux app restores the Material contrast level when it reads
     * state=lcd with no prepare; with the state published first (and the backlight, grabs and frontlight written in
     * between) its 500 ms bookkeeping could fall into that window, write contrast_level at the very start of the switch,
     * and SystemUI's overlay regeneration (CONFIG_ASSETS_PATHS, a second relaunch) made WM miss the 1.5 s themed-redraw
     * deadline: 3 s fail-open (7 Oct 17:25:49, 17:26:25; round 7i 14:18:59). */
    if (S.screen != old) { begin_appearance(); prop_set(P_PREPARE, appearance_req); publish(); enforce_backlight(); apply_grabs(); enforce_frontlight(); }
    if (o->clear) { char b[16]; snprintf(b, sizeof b, "%u", ++clear_seq); prop_set("vendor.eink.clear_req", b); LOG("%s: e-ink clear #%u", why, clear_seq); }
    if (o->wake) key_tap(KEY_WAKEUP, "WAKEUP");
    if (o->sleep) key_tap(KEY_SLEEP, "SLEEP");
    if (o->power_down) key_edge(KEY_POWER, 1, "POWER (long press handed to Android)");
    if (o->power_up) key_edge(KEY_POWER, 0, "POWER");
}
static void read_cfg(void) {
    char v[96];
    S.cfg.long_ms = prop_int("persist.sys.a6l.dualux.long_ms", 800); S.cfg.power_long_ms = prop_int("persist.sys.a6l.dualux.power_long_ms", 450);
    S.cfg.ekey_in_eink = prop_get("persist.sys.a6l.dualux.eink_key", v, sizeof v) > 0 && !strcmp(v, "clear") ? DX_EK_CLEAR : DX_EK_SLEEP;
    int m = prop_int("persist.sys.a6l.dualux.mirror_in_lcd", 0) != 0; if (m != S.cfg.mirror_in_lcd) { S.cfg.mirror_in_lcd = m; publish(); }
    FL.enable = prop_int("persist.sys.a6l.dualux.fl_enable", 1) != 0; FL.max_pct = prop_int("persist.sys.a6l.dualux.fl_max_pct", 100);
    FL.min_level = prop_int("persist.sys.a6l.dualux.fl_min", 1); FL.gamma = prop_double("persist.sys.a6l.dualux.fl_gamma", 1.0);
}
/* r5 pass2 F20: the key state of `kind` is unknown (overrun) or gone: reconcile with the device (fd < 0 = gone = up) */
static int syn_dropped[NDEV];
static void resync(int kind, const char *why) {
    unsigned long kb[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = {0};
    int fd = dev_fd[kind], ok = fd >= 0 && ioctl(fd, EVIOCGKEY(sizeof kb), kb) >= 0;	/* unreadable = released (fail open) */
    if (kind == DEV_POWER) { int down = ok && test_bit(kb, KEY_POWER);
        if (S.pw_down && !down) { LOG("%s: power key press lost, cancelled", why); struct dx_out o = dx_power_cancel(&S); handle(&o, why); }
        else if (!S.pw_down && down) { S.pw_not_held = !dev_grabbed[DEV_POWER]; struct dx_out o = dx_power_key(&S, 1, now()); handle(&o, why); } }
    if (kind == DEV_KEY) { int down = ok && test_bit(kb, KEY_EINK);
        if (S.ek_down && !down) { LOG("%s: e-ink key press lost, cancelled", why); struct dx_out o = dx_eink_cancel(&S); handle(&o, why); }
        else if (!S.ek_down && down) { struct dx_out o = dx_eink_key(&S, 1, now()); handle(&o, why); } }
}
static void drain(int kind) {
    struct input_event ev[64]; ssize_t r = read(dev_fd[kind], ev, sizeof ev);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) {	/* device gone (or FIFO writer closed in host tests): close, rescanned every 5 s */
        if (r < 0) LOG("WARN %s read: %s (reopening)", dev_what[kind], strerror(errno));
        close(dev_fd[kind]); dev_fd[kind] = -1; dev_grabbed[kind] = 0; syn_dropped[kind] = 0;
        resync(kind, "device lost"); return; }
    for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++) {
        if (ev[i].type == EV_SYN && ev[i].code == SYN_DROPPED) { if (!syn_dropped[kind]) LOG("WARN %s: SYN_DROPPED (resync)", dev_what[kind]); syn_dropped[kind] = 1; continue; }
        if (syn_dropped[kind]) {	/* discard up to and including the next SYN_REPORT, then re-read the key state */
            if (ev[i].type == EV_SYN && ev[i].code == SYN_REPORT) { syn_dropped[kind] = 0; resync(kind, "SYN_DROPPED"); }
            continue; }
        if (ev[i].type != EV_KEY) continue;
        if ((kind == DEV_KEY && ev[i].code == KEY_EINK) || (kind == DEV_POWER && ev[i].code == KEY_POWER)) LOG("stage physical key=%u value=%d mono_ms=%.3f", ev[i].code, ev[i].value, now() * 1000);
        if (kind == DEV_KEY && ev[i].code == KEY_EINK) { struct dx_out o = dx_eink_key(&S, ev[i].value, now()); handle(&o, "e-ink key"); }
        if (kind == DEV_POWER && ev[i].code == KEY_POWER) { S.pw_not_held = !dev_grabbed[DEV_POWER];
            struct dx_out o = dx_power_key(&S, ev[i].value, now()); handle(&o, "power key"); }
    }	/* front touch: grabbed = read and dropped; not grabbed = Android has its own copy, ours is discarded */
}

/* ---------------- eink-round3 0016: suspend-aware main-loop watchdog ----------------
 * r5 pass2 F20 kept alarm(WATCHDOG_S) re-armed by every loop iteration. On this kernel the timer keeps running while
 * the frozen daemon is in a system suspend (s2idle), so it fired after every sleep longer than ~10 s (6 Oct: SIGALRM at
 * the resumes after 17 s and 43 s) and init restarted the daemon on the LCD. Now the SIGALRM handler forgives an expiry
 * when a suspend happened since the last heartbeat (kernel suspend counter moved, or CLOCK_BOOTTIME ran > 1 s ahead of
 * CLOCK_MONOTONIC: dx_watchdog_suspended) and re-arms; otherwise (a real hang) it dies with SIGALRM exactly as before.
 * Everything in the handler is async-signal-safe (open/read/close, clock_gettime, alarm, signal, raise). */
static int watchdog_s = WATCHDOG_S;	/* --watchdog-s N: host tests only */
static char wd_path[2][640];
static volatile long wd_count = -1;
static struct timespec wd_boot, wd_mono;
static volatile sig_atomic_t wd_rearms, wd_saved;
static long wd_read_count(void) {	/* /sys/power/suspend_stats/{success,fail}, summed; -1 = unreadable */
    long sum = 0;
    for (int i = 0; i < 2; i++) {
        int fd = open(wd_path[i], O_RDONLY | O_CLOEXEC); if (fd < 0) return -1;
        char b[32]; ssize_t r = read(fd, b, sizeof b - 1); close(fd); if (r <= 0) return -1;
        long v = 0; int any = 0; for (ssize_t k = 0; k < r && b[k] >= '0' && b[k] <= '9'; k++) { v = v * 10 + (b[k] - '0'); any = 1; }
        if (!any) return -1; sum += v;
    }
    return sum;
}
static double ts_diff(const struct timespec *a, const struct timespec *b) { return (double)(a->tv_sec - b->tv_sec) + (a->tv_nsec - b->tv_nsec) / 1e9; }
static void wd_heartbeat(void) {
    if (wd_saved) { LOG("watchdog: %d expiry(ies) during a system suspend forgiven, re-armed (no restart)", (int)wd_saved); wd_saved = 0; }
    wd_count = wd_read_count(); clock_gettime(CLOCK_BOOTTIME, &wd_boot); clock_gettime(CLOCK_MONOTONIC, &wd_mono);
    wd_rearms = 0; alarm((unsigned)watchdog_s);
}
static void on_alarm(int sig) {
    struct timespec b, m; long c = wd_read_count();
    clock_gettime(CLOCK_BOOTTIME, &b); clock_gettime(CLOCK_MONOTONIC, &m);
    if (wd_rearms < DX_WATCHDOG_MAX_REARMS && dx_watchdog_suspended(wd_count, c, ts_diff(&b, &wd_boot), ts_diff(&m, &wd_mono))) {
        wd_rearms++; wd_saved++; wd_count = c; wd_boot = b; wd_mono = m; alarm((unsigned)watchdog_s); return; }
    signal(sig, SIG_DFL); raise(sig);	/* a real hang: die as before; init restarts us (the screen is restored, see main) */
}
#define P_RESTORED_AT "vendor.dualux.restored_at"
static double boottime_s(void) { struct timespec ts; clock_gettime(CLOCK_BOOTTIME, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

int main(int argc, char **argv) {
    double exit_after = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(n) (!strcmp(a, n) && v && ++i)
        if (OPT("--sysroot")) sysroot = v; else if (OPT("--prop-dir")) prop_dir = v; else if (OPT("--key-dev")) key_spec = v;
        else if (OPT("--power-dev")) power_spec = v; else if (OPT("--front-dev")) front_spec = v; else if (OPT("--awake-file")) awake_file = v;
        else if (OPT("--fake-uinput")) fake_uinput = v; else if (OPT("--lcd-bl")) lcd_bl_name = v; else if (OPT("--fl")) fl_spec = v; else if (OPT("--exit-after")) exit_after = atof(v);
        else if (!strcmp(a, "--no-uinput")) use_uinput = 0;
        else if (OPT("--watchdog-s")) watchdog_s = atoi(v) > 0 ? atoi(v) : WATCHDOG_S;
        else { fprintf(stderr, "usage: see the header of a6l_dualux.c\n"); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    snprintf(wd_path[0], sizeof wd_path[0], "%s/sys/power/suspend_stats/success", sysroot);
    snprintf(wd_path[1], sizeof wd_path[1], "%s/sys/power/suspend_stats/fail", sysroot);
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = on_alarm; sigemptyset(&sa.sa_mask); sa.sa_flags = SA_RESTART; sigaction(SIGALRM, &sa, NULL); }
    struct dx_cfg c; dx_default_cfg(&c); dx_fl_default(&FL);
    find_lcd_backlight(); find_frontlight(); find_lcd_dpms();
    /* The phone always boots on the LCD (vendor.dualux.state is empty after a reboot, and a clean stop publishes "lcd").
     * eink-round3 0016: after an UNEXPECTED restart while the e-ink was the active screen (watchdog, crash), come back on
     * the e-ink instead of silently dropping to the LCD; at most once per 60 s (a crash loop falls back to the LCD, power
     * key left to Android). persist.sys.a6l.dualux.restore=0 disables it. */
    char prev_state[32] = ""; prop_get(P_STATE, prev_state, sizeof prev_state);
    double now_boot = boottime_s(), last_restore = prop_double(P_RESTORED_AT, 0);
    int start_screen = dx_restore_screen(prev_state, prop_int("persist.sys.a6l.dualux.restore", 1), last_restore, now_boot);
    if (start_screen == DX_EINK) { char b[32]; snprintf(b, sizeof b, "%.0f", now_boot); prop_set(P_RESTORED_AT, b);
        LOG("previous instance ended on the e-ink (state %s): restoring the e-ink screen", prev_state); }
    else if (!strncmp(prev_state, "eink", 4)) LOG("WARN previous instance ended on the e-ink (state %s) but %s: starting on the LCD", prev_state,
        prop_int("persist.sys.a6l.dualux.restore", 1) ? "it was already restored < 60 s ago (crash loop?)" : "restore is disabled");
    dx_init(&S, &c, start_screen, read_awake());
    read_cfg(); uinput_open();
    open_dev(DEV_KEY, key_spec); open_dev(DEV_POWER, power_spec); open_dev(DEV_FRONT, front_spec);
    char req_last[96] = ""; prop_get(P_REQ, req_last, sizeof req_last);	/* requests made before we started are stale */
    blanked_by_us = 1;	/* a previous instance may have died in e-ink mode: unblank the LCD if Android is awake */
    prop_set(P_PREPARE, "");
    publish(); enforce_backlight(); apply_grabs(); enforce_frontlight();
    LOG("start: screen=%s awake=%d eink_key=%s power_long=%d ms fl=%s uinput=%s", dx_state_name(&S), S.awake, S.cfg.ekey_in_eink ? "clear" : "sleep", S.cfg.power_long_ms, fl_dir[0] ? fl_dir : "none", S.no_inject ? "NO (power key left to Android)" : "yes");
    double t0 = now(), next_slow = 0, next_scan = now() + 5;
    while (!stop && (!exit_after || now() - t0 < exit_after)) {
        wd_heartbeat();	/* r5 pass2 F20: a hung loop must not keep the power key grabbed (0016: suspend-aware) */
        struct pollfd p[NDEV]; int map[NDEV], n = 0;
        for (int k = 0; k < NDEV; k++) if (dev_fd[k] >= 0) { map[n] = k; p[n++] = (struct pollfd){dev_fd[k], POLLIN, 0}; }
        int held = S.ek_down || S.pw_down, to = held ? 50 : S.appearance_hold ? 50 : S.screen == DX_EINK ? 100 : 250;
        int r = poll(p, (nfds_t)n, to);
        if (r < 0 && errno != EINTR) { LOG("FAIL poll: %s", strerror(errno)); break; }
        for (int i = 0; r > 0 && i < n; i++) if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) drain(map[i]);
        double t = now();
        struct dx_out o = dx_tick(&S, t); handle(&o, "long press");
        int aw = read_awake(); if (aw != S.awake) { dx_set_awake(&S, aw); LOG("Android %s (%s)", aw ? "awake" : "asleep", dx_state_name(&S)); LOG("stage display awake=%d mono_ms=%.3f hold=%d", aw, now() * 1000, S.appearance_hold);
            /* eink-lockscreen: keep the system up after falling asleep on the e-ink, so a6l_einklock reads
             * vendor.dualux.state = eink-asleep and takes its own wakelock before the first suspend (timed: never sticks).
             * eink-round5: 3 s (einklock polls every 250 ms, then holds its own lock through its 0.8 s entry delay); the rc
             * now grants CAP_BLOCK_SUSPEND: until then this write was EPERM and Android suspended ~0.1-0.7 s after the
             * screen went off (as soon as the mirror's "power off" released a6l_epdd_crtc), before the lock picture. */
            if (!aw && S.screen == DX_EINK && prop_int("persist.sys.a6l.eink.lock", 1)) {
                char p[600]; snprintf(p, sizeof p, "%s/sys/power/wake_lock", sysroot); int fd = open(p, O_WRONLY | O_CLOEXEC), e = 0;
                static const char wl[] = "a6l_dualux_lock 3000000000";
                if (fd < 0) e = errno; else { if (write(fd, wl, sizeof wl - 1) < 0) e = errno; close(fd); }
                static int last_e = -1;
                if (e != last_e) { if (e) LOG("WARN lock-screen hand-over wakelock not taken: %s%s - the system may suspend before the e-ink lock picture", strerror(e), e == EPERM ? " (needs CAP_BLOCK_SUSPEND: rc 'capabilities BLOCK_SUSPEND')" : ""); else LOG("lock-screen hand-over wakelock: ok (3 s)"); last_e = e; } }
            publish(); apply_grabs();
            /* eink-round2: a prepare begun while Android slept (power key on the sleeping e-ink -> LCD) is only seen by
             * the app once it polls again after SCREEN_ON. Give it the full window from the wake-up, not from the key
             * press: a fail-open before the app applied the target left sys.a6l.dualux.appearance on "<seq> eink"
             * (WM rear white wallpaper over the LCD home, user report 6 Oct). */
            if (aw && S.appearance_hold == 1 && appearance_deadline < now() + 3.0) { appearance_deadline = now() + 3.0; LOG("appearance %s: deadline restarted at wake-up", appearance_req); } }
        char rq[96]; if (prop_get(P_REQ, rq, sizeof rq) > 0 && strcmp(rq, req_last)) { snprintf(req_last, sizeof req_last, "%s", rq);
            const char *cmd = strchr(rq, ' '); cmd = cmd ? cmd + 1 : rq; o = dx_request(&S, cmd); handle(&o, "request"); }
        appearance_tick(); enforce_backlight(); enforce_frontlight();
        if (t >= next_slow) { next_slow = t + 1; read_cfg(); }
        if (t >= next_scan) { next_scan = t + 5;
            if (dev_fd[DEV_KEY] < 0) open_dev(DEV_KEY, key_spec); if (dev_fd[DEV_POWER] < 0) open_dev(DEV_POWER, power_spec);
            if (dev_fd[DEV_FRONT] < 0) open_dev(DEV_FRONT, front_spec);
            if (ui < 0) uinput_open();	/* r5 pass2 F20: retry; the power key is grabbed only once it works */
            if (dev_fd[DEV_FRONT] >= 0 || dev_fd[DEV_POWER] >= 0) apply_grabs();
            if (!lcd_bl[0]) find_lcd_backlight(); if (!fl_dir[0]) find_frontlight(); if (!lcd_dpms[0] && !awake_file) find_lcd_dpms(); }
    }
    /* never leave the phone with a dark LCD and a dead power key */
    /* r5 bug hunt eink-display E3: unblank only when Android is awake. Forcing awake = 1 here switched the LCD backlight
     * on over a sleeping (dark) panel when the service stopped while the phone was asleep (bl_power belongs to the
     * composer then; F50 contract: never override Android's blank). vendor.dualux.lcd_blank=0 is published first, so the
     * composer unblanks at the next wake-up by itself. */
    alarm(0); S.appearance_hold = 0; prop_set(P_PREPARE, ""); S.screen = DX_LCD; publish(); S.awake = read_awake(); blanked_by_us = 1; enforce_backlight(); apply_grabs(); if (fl_dir[0]) { char p[700]; snprintf(p, sizeof p, "%s/brightness", fl_dir); wr_int(p, 0); }
    if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
    LOG("exit (LCD restored)");
    return 0;
}
