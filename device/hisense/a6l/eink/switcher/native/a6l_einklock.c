// SPDX-License-Identifier: Apache-2.0
/* a6l_einklock — e-ink lock screen of the Hisense A6L (vendor daemon; eink-lockscreen 6 Oct 2026,
 * firmware/extracted/eink-lockscreen-20261006/README.md).
 *
 * When Android sleeps while the rear e-ink is the active screen (vendor.dualux.state = eink-asleep), the e-ink shows a lock
 * screen (time, date, battery, optional owner line, chosen background) instead of the last application frame, and the
 * time keeps updating while the system is suspended — as the stock firmware did (Vision_SystemUI com.eink.keyguard:
 * minute alarm -> 800 ms wakelock -> redraw -> REGAL post -> re-arm). Here:
 *   - minute tick: timerfd CLOCK_REALTIME_ALARM (CAP_WAKE_ALARM) at each minute boundary minus --lead-ms (the panel
 *     needs ~1.4 s from a cold start to the final REGAL snap), TFD_TIMER_CANCEL_ON_SET (wall clock change = redraw),
 *     eink-round10: a cancel (also sent at every resume from suspend) redraws only if the displayed minute changed;
 *     SIGUSR1 = a simulated cancel (host tests);
 *     polled with EPOLLWAKEUP (CAP_BLOCK_SUSPEND: the system stays awake from the RTC alarm to our next epoll_wait);
 *     kernel alarms are not deferred by Doze (stock's setExactAndAllowWhileIdle is throttled to ~9 min in deep Doze);
 *   - a timed wakelock "a6l_einklock" (15 s) around each a6l_epdd transaction;
 *   - a6l_epdd "lockframe 720 1440 reading [force]" + the picture: REGAL drives only the changed pixels (the digits);
 *     a6l_epdd switches the e-ink CRTC off right after the frame (eink-round3 0013: never suspend with it enabled);
 *     every --clean-min minutes (persist.sys.a6l.eink.lock_clean_min, default 60) one forced REGAL cleans the ghosts;
 *   - when the lock ends (wake-up, switch to the LCD, disabled): "lock restore [off]" — a6l_epdd redraws the mirror's last
 *     picture unless the mirror already replaced the lock picture (the mirror only sends pixels that changed from ITS
 *     last frame, so without this an unchanged app page would stay hidden behind the lock picture after wake-up);
 *     eink-round5: never during an LCD transition: the restore runs 1 s after waking on the e-ink, and after a wake-up on
 *     the LCD only once the e-ink is used again (the e-ink keeps the lock picture meanwhile, as stock did); the lock entry
 *     comes 0.8 s after eink-asleep (wakelock held from the first sight, restarted after a suspend) and is re-checked
 *     right before the frame is sent (einklock_logic.h ELK_ENTRY_DELAY_S);
 *     eink-round10: after a lock shown in LCD mode (mirror off) no restore: the mirror, turned on again, sends its first
 *     capture and a6l_epdd draws it as the GC16 clean that follows a lock picture (einklock_logic.h ELK_MIRROR_WAIT_S);
 *   - LCD mode: nothing (state lcd). eink-round6: in LCD mode the e-ink shows the lock picture too unless it mirrors the
 *     LCD (persist.vendor.eink.mode = mirror), with the minute ticks; 2 s after LCD mode is seen (so right after boot,
 *     and after a switch to the LCD); ticks wait 1.5 s after a display transition (vendor.dualux.awake / state change).
 *     persist.sys.a6l.eink.lock_lcd = 0 restores the round-5 behaviour (nothing in LCD mode).
 *   - RTC wake fallback (T0 on 6 Oct: the pm8xxx RTC alarm did not visibly wake s2idle): every tick taken while locked
 *     is checked for lateness; a tick > 20 s late (--late-s) = the alarm did not wake the system and ran at the next
 *     natural wake-up -> the picture switches to the date + small "Updated HH:MM" (refreshed at every natural wake-up),
 *     never a big clock that silently goes stale; 2 on-time ticks -> live clock again. persist.vendor.eink.lock_rtc
 *     (ok|late) remembers it; persist.sys.a6l.eink.lock_clock_mode = auto (default) | live | updated forces it.
 * Settings (properties, read every loop): persist.sys.a6l.eink.lock (1), .lock_clock (1), .lock_battery (1),
 *   .lock_24h (1|0, mirrored from Android by the app; unset = locale default), .lock_bg (white|black|image|lcd),
 *   .lock_clean_min (60), .lock_msg; persist.sys.locale; vendor.dualux.lock_bg_seq (init copied a new background:
 *   /data/vendor/a6l_eink_lock/bg.pgm, written by the app to /data/misc/a6l_eink and copied by init — Treble-clean).
 *
 * usage: a6l_einklock [--epd-socket P] [--bg PATH] [--lead-ms N] [--no-wakelock]
 *   host tests only: [--sysroot DIR] [--prop-dir DIR] [--period-s N] [--clock realtime] [--dump DIR] [--exit-after S]
 *                    [--render-once OUT.pgm --at EPOCH]   (render one picture with the current settings and exit)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include "einklock_logic.h"
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef A6L_ANDROID_LOG
#include <android/log.h>
#endif
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif
#ifndef EPOLLWAKEUP
#define EPOLLWAKEUP (1u << 29)
#endif

static const char *sysroot = "", *prop_dir, *epd_socket = "/dev/socket/a6l_epd", *bg_path = "/data/vendor/a6l_eink_lock/bg.pgm", *dump_dir;
static int period_s = 60, lead_ms = 1000, use_wakelock = 1, force_realtime;
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static volatile sig_atomic_t fake_cancel;	/* eink-round10: SIGUSR1 = a simulated TFD_TIMER_CANCEL_ON_SET cancel (host tests) */
static void on_usr1(int s) { (void)s; fake_cancel = 1; }
static double mono(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double realtime(void) { struct timespec t; clock_gettime(CLOCK_REALTIME, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double boottime(void) { struct timespec t; if (clock_gettime(CLOCK_BOOTTIME, &t)) return mono(); return t.tv_sec + t.tv_nsec / 1e9; }

static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    char b[600]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("A6L_EINKLOCK %s\n", b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    char stamped[sizeof b + 96];
    snprintf(stamped, sizeof stamped, "boot_ms=%.3f mono_ms=%.3f %s", boottime() * 1000, mono() * 1000, b);
    __android_log_write(strncmp(b, "FAIL", 4) ? strncmp(b, "WARN", 4) ? ANDROID_LOG_INFO : ANDROID_LOG_WARN : ANDROID_LOG_ERROR, "a6l_einklock", stamped);
#else
    (void)boottime;
#endif
}
#define LOG(...) logline(__VA_ARGS__)
/* eink-round5: display-transition markers in the kernel log ("<6>a6l_einklock: ..."; printk.devkmsg=on). eink-round6: /dev/kmsg is
 * 0600 root:root (first-stage init), so a system daemon cannot open it: /dev/kmsg_debug (0622, userdebug/eng) instead.
 * One timeline with the DPU/DSI/SMMU/PM messages for the round-5 repro (kmsg streamed to the laptop / fsync'ed on the
 * phone). Android builds only: host tests never write the host's kernel log. */
static void kmark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void kmark(const char *fmt, ...) {
#ifdef __ANDROID__
    static int fd = -2;
    if (fd == -2) { fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC); if (fd < 0) fd = open("/dev/kmsg_debug", O_WRONLY | O_CLOEXEC); }
    if (fd < 0) return;
    char b[300]; int n = snprintf(b, sizeof b, "<6>a6l_einklock: "); va_list ap; va_start(ap, fmt);
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
static int prop_int(const char *k, int def) { char v[96]; return prop_get(k, v, sizeof v) > 0 ? atoi(v) : def; }
static void prop_set(const char *k, const char *v) {
    if (prop_dir) { char p[512], t[520]; snprintf(p, sizeof p, "%s/%s", prop_dir, k); snprintf(t, sizeof t, "%s.tmp", p);
        FILE *f = fopen(t, "w"); if (f) { fprintf(f, "%s\n", v); fclose(f); rename(t, p); } return; }
#ifdef __ANDROID__
    if (__system_property_set(k, v)) LOG("WARN setprop %s=%s refused", k, v);
#endif
}
/* RTC wake check (einklock_logic.h): learned state, consecutive on-time ticks, lateness threshold (--late-s) */
static int rtc_state = ELK_RTC_UNKNOWN, rtc_ontime; static double late_s = 20;

static void read_cfg(struct elk_cfg *c) {
    char v[128]; elk_default_cfg(c);
    c->enabled = prop_int("persist.sys.a6l.eink.lock", 1) != 0;
    c->clock = prop_int("persist.sys.a6l.eink.lock_clock", 1) != 0;
    c->battery = prop_int("persist.sys.a6l.eink.lock_battery", 1) != 0;
    prop_get("persist.sys.a6l.eink.lock_24h", v, sizeof v); c->use24 = !strcmp(v, "1") ? 1 : !strcmp(v, "0") ? 0 : -1;
    int cm = prop_int("persist.sys.a6l.eink.lock_clean_min", 60); c->clean_min = cm < 0 ? 0 : cm > 1440 ? 1440 : cm;
    prop_get("persist.sys.a6l.eink.lock_bg", v, sizeof v); c->bg = elk_parse_bg(v);
    if (prop_get("persist.sys.locale", c->locale, sizeof c->locale) <= 0) prop_get("ro.product.locale", c->locale, sizeof c->locale);
    prop_get("persist.sys.a6l.eink.lock_msg", c->msg, sizeof c->msg);
}

/* ---------------- background picture ---------------- */
static uint8_t bg[ELK_W * ELK_H]; static int have_bg; static long bg_seq = -2;
static void load_bg(void) {
    char p[600]; snprintf(p, sizeof p, "%s%s", sysroot, bg_path);
    int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW); have_bg = 0;
    if (fd < 0) { LOG("background %s: %s (plain white instead)", p, strerror(errno)); return; }
    static uint8_t buf[ELK_W * ELK_H + 256]; size_t n = 0; ssize_t r;
    while (n < sizeof buf && (r = read(fd, buf + n, sizeof buf - n)) > 0) n += (size_t)r;
    close(fd);
    long seq; if (elk_parse_pgm(buf, n, bg, &seq)) { LOG("WARN background %s: not a 720x1440 8-bit PGM (%zu bytes): plain white instead", p, n); return; }
    have_bg = 1; LOG("background loaded (seq %ld)", seq);
}

/* ---------------- battery ---------------- */
static void battery(int *pct, int *charging) {
    static const char *names[] = {"qcom-battery", "battery", NULL}; char p[600], v[64];
    *pct = -1; *charging = 0;
    for (int i = 0; names[i]; i++) {
        snprintf(p, sizeof p, "%s/sys/class/power_supply/%s/capacity", sysroot, names[i]);
        FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(v, sizeof v, f)) *pct = atoi(v); fclose(f);
        snprintf(p, sizeof p, "%s/sys/class/power_supply/%s/status", sysroot, names[i]);
        f = fopen(p, "r"); if (f) { if (fgets(v, sizeof v, f)) *charging = !strncmp(v, "Charging", 8) || !strncmp(v, "Full", 4); fclose(f); }
        return;
    }
}

/* ---------------- wakelock ---------------- */
static void wakelock(int on) {
    if (!use_wakelock) return;
    int f = open(on ? "/sys/power/wake_lock" : "/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC); if (f < 0) return;
    const char *s = on ? "a6l_einklock 15000000000" : "a6l_einklock";	/* timed: a stuck transaction cannot block suspend */
    if (write(f, s, strlen(s)) < 0 && on) { static int w; if (!w++) LOG("WARN wakelock: %s", strerror(errno)); }
    close(f);
}

/* ---------------- a6l_epdd client (one transaction at a time, blocking with a 30 s bound) ---------------- */
static int epd = -1;
static int epd_connect(void) {
    if (epd >= 0) return 0;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", epd_socket);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) { LOG("WARN a6l_epdd %s: %s", epd_socket, strerror(errno)); if (s >= 0) close(s); return -1; }
    struct timeval tv = {30, 0}; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    epd = s; return 0;
}
static int send_all(const void *p, size_t n) {
    const uint8_t *b = p; while (n) { ssize_t w = send(epd, b, n, MSG_NOSIGNAL); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; } b += w; n -= (size_t)w; } return 0;
}
static int transact(const char *line, const uint8_t *px, size_t n, char *reply, size_t rn) {
    for (int attempt = 0; attempt < 2; attempt++) {
        reply[0] = 0;
        if (epd_connect()) return -1;
        char l[128]; int ln = snprintf(l, sizeof l, "%s\n", line); size_t got = 0; int ok = 0;
        if (!send_all(l, (size_t)ln) && (!px || !send_all(px, n))) {
            while (got < rn - 1) { ssize_t r = recv(epd, reply + got, rn - 1 - got, 0); if (r <= 0) break; got += (size_t)r; reply[got] = 0; if (strchr(reply, '\n')) { ok = 1; break; } }
        }
        if (ok) { reply[strcspn(reply, "\n")] = 0; return strncmp(reply, "OK", 2) ? -1 : 0; }
        LOG("WARN a6l_epdd transaction '%s' failed (%s): reconnecting", line, got ? "partial reply" : strerror(errno));
        close(epd); epd = -1;
    }
    return -1;
}

/* ---------------- minute timer ---------------- */
static int tfd = -1, armed; static long long armed_target;
static void timer_open(void) {
    if (!force_realtime) tfd = timerfd_create(CLOCK_REALTIME_ALARM, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) { if (!force_realtime) LOG("WARN timerfd CLOCK_REALTIME_ALARM: %s (no CAP_WAKE_ALARM?): CLOCK_REALTIME, the clock will NOT update while suspended", strerror(errno));
        else LOG("minute timer: CLOCK_REALTIME (--clock realtime, host tests), lead %d ms", lead_ms);
        tfd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC); }
    else LOG("minute timer: CLOCK_REALTIME_ALARM (wakes the system), lead %d ms", lead_ms);
}
static void timer_arm(long long target) {
    double w = (double)target - lead_ms / 1000.0; struct itimerspec it; memset(&it, 0, sizeof it);
    it.it_value.tv_sec = (time_t)w; it.it_value.tv_nsec = (long)((w - (double)(time_t)w) * 1e9);
    if (timerfd_settime(tfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &it, NULL)) { LOG("WARN timerfd_settime: %s", strerror(errno)); return; }
    armed = 1; armed_target = target;
}
static void timer_disarm(void) { struct itimerspec it; memset(&it, 0, sizeof it); timerfd_settime(tfd, 0, &it, NULL); armed = 0; armed_target = 0; }

