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
static const char *sysroot = "", *prop_dir, *key_spec = "auto", *power_spec = "auto", *front_spec = "auto", *awake_file, *lcd_bl_name, *fl_spec;
static int use_uinput = 1; static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static double now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    char b[512]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("A6L_DUALUX %s\n", b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    __android_log_write(strncmp(b, "FAIL", 4) ? strncmp(b, "WARN", 4) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN : ANDROID_LOG_ERROR, "a6l_dualux", b);
#endif
}
#define LOG(...) logline(__VA_ARGS__)

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
static void uinput_open(void) {
    if (!use_uinput) return;
    ui = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC); if (ui < 0) { LOG("WARN /dev/uinput: %s (no wake/sleep/power injection)", strerror(errno)); return; }
    ioctl(ui, UI_SET_EVBIT, EV_KEY); ioctl(ui, UI_SET_EVBIT, EV_SYN);
    ioctl(ui, UI_SET_KEYBIT, KEY_POWER); ioctl(ui, UI_SET_KEYBIT, KEY_SLEEP); ioctl(ui, UI_SET_KEYBIT, KEY_WAKEUP);
    struct uinput_setup us; memset(&us, 0, sizeof us); us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0x2a6c; us.id.product = 0x0d0a; us.id.version = 1;
    snprintf(us.name, sizeof us.name, "a6l-dualux-keys");
    if (ioctl(ui, UI_DEV_SETUP, &us) || ioctl(ui, UI_DEV_CREATE)) { LOG("WARN uinput setup: %s", strerror(errno)); close(ui); ui = -1; return; }
    LOG("uinput a6l-dualux-keys created");
}
static void emit(int type, int code, int val) {
    if (ui < 0) return; struct input_event ev; memset(&ev, 0, sizeof ev); ev.type = (unsigned short)type; ev.code = (unsigned short)code; ev.value = val;
    if (write(ui, &ev, sizeof ev) != sizeof ev) LOG("WARN uinput write: %s", strerror(errno));
}
static void key_edge(int code, int val, const char *what) { LOG("inject %s %s", what, val ? "down" : "up"); emit(EV_KEY, code, val); emit(EV_SYN, SYN_REPORT, 0); }
static void key_tap(int code, const char *what) { key_edge(code, 1, what); key_edge(code, 0, what); }

/* ---------------- state application ---------------- */
static struct dx_state S; static struct dx_fl_cfg FL; static unsigned clear_seq; static int last_fl = -1, blanked_by_us;
#define P_REQ "sys.a6l.dualux.req"
#define P_STATE "vendor.dualux.state"
static void publish(void) {
    prop_set("vendor.dualux.lcd_blank", dx_lcd_blank(&S) ? "1" : "0");	/* read by the patched composer (0002 patch): no LCD flash at wake-up */
    prop_set(P_STATE, dx_state_name(&S)); prop_set("persist.vendor.eink.mode", dx_mirror_on(&S) ? "mirror" : "off"); }
static void enforce_backlight(void) {
    if (!lcd_bl[0]) return;
    char p[700]; snprintf(p, sizeof p, "%s/bl_power", lcd_bl); int pw = rd_int(p, -1);
    if (dx_lcd_blank(&S)) { if (pw != 4) { if (!wr_int(p, 4)) { if (!blanked_by_us) LOG("LCD backlight blanked (bl_power 4)"); else LOG("LCD backlight re-blanked (composer turned it on at wake-up)"); blanked_by_us = 1; } } }
    else if (blanked_by_us) {	/* back to the LCD: unblank only when Android is awake (asleep = the composer owns bl_power) */
        if (S.awake && pw == 4) { if (!wr_int(p, 0)) LOG("LCD backlight unblanked (bl_power 0)"); }
        blanked_by_us = 0; }
}
static void enforce_frontlight(void) {
    if (!fl_dir[0]) return;
    char p[700]; int v = 0; if (lcd_bl[0]) { snprintf(p, sizeof p, "%s/brightness", lcd_bl); v = rd_int(p, 0); }
    int lvl = dx_frontlight_level(&S, &FL, v, lcd_bl_max, lcd_bl_linear, fl_max);
    if (lvl == last_fl) return;
    snprintf(p, sizeof p, "%s/brightness", fl_dir);
    if (!wr_int(p, lvl)) { if (last_fl < 0 || (lvl == 0) != (last_fl == 0)) LOG("frontlight %d/%d (LCD request %d/%d)", lvl, fl_max, v, lcd_bl_max); last_fl = lvl; }
    else { static int w; if (!w++) LOG("WARN frontlight write %s: %s", p, strerror(errno)); }
}
static void apply_grabs(void) { set_grab(DEV_POWER, dx_power_grabbed(&S)); set_grab(DEV_FRONT, dx_front_touch_grabbed(&S)); }
static void handle(const struct dx_out *o, const char *why) {
    if (o->set_screen >= 0 && o->set_screen != S.screen) LOG("%s: %s -> %s", why, S.screen == DX_EINK ? "e-ink" : "LCD", o->set_screen == DX_EINK ? "e-ink" : "LCD");
    int old = S.screen; dx_apply(&S, o);
    if (S.screen != old) { publish(); enforce_backlight(); apply_grabs(); enforce_frontlight(); }
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
static void drain(int kind) {
    struct input_event ev[64]; ssize_t r = read(dev_fd[kind], ev, sizeof ev);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) {	/* device gone (or FIFO writer closed in host tests): close, rescanned every 5 s */
        if (r < 0) LOG("WARN %s read: %s (reopening)", dev_what[kind], strerror(errno));
        close(dev_fd[kind]); dev_fd[kind] = -1; dev_grabbed[kind] = 0; return; }
    for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++) {
        if (ev[i].type != EV_KEY) continue;
        if (kind == DEV_KEY && ev[i].code == KEY_EINK) { struct dx_out o = dx_eink_key(&S, ev[i].value, now()); handle(&o, "e-ink key"); }
        if (kind == DEV_POWER && ev[i].code == KEY_POWER) { struct dx_out o = dx_power_key(&S, ev[i].value, now()); handle(&o, "power key"); }
    }	/* front touch: grabbed = read and dropped; not grabbed = Android has its own copy, ours is discarded */
}