/* ---------------- drawing ---------------- */
static uint8_t canvas[ELK_W * ELK_H];
static long long drawn_t = -1;	/* eink-round10: the minute the last lock frame showed (elk_wallclock_redraw) */
static void render_at(long long t, const struct elk_cfg *c) {
    struct elk_info in; memset(&in, 0, sizeof in); time_t tt = (time_t)t; localtime_r(&tt, &in.tm);
    battery(&in.battery_pct, &in.charging);
    elk_render(canvas, have_bg ? bg : NULL, c, &in, NULL);
}
static int write_pgm(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P5\n%d %d\n255\n", ELK_W, ELK_H); int ok = fwrite(canvas, 1, sizeof canvas, f) == sizeof canvas;
    return fclose(f) || !ok ? -1 : 0;
}
static int draw(const struct elk_cfg *c, int force, const char *why) {
    long long t = elk_display_time(realtime(), period_s, lead_ms / 1000.0); double t0 = mono();
    render_at(t, c);
    double tr = mono();
    static int nframe; char reply[256], line[64]; snprintf(line, sizeof line, "lockframe %d %d reading%s", ELK_W, ELK_H, force ? " force" : "");
    if (dump_dir) { char p[600]; snprintf(p, sizeof p, "%s/lock%03d.pgm", dump_dir, nframe); write_pgm(p); }
    nframe++; drawn_t = t;
    kmark("lock frame %d (%s) sent", nframe, why);
    int rc = transact(line, canvas, sizeof canvas, reply, sizeof reply);
    struct tm tm; time_t tt = (time_t)t; localtime_r(&tt, &tm);
    LOG("lock frame %d (%s%s) for %02d:%02d: render %.0f ms, a6l_epdd %.0f ms: %s", nframe, why, force ? ", forced ghost cleanup" : "",
        tm.tm_hour, tm.tm_min, (tr - t0) * 1000, (mono() - tr) * 1000, reply[0] ? reply : "no reply");
    return rc;
}