int main(int argc, char **argv) {
    double exit_after = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(n) (!strcmp(a, n) && v && ++i)
        if (OPT("--sysroot")) sysroot = v; else if (OPT("--prop-dir")) prop_dir = v; else if (OPT("--key-dev")) key_spec = v;
        else if (OPT("--power-dev")) power_spec = v; else if (OPT("--front-dev")) front_spec = v; else if (OPT("--awake-file")) awake_file = v;
        else if (OPT("--lcd-bl")) lcd_bl_name = v; else if (OPT("--fl")) fl_spec = v; else if (OPT("--exit-after")) exit_after = atof(v);
        else if (!strcmp(a, "--no-uinput")) use_uinput = 0;
        else { fprintf(stderr, "usage: see the header of a6l_dualux.c\n"); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN);
    struct dx_cfg c; dx_default_cfg(&c); dx_fl_default(&FL);
    find_lcd_backlight(); find_frontlight(); find_lcd_dpms();
    dx_init(&S, &c, DX_LCD, read_awake());	/* the phone always boots on the LCD */
    read_cfg(); uinput_open();
    open_dev(DEV_KEY, key_spec); open_dev(DEV_POWER, power_spec); open_dev(DEV_FRONT, front_spec);
    char req_last[96] = ""; prop_get(P_REQ, req_last, sizeof req_last);	/* requests made before we started are stale */
    blanked_by_us = 1;	/* a previous instance may have died in e-ink mode: unblank the LCD if Android is awake */
    publish(); enforce_backlight(); apply_grabs(); enforce_frontlight();
    LOG("start: screen=%s awake=%d eink_key=%s power_long=%d ms fl=%s", dx_state_name(&S), S.awake, S.cfg.ekey_in_eink ? "clear" : "sleep", S.cfg.power_long_ms, fl_dir[0] ? fl_dir : "none");
    double t0 = now(), next_slow = 0, next_scan = now() + 5;
    while (!stop && (!exit_after || now() - t0 < exit_after)) {
        struct pollfd p[NDEV]; int map[NDEV], n = 0;
        for (int k = 0; k < NDEV; k++) if (dev_fd[k] >= 0) { map[n] = k; p[n++] = (struct pollfd){dev_fd[k], POLLIN, 0}; }
        int held = S.ek_down || S.pw_down, to = held ? 50 : S.screen == DX_EINK ? 100 : 250;
        int r = poll(p, (nfds_t)n, to);
        if (r < 0 && errno != EINTR) { LOG("FAIL poll: %s", strerror(errno)); break; }
        for (int i = 0; r > 0 && i < n; i++) if (p[i].revents & (POLLIN | POLLHUP | POLLERR)) drain(map[i]);
        double t = now();
        struct dx_out o = dx_tick(&S, t); handle(&o, "long press");
        int aw = read_awake(); if (aw != S.awake) { dx_set_awake(&S, aw); LOG("Android %s (%s)", aw ? "awake" : "asleep", dx_state_name(&S)); publish(); apply_grabs(); }
        char rq[96]; if (prop_get(P_REQ, rq, sizeof rq) > 0 && strcmp(rq, req_last)) { snprintf(req_last, sizeof req_last, "%s", rq);
            const char *cmd = strchr(rq, ' '); cmd = cmd ? cmd + 1 : rq; o = dx_request(&S, cmd); handle(&o, "request"); }
        enforce_backlight(); enforce_frontlight();
        if (t >= next_slow) { next_slow = t + 1; read_cfg(); }
        if (t >= next_scan) { next_scan = t + 5;
            if (dev_fd[DEV_KEY] < 0) open_dev(DEV_KEY, key_spec); if (dev_fd[DEV_POWER] < 0) open_dev(DEV_POWER, power_spec);
            if (dev_fd[DEV_FRONT] < 0) open_dev(DEV_FRONT, front_spec); if (dev_fd[DEV_FRONT] >= 0 || dev_fd[DEV_POWER] >= 0) apply_grabs();
            if (!lcd_bl[0]) find_lcd_backlight(); if (!fl_dir[0]) find_frontlight(); if (!lcd_dpms[0] && !awake_file) find_lcd_dpms(); }
    }
    /* never leave the phone with a dark LCD and a dead power key */
    S.screen = DX_LCD; publish(); S.awake = 1; blanked_by_us = 1; enforce_backlight(); apply_grabs(); if (fl_dir[0]) { char p[700]; snprintf(p, sizeof p, "%s/brightness", fl_dir); wr_int(p, 0); }
    if (ui >= 0) { ioctl(ui, UI_DEV_DESTROY); close(ui); }
    LOG("exit (LCD restored)");
    return 0;
}