int main(int argc, char **argv) {
    double exit_after = 0, at = -1; const char *render_once = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(n) (!strcmp(a, n) && v && ++i)
        if (OPT("--sysroot")) sysroot = v; else if (OPT("--prop-dir")) prop_dir = v; else if (OPT("--epd-socket")) epd_socket = v;
        else if (OPT("--bg")) bg_path = v; else if (OPT("--lead-ms")) lead_ms = atoi(v); else if (OPT("--period-s")) period_s = atoi(v);
        else if (OPT("--dump")) dump_dir = v; else if (OPT("--exit-after")) exit_after = atof(v);
        else if (OPT("--clock")) force_realtime = !strcmp(v, "realtime");
        else if (OPT("--render-once")) render_once = v; else if (OPT("--at")) at = atof(v);
        else if (!strcmp(a, "--no-wakelock")) use_wakelock = 0; else if (OPT("--late-s")) late_s = atof(v);
        else { fprintf(stderr, "usage: see the header of a6l_einklock.c (%s)\n", a); return 2; }
    }
    if (lead_ms < 0 || lead_ms > 1500) lead_ms = 1000;
    if (period_s < 1 || period_s > 3600) period_s = 60;
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN); signal(SIGUSR1, on_usr1);
    { char v[16]; prop_get("persist.vendor.eink.lock_rtc", v, sizeof v); rtc_state = !strcmp(v, "late") ? ELK_RTC_LATE : !strcmp(v, "ok") ? ELK_RTC_OK : ELK_RTC_UNKNOWN; }
    struct elk_cfg cfg, prev; read_cfg(&cfg);
    { char m[16]; prop_get("persist.sys.a6l.eink.lock_clock_mode", m, sizeof m); cfg.stale = elk_stale_variant(m, rtc_state); }
    prev = cfg;
    char seq[32] = ""; prop_get("vendor.dualux.lock_bg_seq", seq, sizeof seq); bg_seq = atol(seq); load_bg();
    if (render_once) { render_at(at >= 0 ? (long long)at : (long long)realtime(), &cfg); return write_pgm(render_once) ? 1 : 0; }
    timer_open();
    int ep = epoll_create1(EPOLL_CLOEXEC); struct epoll_event ev = {.events = EPOLLIN | EPOLLWAKEUP, .data.fd = tfd};
    if (ep < 0 || epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &ev)) { LOG("FAIL epoll: %s", strerror(errno)); return 1; }
    struct elk_sm sm; elk_sm_init(&sm);
    sm.restore_pending = 1;	/* a previous instance may have died while locked: restored once awake on the e-ink */
    double t_start = mono(), gap_prev = boottime() - mono(), last_transition = -100; int holding = 0;
    LOG("start: enabled=%d clock=%d bg=%d period=%d s lead=%d ms socket=%s", cfg.enabled, cfg.clock, cfg.bg, period_s, lead_ms, epd_socket);
    while (!stop && (!exit_after || mono() - t_start < exit_after)) {
        struct epoll_event got; int n = epoll_wait(ep, &got, 1, 250), fired = 0;
        if (n < 0 && errno != EINTR) { LOG("FAIL epoll_wait: %s", strerror(errno)); break; }
        int ontime_tick = 0, cancel = 0;
        if (n > 0) { uint64_t x; ssize_t r = read(tfd, &x, sizeof x);
            if (r == (ssize_t)sizeof x) { fired = 1; ontime_tick = 1; }
            else if (r < 0 && errno == ECANCELED) cancel = 1;
            if (fired) armed = 0; }
        if (fake_cancel) { fake_cancel = 0; cancel = 1; }
        if (cancel) {	/* eink-round10: also every resume from suspend; the timer is re-armed below either way */
            static int skipped; armed = 0;
            if (!sm.locked || elk_wallclock_redraw(drawn_t, realtime(), period_s, lead_ms / 1000.0)) { fired = 1; LOG("wall clock changed: redraw"); }
            else if (++skipped <= 3 || skipped % 100 == 0)
                LOG("wall clock notification (system resume or time set), displayed minute unchanged: timer re-armed, no redraw (%d)", skipped);
        }
        if (ontime_tick && sm.locked && armed_target) {	/* RTC wake check: how late did this tick run? */
            double late = realtime() - ((double)armed_target - lead_ms / 1000.0);
            int was = rtc_state; rtc_state = elk_rtc_learn(rtc_state, &rtc_ontime, late, late_s);
            if (rtc_state != was) {
                LOG("%s: tick %.1f s late", rtc_state == ELK_RTC_LATE ? "WARN minute alarm did not wake the system (RTC wake not working?): \"updated HH:MM\" lock picture"
                                                                     : "minute alarm wakes the system: live clock", late);
                prop_set("persist.vendor.eink.lock_rtc", rtc_state == ELK_RTC_LATE ? "late" : "ok");
            }
        }
        read_cfg(&cfg);
        { char m[16]; prop_get("persist.sys.a6l.eink.lock_clock_mode", m, sizeof m); cfg.stale = elk_stale_variant(m, rtc_state); }
        char st[32]; prop_get("vendor.dualux.state", st, sizeof st);
        prop_get("vendor.dualux.lock_bg_seq", seq, sizeof seq);
        /* eink-round6: LCD mode with the e-ink not mirroring = not in use; display transitions (awake/asleep, switches) */
        char em[16], aw[8]; prop_get("persist.vendor.eink.mode", em, sizeof em); prop_get("vendor.dualux.awake", aw, sizeof aw);
        int lcd_idle = !strcmp(st, "lcd") && strcmp(em, "mirror") && prop_int("persist.sys.a6l.eink.lock_lcd", 1) != 0;
        { static char last_st[32], last_aw[8]; if (strcmp(st, last_st) || strcmp(aw, last_aw)) { last_transition = mono(); snprintf(last_st, sizeof last_st, "%s", st); snprintf(last_aw, sizeof last_aw, "%s", aw); } }
        /* eink-round6d: a6l_dualux holds the lights while a screen switch completes (vendor.dualux.prepare = the request;
         * the LCD waits for its themed frame). A lock frame in that window costs ~1.2 s of CPU (render + REGAL generation +
         * e-ink modeset) right when the LCD redraws: 7 Oct 14:18:02 wake-up 2.65 s (lock frame at +2.0 s), 14:18:59 switch
         * failed open at 3.3 s (LCD-mode lock entry at +2.2 s). The window counts as a transition until 1.5 s after it ends. */
        { char pr[96]; if (prop_get("vendor.dualux.prepare", pr, sizeof pr) > 0) last_transition = mono(); }
        int settling = mono() - last_transition < ELK_SETTLE_S;
        int changed = !elk_cfg_equal(&cfg, &prev);
        if (atol(seq) != bg_seq) { bg_seq = atol(seq); load_bg(); changed |= cfg.bg == ELK_BG_IMAGE; }
        prev = cfg;
        /* eink-round5: a system suspend since the previous loop (CLOCK_MONOTONIC stops in s2idle, CLOCK_BOOTTIME does not) */
        double gap = boottime() - mono(); int resumed = gap - gap_prev > 0.5; gap_prev = gap;
        if (resumed && !strncmp(st, "eink", 4) && !sm.locked && sm.asleep_since > 0)
            LOG("system suspended before the lock entry (state %s): entry delay restarted", st);
        /* eink-round11 reader sleep (a6l_dualux put Android to sleep between pages): the e-ink keeps the page - no lock
         * picture, no minute ticks, no restore. A lock already shown (not a reader sleep) is handled as before. */
        { char rs[8] = ""; static int in_reader; prop_get("vendor.dualux.reader_sleep", rs, sizeof rs);
          int reader = !strcmp(rs, "1") && !strcmp(st, "eink-asleep") && !sm.locked;
          if (reader != in_reader) { in_reader = reader; KLOG("%s", reader ? "reader sleep: the page stays on the e-ink (no lock picture)" : "reader sleep over"); }
          if (reader) { sm.asleep_since = 0; if (armed) timer_disarm(); if (holding) { wakelock(0); holding = 0; } continue; } }
        struct elk_in in = {cfg.enabled, cfg.clock, !strcmp(st, "eink-asleep"), !strncmp(st, "eink", 4), fired, changed, cfg.clean_min, mono(), resumed, lcd_idle, settling,
                            !strcmp(em, "mirror")};
        int was_locked = sm.locked;
        struct elk_act a = elk_step(&sm, &in);
        if (a.hold && !holding) { wakelock(1); holding = 1;
            if (in.asleep_eink) KLOG("asleep on the e-ink: lock entry in %.1f s (wakelock held)", ELK_ENTRY_DELAY_S);
            else KLOG("LCD mode, e-ink not in use: lock entry in %.1f s (wakelock held)", ELK_LCD_ENTRY_DELAY_S); }
        if (!was_locked && sm.locked) LOG("%s: lock screen on", in.asleep_eink ? "Android asleep on the e-ink" : "LCD mode, e-ink not in use");
        if (a.restore) {	/* awake on the e-ink: the mirror resumes, keep the CRTC up; still asleep (lock disabled): switch it off */
            char reply[256]; wakelock(1);
            int rc = transact(!strcmp(st, "eink") ? "lock restore" : "lock restore off", NULL, 0, reply, sizeof reply); wakelock(0); holding = 0;
            KLOG("lock screen off (%s): a6l_epdd: %s", strcmp(st, "eink") ? st[0] ? st : "no state" : "awake on the e-ink", reply[0] ? reply : "no reply");
            if (rc) elk_restore_failed(&sm, mono());
        }
        if (a.mirror_redraw)	/* eink-round10 */
            KLOG("lock screen off (%s): no restore, the mirror's first frame replaces the lock picture (GC16 clean in a6l_epdd)", st[0] ? st : "no state");
        if (a.draw) {
            char st2[32], em2[16]; prop_get("vendor.dualux.state", st2, sizeof st2);	/* eink-round5: re-checked right before the frame */
            prop_get("persist.vendor.eink.mode", em2, sizeof em2);
            int still = !strcmp(st2, "eink-asleep") || (in.lcd_idle && !strcmp(st2, "lcd") && strcmp(em2, "mirror"));	/* eink-round6 */
            if (!still) { KLOG("state %s just before the lock frame: not drawn (no e-ink modeset during a display transition)", st2[0] ? st2 : "empty"); if (!was_locked) elk_sm_abort(&sm); }
            else { wakelock(1); draw(&cfg, a.force, !was_locked ? (in.asleep_eink ? "lock entry" : "lock entry (LCD mode)") : fired ? "minute" : "settings changed / deferred"); elk_sm_sent(&sm, &a, mono()); }
            wakelock(0); holding = 0;
        } else if (holding && !a.hold) { wakelock(0); holding = 0; }
        if (a.arm) { if (!armed) timer_arm(elk_next_target(realtime(), period_s, lead_ms / 1000.0)); }
        else if (armed) timer_disarm();
    }
    LOG("exit");
    return 0;
}
