// SPDX-License-Identifier: Apache-2.0
/* a6l_eink_mirror — Hisense A6L rear e-ink controller for the installed ROM (v2, agent eink3, 24 Sep 2026).
 * Base: device/hisense/a6l/diagnostic/a6l_eink_mirror.c (M1: capture + resample + tile diff + policy).
 *
 * One vendor service that owns everything user-facing about the rear screen:
 *  - MODE (persist.vendor.eink.mode = off | mirror), toggled by the e-ink side key (KEY code 616, gpio-keys
 *    "A6L side keys"): short press = mirror on/off, long press (>= 800 ms) = stock-style ghost refresh.
 *  - MIRROR: captures the front screen, converts it to the 720x1440 portrait e-ink picture and drives a6l_epdd over its
 *    control socket with the automatic waveform policy of eink_logic.c (quality on discrete changes, A2 bursts while
 *    scrolling, one clean update when it settles, periodic clears); READING (persist.vendor.eink.reading = 1): no A2,
 *    REGAL partial updates when the page is still, GC16 on page turns, periodic forced refresh.
 *    Front screen off (LCD CRTC inactive) -> pause (the e-paper keeps the last picture).
 *  - REAR TOUCH (ft5x06 on blsp_i2c7, 720x1440): always grabbed (EVIOCGRAB) so Android never sees raw rear touches.
 *    Mirror on: contacts are mapped onto the mirrored picture and re-injected on a uinput touchscreen
 *    "a6l-eink-rear-touch" sized like the front display (idc: internal, display 0). Mirror off: dropped.
 *    eink-round10: when forwarding starts (mirror ON, front screen back on) contacts already down are ignored until
 *    lifted; after mirror ON new contacts are also held until the e-ink shows its first page (or --touch-guard-ms).
 *
 * Capture sources (--source):
 *   drm (default, ROM): LCD CRTC planes via GETFB2 + PRIME mmap (CAP_SYS_ADMIN, never DRM master; linear buffers only).
 *       The card is opened ONCE, after the composer is running, and master is dropped at once (see a6l_epdd --no-master
 *       for why a stray master would break the composer).
 *   screencap: /system/bin/screencap in SurfaceFlinger's mount namespace (RAM session, root only).
 *   file:PATH / files:PATTERN: tests.
 * a6l_epdd connection: --epd-socket PATH (default /dev/socket/a6l_epd); pixels are sent inline ("frame 720 1440 MODE").
 *
 * usage: a6l_eink_mirror [--source S] [--epd-socket P] [--interval ms] [--quiet ms] [--settle ms] [--min-gap ms]
 *        [--clear-every N] [--max-per-min N] [--active-mode M] [--fit letterbox|crop|stretch] [--threshold N] [--frames N]
 *        [--mode off|mirror] [--reading 0|1] [--no-props] [--key-dev auto|PATH|none] [--touch-dev auto|PATH|none]
 *        [--touch-transform T] [--front WxH] [--wait-prop NAME=VALUE] [--dry] [--ns-pid N] [--screencap P] [--out DIR]
 *        [--touch-debug] (print raw rear contacts "TOUCH_IN" and every injected event "TOUCH_OUT type code value")
 *        [--touch-guard-ms N] (eink-round10: after mirror ON, rear contacts that begin before the first page is shown on
 *        the e-ink are not forwarded, at most N ms; default 2000, 0 = off; sys.a6l.eink.touch_guard_ms overrides it)
 *        [--release-quiet ms] [--release-max ms] (after a held touch drag is released: wait for one unchanged capture
 *        pair, at most release-max ms, so the settled page is sent instead of the last drag position; 90 / 700)
 *        [--tone B,W,G] (grey tone curve before quantisation: black clip, white clip, gamma x100; default 24,232,150;
 *        0,255,100 = linear; persist.sys.a6l.eink.contrast still widens the black/white points live)
 *        [--reply-timeout ms] (a6l_epdd reply deadline, default 30000; also bounds socket writes)
 *        [--copy-guard-kib N] (eink-round3: drm source copies each plane in N KiB chunks and re-reads the plane set between
 *        chunks, so an A->B->A buffer flip during the copy discards the torn capture; default 1024; eink-round5: 0 = 1024,
 *        the check can no longer be switched off; a plane that flips during every copy is stitched, see plane_copy_policy)
 *        [--guard-max N,MS] (eink-round4: a capture whose LCD plane layout changed, or whose plane kept flipping during its
 *        own copy, is discarded at most N times in a row / for MS since the first discard, then the newest capture is
 *        accepted; default 3,400; 0,0 = never discard. Content flips of OTHER planes no longer discard anything)
 *        [--pipeline 0|1] (eink-round4, default 1: the next page is captured while a6l_epdd still drives the current
 *        update, timed to end at its reply, and sent at the reply; 0 = capture only after the reply)
 *        [--keep-crtc-front-off] (eink-round3: do NOT send "power off" when the LCD CRTC goes off; by default the e-ink
 *        CRTC follows the LCD CRTC so it is never enabled across a system suspend)
 *   live properties (dualux, 25 Sep): persist.sys.a6l.eink.refresh (auto|quality|partial|fast|fastest, overrides .reading),
 *        persist.sys.a6l.eink.clear_every, vendor.eink.clear_req (any change = ghost refresh; written by a6l_dualux),
 *        persist.sys.a6l.eink.contrast (0..100, black/white point stretch before the e-ink quantisation),
 *        persist.sys.a6l.eink.invert (eink-round11: 1 = inverted rendering, for a dark LCD theme shown without the e-ink
 *        theme change; eink_logic.h tone_lut_invert)
 *   In the ROM the e-ink key is handled by a6l_dualux (rc: --key-dev none); --key-dev auto keeps the old toggle behaviour.
 *        a6l_eink_mirror [--epd-socket P] --send "<command>"   one command to a6l_epdd, prints its reply line
 *   --dry: no socket: prints "A6L_MIRROR CMD ..." lines and simulates completion; with --out it also writes the PGMs.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef NO_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#ifdef A6L_ANDROID_LOG
#include <android/log.h>
#endif
#include "eink_logic.h"
#ifndef NO_DRM
#include "capture_plane_snapshot.h"
#endif

#define OW 720
#define OH 1440
#define TS 8
#define MAXRAW (64u << 20)
#define KEY_EINK 616

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#ifdef A6L_ANDROID_LOG
/* Match Java elapsedRealtime across suspend; scheduling still uses now(). */
static double boot_now(void) { struct timespec t; if (clock_gettime(CLOCK_BOOTTIME, &t)) return now(); return t.tv_sec + t.tv_nsec / 1e9; }
#endif
static double t_start;
static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("[%.3f] A6L_MIRROR %s\n", now() - t_start, b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    char stamped[sizeof b + 128];
    snprintf(stamped, sizeof stamped, "boot_ms=%.3f mono_ms=%.3f %s", boot_now() * 1000, now() * 1000, b);
    __android_log_write(strstr(b, "FAIL") ? ANDROID_LOG_ERROR : strstr(b, "WARN") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "a6l_eink", stamped);
#endif
}
#define LOG(...) logline(__VA_ARGS__)
/* eink-round5: display-transition markers in the kernel log ("<6>a6l_eink: ..."; printk.devkmsg=on). eink-round6: /dev/kmsg is
 * 0600 root:root (first-stage init), so a system daemon cannot open it: /dev/kmsg_debug (0622, userdebug/eng) instead.
 * One timeline with the DPU/DSI/SMMU/PM messages for the round-5 repro (kmsg streamed to the laptop / fsync'ed on the
 * phone). Android builds only: host tests never write the host's kernel log. */
static void kmark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void kmark(const char *fmt, ...) {
#ifdef __ANDROID__
    static int fd = -2;
    if (fd == -2) { fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC); if (fd < 0) fd = open("/dev/kmsg_debug", O_WRONLY | O_CLOEXEC); }
    if (fd < 0) return;
    char b[300]; int n = snprintf(b, sizeof b, "<6>a6l_eink: "); va_list ap; va_start(ap, fmt);
    int m = vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap); va_end(ap);
    if (m < 0) return;
    n += m; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n'; if (write(fd, b, (size_t)n) < 0) { /* best effort */ }
#else
    (void)fmt;
#endif
}
#define KLOG(...) do { LOG(__VA_ARGS__); kmark(__VA_ARGS__); } while (0)

static const char *epd_socket = "/dev/socket/a6l_epd", *outdir, *source = "drm", *screencap = "/system/bin/screencap", *display_id,
                  *key_dev = "auto", *touch_dev = "auto", *wait_prop;
static int interval_ms = 250, fit, threshold = 6, frames_max, dry, ns_pid = -1, use_props = 1, front_w = 1080, front_h = 2340;
static struct pol_cfg pcfg;
static int auto_small_regal; /* explicit trial flag; no service/property default */
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

/* ---------------- properties ---------------- */
static int prop_get(const char *k, char *v, size_t n) {
#ifdef __ANDROID__
    char b[PROP_VALUE_MAX] = {0}; int l = __system_property_get(k, b); snprintf(v, n, "%s", b); return l;
#else
    const char *dir = getenv("A6L_PROP_DIR");	/* eink-round11 host tests: one file per property, re-read every time */
    if (dir) { char p[512]; snprintf(p, sizeof p, "%s/%s", dir, k); FILE *f = fopen(p, "r"); v[0] = 0;
        if (f) { if (fgets(v, (int)n, f)) v[strcspn(v, "\n")] = 0; fclose(f); return (int)strlen(v); } }
    const char *e = getenv(k); snprintf(v, n, "%s", e ? e : ""); return (int)strlen(v);
#endif
}
static void prop_set(const char *k, const char *v) {
#ifdef __ANDROID__
    if (__system_property_set(k, v)) LOG("WARN setprop %s=%s refused (SELinux?)", k, v);
#else
    setenv(k, v, 1);
#endif
}
static int prop_int(const char *k, int def) { char v[96]; return prop_get(k, v, sizeof v) > 0 ? atoi(v) : def; }

/* Tag only pixels captured after Java committed this exact appearance. A new
 * request arriving while an older frame is in flight must not inherit its ACK. */
static void appearance_snapshot(char *v, size_t n) {
    char ready[96]; v[0] = 0;
    if (!use_props) return;
    prop_get("vendor.dualux.prepare", v, n);
    prop_get("sys.a6l.dualux.ready", ready, sizeof ready);
    size_t len = strlen(v);
    if (len < 6 || strcmp(v + len - 5, " eink") || strcmp(v, ready)) v[0] = 0;
}

/* Normal Auto waits for a second moving capture before starting a burst.
 * A new switch already has an exact appearance/redraw ACK and passed the
 * scanout gate before its private capture. Do not recapture just to reach
 * consec==2: submit that first eligible page with Auto's normal burst waveform.
 * Keep explicit quality/reading settle and every normal rate/queue guard. */
/* eink-round4: the capture's motion was already recorded (pol_observe) when it was taken, possibly during the previous
 * update (pipelined capture); this only decides. */
static struct pol_action appearance_policy_decide(struct pol_state *ps, double t, double vs_panel,
                                                  int busy_now, const char *captured) {
    struct pol_action a = pol_decide(ps, t, vs_panel, busy_now);
    if (a.kind != POL_NONE || !captured[0] || ps->cfg.reading || ps->cfg.fixed_fast ||
        busy_now || vs_panel <= 0 || (t - ps->done_t) * 1000 < ps->cfg.min_gap_ms ||
        ps->win_n >= ps->cfg.max_per_min) return a;
    char current[96], panel_ready[96];
    appearance_snapshot(current, sizeof current);
    prop_get("vendor.eink.ready", panel_ready, sizeof panel_ready);
    if (strcmp(current, captured) || !strcmp(panel_ready, captured)) return a;
    a.kind = POL_SHOW; a.mode = ps->cfg.active_mode;
    LOG("appearance %s: first eligible Auto frame, no settle recapture", captured);
    return a;
}

/* ---------------- gray canvas (front screen, full resolution) ---------------- */
static uint8_t *gray; static int gw, gh;
static int gray_alloc(int w, int h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    if (w != gw || h != gh) { free(gray); gray = malloc((size_t)w * h); if (!gray) return -1; gw = w; gh = h; }
    return 0;
}
static inline uint8_t luma(int r, int g, int b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }

/* ---------------- source: screencap raw (RAM session) ---------------- */
static char **sf_env; static int sf_pid_cached = -1;
static int find_pid(const char *comm) {
    DIR *d = opendir("/proc"); struct dirent *e; int found = -1;
    while (d && (e = readdir(d))) { int pid = atoi(e->d_name); if (pid <= 0) continue; char p[64], c[64] = {0};
        snprintf(p, sizeof p, "/proc/%d/comm", pid); FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(c, sizeof c, f)) { char *nl = strchr(c, '\n'); if (nl) *nl = 0; if (!strcmp(c, comm)) found = pid; } fclose(f); if (found > 0) break; }
    if (d) closedir(d); return found;
}
static char **read_environ(int pid) {
    char p[64]; snprintf(p, sizeof p, "/proc/%d/environ", pid); FILE *f = fopen(p, "rb"); if (!f) return NULL;
    static char buf[65536]; size_t n = fread(buf, 1, sizeof buf - 2, f); fclose(f); buf[n] = 0; buf[n + 1] = 0;
    int cnt = 0; for (size_t i = 0; i < n; i++) if (!buf[i]) cnt++;
    char **env = calloc((size_t)cnt + 2, sizeof *env); int k = 0; if (!env) return NULL;
    for (size_t i = 0; i < n && k < cnt + 1; ) { size_t l = strlen(buf + i); if (l) env[k++] = buf + i; i += l + 1; }
    env[k] = NULL; return env;
}
static uint8_t *raw; static size_t raw_len;
static int read_all(int fd) {
    if (!raw && !(raw = malloc(MAXRAW))) return -1; raw_len = 0; ssize_t r;
    while ((r = read(fd, raw + raw_len, MAXRAW - raw_len)) > 0) { raw_len += (size_t)r; if (raw_len == MAXRAW) break; }
    return r < 0 ? -1 : 0;
}
static int parse_raw(void) {	/* raw screencap: u32 w, h, format [, dataspace] then w*h*bpp bytes (stride removed) */
    if (raw_len < 16) { LOG("FAIL screencap output too short (%zu bytes)", raw_len); return -1; }
    uint32_t w, h, f; memcpy(&w, raw, 4); memcpy(&h, raw + 4, 4); memcpy(&f, raw + 8, 4);
    int bpp = (f == 1 || f == 2 || f == 5) ? 4 : f == 3 ? 3 : f == 4 ? 2 : 0;
    if (!bpp || w == 0 || h == 0 || w > 8192 || h > 8192) { LOG("FAIL screencap header w=%u h=%u format=%u", w, h, f); return -1; }
    size_t px = (size_t)w * h * bpp; if (raw_len < px + 12) { LOG("FAIL screencap short: %zu < %zu", raw_len, px + 12); return -1; }
    size_t hdr = raw_len - px; if (hdr != 12 && hdr != 16) { LOG("WARN screencap header %zu bytes (expected 12/16)", hdr); if (hdr > 64) return -1; }
    if (gray_alloc((int)w, (int)h)) return -1;
    const uint8_t *s = raw + hdr;
    for (size_t i = 0; i < (size_t)w * h; i++, s += bpp) {
        if (f == 5) gray[i] = luma(s[2], s[1], s[0]);
        else if (bpp == 4 || bpp == 3) gray[i] = luma(s[0], s[1], s[2]);
        else { uint16_t v = (uint16_t)(s[0] | s[1] << 8); gray[i] = luma((v >> 11) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3); }
    }
    return 0;
}
static int capture_screencap(void) {
    if (ns_pid == 0) sf_pid_cached = 0;
    else if (ns_pid > 0) sf_pid_cached = ns_pid;
    else { if (sf_pid_cached > 0) { char p[64]; snprintf(p, sizeof p, "/proc/%d/ns/mnt", sf_pid_cached); if (access(p, F_OK)) { sf_pid_cached = -1; free(sf_env); sf_env = NULL; } }
        if (sf_pid_cached <= 0) { sf_pid_cached = find_pid("surfaceflinger"); if (sf_pid_cached <= 0) { LOG("WARN surfaceflinger not running"); return -1; } LOG("surfaceflinger pid %d", sf_pid_cached); } }
    if (sf_pid_cached > 0 && !sf_env) sf_env = read_environ(sf_pid_cached);
    int pp[2]; if (pipe2(pp, O_CLOEXEC)) return -1;
    pid_t c = fork(); if (c < 0) { close(pp[0]); close(pp[1]); return -1; }
    if (!c) {
        if (sf_pid_cached > 0) { char p[64]; snprintf(p, sizeof p, "/proc/%d/ns/mnt", sf_pid_cached); int nf = open(p, O_RDONLY | O_CLOEXEC);
            if (nf < 0 || setns(nf, CLONE_NEWNS)) { dprintf(2, "A6L_MIRROR setns %s: %s\n", p, strerror(errno)); _exit(97); } if (chdir("/")) _exit(96); }
        dup2(pp[1], 1); int dn = open("/dev/null", O_WRONLY); if (dn >= 0) dup2(dn, 2);
        char *args[5] = {(char *)screencap, NULL, NULL, NULL, NULL}; if (display_id) { args[1] = "-d"; args[2] = (char *)display_id; }
        static char *empty[] = {"PATH=/system/bin:/system/xbin:/vendor/bin", NULL};
        execve(screencap, args, sf_env ? sf_env : empty); _exit(98);
    }
    close(pp[1]); int r = read_all(pp[0]); close(pp[0]); int st = 0; waitpid(c, &st, 0);
    if (r || !WIFEXITED(st) || WEXITSTATUS(st)) { LOG("WARN screencap failed (status 0x%x, %zu bytes)", st, raw_len); return -1; }
    return parse_raw();
}
static int capture_file(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC); if (fd < 0) { LOG("FAIL open %s: %s", path, strerror(errno)); return -1; }
    int r = read_all(fd); close(fd); return r ? -1 : parse_raw();
}

/* ---------------- source: DRM planes of the LCD CRTC (ROM) ---------------- */
#define CAP_FRONT_OFF 1	/* capture result: the LCD CRTC is inactive (screen off) */
#define CAP_TORN 2	/* eink-round3: the plane set changed during the copy: discarded, not a failure (no 40-failure pause) */
static size_t guard_chunk = GUARDED_COPY_CHUNK;	/* --copy-guard-kib N; eink-round5: 0 = the default 1 MiB (the guard can no longer be switched off) */
static int keep_crtc_front_off;			/* --keep-crtc-front-off: old behaviour (e-ink CRTC stays on) */
static struct cap_guard cguard;			/* eink-round4: discard bound, the mirror never starves */
static int guard_max_discards = CAP_GUARD_MAX_DISCARDS, guard_max_ms = CAP_GUARD_MAX_MS;	/* --guard-max N,MS */
static struct front_follow front_ff;
#ifndef NO_DRM
static int drm_fd = -1;
static uint64_t prop_of(uint32_t obj, uint32_t type, const char *name, uint64_t def) {
    drmModeObjectProperties *pp = drmModeObjectGetProperties(drm_fd, obj, type); uint64_t v = def;
    for (unsigned j = 0; pp && j < pp->count_props; j++) { drmModePropertyRes *p = drmModeGetProperty(drm_fd, pp->props[j]);
        if (p && !strcmp(p->name, name)) v = pp->prop_values[j]; drmModeFreeProperty(p); }
    drmModeFreeObjectProperties(pp); return v;
}
static int drm_open_once(void) {	/* the KMS card with a non-e-ink connector; opened once and kept (see header) */
    for (int c = 0; c < 4 && drm_fd < 0; c++) {
        char path[32]; snprintf(path, sizeof path, "/dev/dri/card%d", c); int fd = open(path, O_RDWR | O_CLOEXEC); if (fd < 0) continue;
        if (drmIsMaster(fd)) { drmDropMaster(fd); LOG("WARN %s: we were DRM master (no composer?) -> dropped", path); }
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        drmModeRes *res = drmModeGetResources(fd); int lcd = 0;
        for (int i = 0; res && i < res->count_connectors && !lcd; i++) { drmModeConnector *k = drmModeGetConnector(fd, res->connectors[i]);
            if (k && k->connector_type != DRM_MODE_CONNECTOR_WRITEBACK && k->count_modes && k->modes[0].hdisplay != 384) lcd = 1; drmModeFreeConnector(k); }
        if (res) drmModeFreeResources(res);
        if (lcd) { drm_fd = fd; LOG("DRM source: %s", path); } else close(fd);
    }
    return drm_fd >= 0 ? 0 : -1;
}
static int lcd_crtc(uint32_t *crtc_id, int *w, int *h) {	/* active CRTC driving a non-e-ink connector; 0 = none active */
    drmModeRes *res = drmModeGetResources(drm_fd); int found = 0;
    for (int i = 0; res && i < res->count_connectors && !found; i++) { drmModeConnector *k = drmModeGetConnector(drm_fd, res->connectors[i]);
        if (k && k->connection == DRM_MODE_CONNECTED && k->count_modes && k->modes[0].hdisplay != 384 && k->encoder_id) {
            drmModeEncoder *e = drmModeGetEncoder(drm_fd, k->encoder_id); if (e && e->crtc_id) { drmModeCrtc *cr = drmModeGetCrtc(drm_fd, e->crtc_id);
                if (cr && cr->mode_valid && prop_of(e->crtc_id, DRM_MODE_OBJECT_CRTC, "ACTIVE", 1)) { *crtc_id = e->crtc_id; *w = cr->mode.hdisplay; *h = cr->mode.vdisplay; found = 1; }
                drmModeFreeCrtc(cr); } drmModeFreeEncoder(e); }
        drmModeFreeConnector(k); }
    if (res) drmModeFreeResources(res);
    return found;
}
struct pl { uint32_t fb; int64_t zpos; int cx, cy, cw, ch; double sx, sy, sw, sh; uint32_t rot, alpha; int blend; unsigned ri; };	/* F41: + rotation/alpha/blend; round4: + plane resource index */
static int cmp_pl(const void *a, const void *b) { const struct pl *x = a, *y = b; return x->zpos < y->zpos ? -1 : x->zpos > y->zpos; }
static int drm_allowed(void) {	/* --wait-prop NAME=VALUE: the card is only opened once the composer surely holds master */
    if (!wait_prop) return 1;
    const char *eq = strchr(wait_prop, '='); if (!eq) return 1;
    char k[96], v[96]; snprintf(k, sizeof k, "%.*s", (int)(eq - wait_prop), wait_prop); prop_get(k, v, sizeof v);
    if (strcmp(v, eq + 1)) { static double last; if (now() - last > 30) { last = now(); LOG("drm source: waiting for %s (now '%s')", wait_prop, v); } return 0; }
    return 1;
}
static uint8_t *scanout_copy;
static size_t scanout_copy_n;
static int dma_read_sync(int fd, uint64_t flags) {
    struct dma_buf_sync sy = {flags}; int rc;
    do { rc = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sy); } while (rc && (errno == EINTR || errno == EAGAIN) && !stop);
    return rc;
}
/* eink-round4: each plane's copy is guarded by THAT plane's tuple only (FB id + geometry, re-read between chunks). A
 * flip of another plane (status bar, navigation bar, a second app layer) during the copy does not tear this plane's
 * pixels; it was or will be copied consistently on its own. Round 3 compared the whole plane set and discarded the
 * capture whenever any plane flipped: 6 Oct 17:01, "changed plane 2" (the 1080x75 status bar layer) discarded 75
 * captures in 15.4 s and 94 in 45.5 s, so a swipe back and a pulled shade never reached the e-ink.
 * eink-round5: the copy itself is plane_copy_policy() (eink_logic.c, host-tested with a 60 Hz drawer model): the check
 * can no longer be switched off (round 4 with copy_guard_kib=0 made none: the 6 Oct 21:18 drawer artefacts), and a plane
 * that flips during its last retry is STITCHED from the buffers it shows (clean seams between consecutive frames)
 * instead of being finished torn from a buffer the producer is re-rendering. */
/* eink-round4: last consistent copy of each plane (by resource index), the fallback for a plane that flips during every
 * copy of it. Up to one full-screen buffer per plane in use (~10 MB each); swapped with the scratch copy, never copied. */
struct plane_cache { uint8_t *buf; size_t cap; int valid; uint32_t id, fmt, w, h, pitch; double t; struct pl geo; };	/* round5: + geometry */
static struct plane_cache pcache[16];
static unsigned capture_discards; static double capture_discard_since;	/* consecutive discarded captures (logging) */
static void capture_discarded(const char *why, int plane, int quiet) {
    if (!capture_discards++) capture_discard_since = now();
    if ((!quiet && capture_discards == 1) || capture_discards % 25 == 0)
        LOG("capture discarded (%s; plane %d): %u in a row over %.1f s", why, plane, capture_discards, now() - capture_discard_since);
}
static void pl_fill(struct pl *q, const struct a6l_object_tuple *p, unsigned ri) {
    const uint64_t *v = p->v;
    q->fb = (uint32_t)v[PF_FB]; q->zpos = (int64_t)v[PF_ZPOS];
    q->cx = (int)(int32_t)v[PF_CX]; q->cy = (int)(int32_t)v[PF_CY]; q->cw = (int)v[PF_CW]; q->ch = (int)v[PF_CH];
    q->sx = v[PF_SX] / 65536.0; q->sy = v[PF_SY] / 65536.0; q->sw = v[PF_SW] / 65536.0; q->sh = v[PF_SH] / 65536.0;
    q->rot = (uint32_t)v[PF_ROT]; q->alpha = (uint32_t)v[PF_ALPHA]; q->blend = (int)v[PF_BLEND]; q->ri = ri;
}
static int pl_same_geometry(const struct pl *a, const struct pl *b) {
    return a->zpos == b->zpos && a->cx == b->cx && a->cy == b->cy && a->cw == b->cw && a->ch == b->ch && a->sx == b->sx &&
           a->sy == b->sy && a->sw == b->sw && a->sh == b->sh && a->rot == b->rot && a->alpha == b->alpha && a->blend == b->blend;
}
static void gem_close_fb(drmModeFB2 *fb) {	/* GETFB2 creates GEM handles: close each distinct one */
    for (int h = 0; h < 4; h++) if (fb->handles[h]) { int dup = 0; for (int g = 0; g < h; g++) if (fb->handles[g] == fb->handles[h]) dup = 1;
        if (!dup) { struct drm_gem_close gc = {.handle = fb->handles[h]}; drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc); } }
}
/* eink-round5: the buffer side of plane_copy_policy(): map the buffer the plane shows NOW (GETFB2 + PRIME + mmap +
 * dma-buf read sync) and re-read the plane's own tuple between chunks. The buffer layout (format, size, pitch, offset,
 * modifier) must stay the one validated by capture_plane(); a new geometry (position, size, z-order, alpha, blend)
 * restarts the copy and marks the capture as a layout change (discarded unless forced by the never-starve rule). */
struct drm_pcopy {
    struct pl *q, geo; uint32_t plane, crtc; unsigned ri;
    uint32_t fmt, w, h, pitch, off; uint64_t mod;
    drmModeFB2 *fb; int dmafd, synced, layout_changed, err; uint8_t *map; size_t len; double *sync_ms;
};
static void dpc_close(void *p) {
    struct drm_pcopy *d = p;
    if (d->synced && dma_read_sync(d->dmafd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ)) LOG("WARN dma-buf read end sync: %s", strerror(errno));
    d->synced = 0;
    if (d->map) munmap(d->map, d->len);
    d->map = NULL;
    if (d->dmafd >= 0) close(d->dmafd);
    d->dmafd = -1;
    if (d->fb) { gem_close_fb(d->fb); drmModeFreeFB2(d->fb); d->fb = NULL; }
}
static int dpc_open(void *p, const uint8_t **px, uint32_t *gen) {
    struct drm_pcopy *d = p; struct a6l_object_tuple t; struct pl fresh;
    if (a6l_plane_tuple_read(drm_fd, d->plane, d->ri, &t)) { d->err = errno; return -1; }
    if (!a6l_plane_on_crtc(&t, d->crtc)) { d->layout_changed = 1; return 1; }	/* the plane left the LCD: skip it */
    pl_fill(&fresh, &t, d->ri);
    int changed = !pl_same_geometry(&fresh, &d->geo);
    drmModeFB2 *fb = drmModeGetFB2(drm_fd, fresh.fb);
    if (!fb) { d->err = errno; return -1; }
    uint64_t mod = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : DRM_FORMAT_MOD_LINEAR;
    if (fb->pixel_format != d->fmt || fb->width != d->w || fb->height != d->h || fb->pitches[0] != d->pitch ||
        fb->offsets[0] != d->off || mod != d->mod || !fb->handles[0]) {	/* another buffer layout: not this picture */
        gem_close_fb(fb); drmModeFreeFB2(fb); d->layout_changed = 1; return 1;
    }
    d->fb = fb; d->len = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
    if (drmPrimeHandleToFD(drm_fd, fb->handles[0], DRM_CLOEXEC, &d->dmafd)) { d->err = errno; d->dmafd = -1; dpc_close(d); return -1; }
    uint8_t *m = mmap(NULL, d->len, PROT_READ, MAP_SHARED, d->dmafd, 0);
    if (m == MAP_FAILED) { d->err = errno; LOG("WARN prime map fb %u: %s", fresh.fb, strerror(errno)); dpc_close(d); return -1; }
    d->map = m;
    double ts = now(); d->synced = !dma_read_sync(d->dmafd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ); *d->sync_ms += (now() - ts) * 1000;
    if (!d->synced) { d->err = errno; LOG("WARN dma-buf read sync: %s", strerror(errno)); dpc_close(d); return -1; }
    if (changed) { d->layout_changed = 1; d->geo = fresh; }
    *d->q = fresh;	/* composed with the geometry of the buffer actually copied */
    *px = d->map + fb->offsets[0]; *gen = fresh.fb;
    return changed ? 2 : 0;
}
static int dpc_still(void *p, uint32_t gen) {
    struct drm_pcopy *d = p; struct a6l_object_tuple t; struct pl fresh;
    if (a6l_plane_tuple_read(drm_fd, d->plane, d->ri, &t)) { d->err = errno; return -1; }
    if (!a6l_plane_on_crtc(&t, d->crtc) || (uint32_t)t.v[PF_FB] != gen) return 0;
    pl_fill(&fresh, &t, d->ri);
    return pl_same_geometry(&fresh, &d->geo);
}
static const struct pcopy_ops dpc_ops = {dpc_open, dpc_close, dpc_still};
/* Copy + compose one plane (eink-round5: plane_copy_policy). Returns 0 = composed, *torn = 0 consistent, 1 stitched from
 * consecutive buffers, 2 its last consistent copy (<= 0.5 s), 3 torn (> 12 flips during one copy); 1 = the plane left the
 * LCD or changed its buffer layout (not composed: layout change); -1 = error. */
static int capture_plane(struct pl *q, uint32_t plane_id, uint32_t crtc, int k, int *torn, int *layout_changed,
                         double *sync_ms, double *copy_ms, double *compose_ms, int *checks, int *switches) {
    drmModeFB2 *fb = drmModeGetFB2(drm_fd, q->fb); if (!fb) { LOG("WARN GETFB2 %u: %s", q->fb, strerror(errno)); return -1; }
    int ok = 1, bpp = 0, alpha = 0, bgr = 0; uint32_t f = fb->pixel_format;
    if (f == DRM_FORMAT_XRGB8888 || f == DRM_FORMAT_ARGB8888) { bpp = 4; alpha = f == DRM_FORMAT_ARGB8888; bgr = 1; }
    else if (f == DRM_FORMAT_XBGR8888 || f == DRM_FORMAT_ABGR8888) { bpp = 4; alpha = f == DRM_FORMAT_ABGR8888; }
    else if (f == DRM_FORMAT_RGB565) bpp = 2;
    if (!bpp) { static int w1; if (!w1++) LOG("WARN plane fb format %.4s unsupported", (char *)&f); ok = 0; }
    if ((fb->flags & DRM_MODE_FB_MODIFIERS) && fb->modifier != DRM_FORMAT_MOD_LINEAR) { static int warned; if (!warned++) LOG("WARN fb modifier 0x%llx (compressed/tiled): drm source cannot read it", (unsigned long long)fb->modifier); ok = 0; }
    struct plane_geo geo = {q->cx, q->cy, q->cw, q->ch, q->sx, q->sy, q->sw, q->sh, q->rot, q->alpha, q->blend};
    /* Log layout changes, not every buffer swap: next attended trace can
     * establish whether the identity/alpha fast path covers the real page. */
    { static char layouts[16][240]; char layout[240];
      snprintf(layout, sizeof layout, "%.4s fb=%ux%u dst=%d,%d %dx%d src=%.2f,%.2f %.2fx%.2f rot=%u alpha=%u blend=%d", (char *)&f, fb->width, fb->height, q->cx, q->cy, q->cw, q->ch, q->sx, q->sy, q->sw, q->sh, q->rot, q->alpha, q->blend);
      if (k >= 0 && k < 16 && strcmp(layouts[k], layout)) { snprintf(layouts[k], sizeof layouts[k], "%s", layout); LOG("plane %d layout: %s", k, layout); }
    }
    if (plane_supported(&geo)) { static int w3; if (!w3++) LOG("WARN plane rotation 0x%x / alpha 0x%x / blend mode %d unsupported: capture refused", q->rot, q->alpha, q->blend); ok = 0; }
    if (!fb->handles[0]) { static int w2; if (!w2++) LOG("WARN GETFB2 returned no handle (needs CAP_SYS_ADMIN)"); ok = 0; }
    size_t len = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
    if (len > MAXRAW || !fb->height || !fb->width || fb->pitches[0] < (uint64_t)fb->width * bpp) {
        LOG("WARN invalid/oversized scanout layout: %ux%u pitch=%u offset=%u", fb->width, fb->height, fb->pitches[0], fb->offsets[0]); ok = 0;
    }
    struct drm_pcopy d = {.q = q, .geo = *q, .plane = plane_id, .crtc = crtc, .ri = q->ri, .fmt = f, .w = fb->width, .h = fb->height,
                          .pitch = fb->pitches[0], .off = fb->offsets[0], .mod = (fb->flags & DRM_MODE_FB_MODIFIERS) ? fb->modifier : DRM_FORMAT_MOD_LINEAR,
                          .dmafd = -1, .sync_ms = sync_ms};
    gem_close_fb(fb);	/* GETFB2 creates GEM handles: the copy maps the buffer the plane shows when it starts */
    drmModeFreeFB2(fb);
    if (!ok) return -1;
    /* Scanout mappings can be uncached/write-combined. Bulk-copy once into normal CPU memory rather than issuing byte
     * loads for each luma/blend operation. No mapping or GEM handle survives capture. */
    size_t bytes = (size_t)d.pitch * d.h;
    if (bytes > scanout_copy_n) {
        uint8_t *p = realloc(scanout_copy, bytes);
        if (!p) return -1;
        scanout_copy = p; scanout_copy_n = bytes;
    }
    double ts = now(), s0 = *sync_ms; struct pcopy_stats st;
    int r = plane_copy_policy(scanout_copy, bytes, pcopy_chunk(guard_chunk, bytes), PCOPY_RETRIES, PCOPY_MAX_SWITCHES, &dpc_ops, &d, &st);
    *copy_ms += (now() - ts) * 1000 - (*sync_ms - s0);
    *checks += st.checks; *switches += st.switches;
    if (d.layout_changed) *layout_changed = 1;
    if (r == PCOPY_ERR) { LOG("WARN plane %d copy: tuple/buffer query failed: %s", k, strerror(d.err)); return -1; }
    if (r == PCOPY_GONE) return 1;
    const uint8_t *pixels = scanout_copy;
    struct plane_cache *pc = q->ri < 16 ? &pcache[q->ri] : NULL;
    if (r == PCOPY_OK) {	/* consistent: keep it as this plane's fallback (buffer swap, no copy) */
        *torn = 0;
        if (pc) {
            uint8_t *b = pc->buf; size_t c = pc->cap;
            pc->buf = scanout_copy; pc->cap = scanout_copy_n; scanout_copy = b; scanout_copy_n = c; pixels = pc->buf;
            pc->valid = 1; pc->id = q->fb; pc->fmt = d.fmt; pc->w = d.w; pc->h = d.h; pc->pitch = d.pitch; pc->t = now(); pc->geo = *q;
        }
    } else if (r == PCOPY_TORN && pc && pcache_usable(now() - pc->t, pc->valid && pc->fmt == d.fmt && pc->w == d.w && pc->h == d.h &&
                                                     pc->pitch == d.pitch && pl_same_geometry(&pc->geo, q))) {
        pixels = pc->buf; *torn = 2;	/* still flipping after 12 switches: its last consistent copy (<= 0.5 s), never a torn one */
    } else *torn = r == PCOPY_STITCHED ? 1 : 3;
    if (r != PCOPY_OK && (*torn == 3 || st.switches > 6)) LOG("WARN plane %d: %d buffer switches during one copy (%s)", k, st.switches, *torn == 3 ? "torn" : "stitched");
    /* F41: reflection, rotation, scaling and the plane's blend equation (eink_logic.c plane_compose, host-tested) */
    struct plane_geo g2 = {q->cx, q->cy, q->cw, q->ch, q->sx, q->sy, q->sw, q->sh, q->rot, q->alpha, q->blend};
    struct plane_fb pf = {pixels, d.pitch, (int)d.w, (int)d.h, bpp, bgr, alpha};
    ts = now();
    int rc = plane_compose(gray, gw, gh, &g2, &pf) ? -1 : 0;
    *compose_ms += (now() - ts) * 1000;
    return rc;
}
static int capture_drm(void) {
    double sync_ms = 0, copy_ms = 0, compose_ms = 0;
    static unsigned captures;
    if (drm_fd < 0 && !drm_allowed()) return CAP_FRONT_OFF;	/* treated like "front off": paused, not a failure */
    if (drm_fd < 0 && drm_open_once()) { LOG("WARN no KMS card with an LCD connector"); return -1; }
    uint32_t crtc = 0; int lw = 0, lh = 0;
    if (!lcd_crtc(&crtc, &lw, &lh)) return CAP_FRONT_OFF;
    struct a6l_plane_snapshot before, after;
    int sr=a6l_plane_snapshot_read(drm_fd,crtc,&before);
    if(sr>0)return CAP_FRONT_OFF;
    if(sr<0){LOG("WARN capture tuple required query failed: %s",strerror(errno));return -1;}
    lw=(int)before.width;lh=(int)before.height;
    uint64_t seq_before=0,seq_after=0,seq_ns;
    int seq_before_ok=!drmCrtcGetSequence(drm_fd,crtc,&seq_before,&seq_ns);
    if (lw != front_w || lh != front_h) { LOG("front display %dx%d", lw, lh); front_w = lw; front_h = lh; }
    if (gray_alloc(lw, lh)) return -1; memset(gray, 0, (size_t)gw * gh);
    struct pl pls[16]; int n=0;
    for(unsigned i=0;i<before.count;i++) {
        if(!a6l_plane_on_crtc(&before.planes[i],crtc))continue;
        pl_fill(&pls[n++],&before.planes[i],i);
    }
    if (!n) { LOG("WARN no plane on the LCD CRTC"); return -1; }
    qsort(pls, (size_t)n, sizeof pls[0], cmp_pl);
    int first = 0;
    /* Avoid CPU reads of completely hidden layers. Recheck actual FB bounds;
     * all visible planes still follow the normal validation and DMA sync path. */
    for (int k = n - 1; k > 0; k--) {
        struct pl *q = &pls[k];
        struct plane_geo geo = {q->cx, q->cy, q->cw, q->ch, q->sx, q->sy, q->sw, q->sh, q->rot, q->alpha, q->blend};
        if (!plane_opaque_fullscreen(&geo, lw, lh, lw, lh)) continue;
        drmModeFB2 *fb = drmModeGetFB2(drm_fd, q->fb);
        if (!fb) continue;
        uint32_t fmt = fb->pixel_format;
        int bpp = fmt == DRM_FORMAT_RGB565 ? 2 :
                  (fmt == DRM_FORMAT_XRGB8888 || fmt == DRM_FORMAT_ARGB8888 ||
                   fmt == DRM_FORMAT_XBGR8888 || fmt == DRM_FORMAT_ABGR8888) ? 4 : 0;
        size_t len = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
        int opaque = bpp && fb->handles[0] && fb->width && fb->height && len <= MAXRAW &&
                     fb->pitches[0] >= (uint64_t)fb->width * bpp &&
                     (!(fb->flags & DRM_MODE_FB_MODIFIERS) || fb->modifier == DRM_FORMAT_MOD_LINEAR) &&
                     plane_opaque_fullscreen(&geo, (int)fb->width, (int)fb->height, lw, lh);
        gem_close_fb(fb);	/* GETFB2 creates GEM handles even for this metadata-only probe. */
        drmModeFreeFB2(fb);
        if (opaque) { first = k; break; }
    }
    int layout_changed = 0, torn = 0, torn_plane = -1, switches = 0, checks = 0;
    for (int k = first; k < n; k++) {
        struct pl *q = &pls[k]; int t = 0;
        int r = capture_plane(q, before.planes[q->ri].id, crtc, k, &t, &layout_changed, &sync_ms, &copy_ms, &compose_ms, &checks, &switches);
        if (r < 0) return -1;
        if (r == 1) { layout_changed = 1; continue; }	/* the plane left the LCD / changed buffer layout: skipped */
        if (t) { torn = t > torn ? t : torn; torn_plane = k; }
    }
    uint32_t after_crtc=0;int after_w=0,after_h=0;
    int after_lcd=lcd_crtc(&after_crtc,&after_w,&after_h);
    int after_ok=after_lcd&&after_crtc==crtc?a6l_plane_snapshot_read(drm_fd,crtc,&after):-1;
    int after_errno=errno;
    int seq_after_ok=!drmCrtcGetSequence(drm_fd,crtc,&seq_after,&seq_ns);
    if (after_ok < 0 && after_lcd && after_crtc == crtc) {
        LOG("WARN capture end tuple query failed: %s", strerror(after_errno)); return -1;
    }
    if (after_ok || !a6l_plane_layout_equal(&before, &after, crtc)) layout_changed = 1;	/* LCD off/changed or new layout */
    enum capk kind = torn ? CAPK_TORN : layout_changed ? CAPK_LAYOUT : CAPK_OK;
    unsigned streak = cguard.discards; double streak_ms = streak ? (now() - cguard.first_t) * 1000 : 0;
    enum cap_verdict verdict = cap_guard_step(&cguard, kind, now());
    if (verdict == CV_DISCARD) {
        capture_discarded(kind == CAPK_TORN ? "a plane flipped during each copy of it (stitched)" : "LCD plane layout changed during the capture", torn_plane, 0);
        return CAP_TORN;
    }
    if (verdict == CV_FORCE && (cguard.forced <= 3 || cguard.forced % 25 == 0))
        LOG("capture accepted after %u discards (%.0f ms, %s): newest picture sent, the next consistent capture cleans it up (%u forced)",
            streak, streak_ms, kind != CAPK_TORN ? "layout change" : torn == 1 ? "a plane keeps flipping: stitched from consecutive frames (clean seams)" : torn == 2 ? "a plane keeps flipping: its last consistent copy is shown" : "a plane keeps flipping: TORN copy (more than 12 flips during one copy)", cguard.forced);
    if (capture_discards) { LOG("capture %s after %u discards (%.1f s)", verdict == CV_FORCE ? "forced" : "consistent again", capture_discards, now() - capture_discard_since); capture_discards = 0; }
    /* Vblank normally advances during a CPU copy. It is a diagnostic, not a
     * producer-ownership fence or a reason to discard otherwise equal tuples. */
    if(captures<3||captures%120==0)
        LOG("capture tuple stable: planes=%u seq=%s%llu->%s%llu switches=%d checks=%d",before.count,
            seq_before_ok?"":"unavailable:",(unsigned long long)seq_before,
            seq_after_ok?"":"unavailable:",(unsigned long long)seq_after,switches,checks);
    captures++;
    if (captures <= 3 || sync_ms + copy_ms + compose_ms > 250 || captures % 120 == 0)
        LOG("capture stages: planes=%d culled=%d sync=%.0f ms copy=%.0f ms compose=%.0f ms", n, first, sync_ms, copy_ms, compose_ms);
    return 0;
}
#else
static int capture_drm(void) { LOG("FAIL built without DRM"); return -1; }
#endif

/* After the framework's committed-buffer ACK, allow a scanout to advance
 * before reading its planes. This is a presentation opportunity, not proof
 * that every application complied with its redraw request. Never wait forever. */
struct appearance_scanout { char seen[96]; uint64_t baseline; uint32_t crtc; double deadline; int waiting; };
static int appearance_scanout_update(struct appearance_scanout *s, const char *request,
                                     int valid, uint32_t crtc, uint64_t seq, double t) {
    if (!request[0]) return 1;
    if (!valid) {
        if (strcmp(s->seen, request)) { LOG("WARN appearance %s: scanout sequence unavailable", request); snprintf(s->seen, sizeof s->seen, "%s", request); }
        s->waiting = 0; return 1;
    }
    if (strcmp(s->seen, request)) {
        snprintf(s->seen, sizeof s->seen, "%s", request);
        s->baseline = seq; s->crtc = crtc; s->deadline = t + 0.5; s->waiting = 1;
        LOG("appearance %s: scanout baseline %llu", request, (unsigned long long)seq);
        return 0;
    }
    if (!s->waiting) return 1;
    if (crtc != s->crtc || seq != s->baseline) { LOG("appearance %s: scanout advanced %llu", request, (unsigned long long)seq); s->waiting = 0; return 1; }
    if (t >= s->deadline) { LOG("WARN appearance %s: scanout sequence did not advance, fail open", request); s->waiting = 0; return 1; }
    return 0;
}
static int appearance_scanout_barrier(const char *request) {
#ifndef NO_DRM
    static struct appearance_scanout s;
    if (!request[0] || strcmp(source, "drm")) return 1;
    uint32_t crtc = 0; int w, h; uint64_t seq = 0, ns;
    if (drm_fd < 0 && (!drm_allowed() || drm_open_once())) return 1;
    int valid = lcd_crtc(&crtc, &w, &h) && !drmCrtcGetSequence(drm_fd, crtc, &seq, &ns);
    return appearance_scanout_update(&s, request, valid, crtc, seq, now());
#else
    (void)request; return 1;
#endif
}

/* ---------------- conversion: area average into 720x1440 (or 1440x720) ---------------- */
static uint8_t out[OW * OH]; static int out_w = OW, out_h = OH, geo_ox, geo_oy, geo_dw, geo_dh;
/* dualux (25 Sep): contrast LUT (persist.sys.a6l.eink.contrast 0..100, stock "high contrast text"): linear stretch
 * between a black point and a white point (0 = identity; 100 = 0..60 -> black, 195..255 -> white) */
static uint8_t lut[256]; static int lut_contrast = -1, lut_invert;
/* eink-round2: --tone BLACK,WHITE,GAMMAx100 (rc: persist.vendor.eink.tone, default 24,232,150; "0,255,100" = the old
 * linear LUT). Text-darkening curve for Material light UI on e-paper; see eink_logic.h tone_lut(). */
static int tone_black = TONE_DEFAULT_BLACK, tone_white = TONE_DEFAULT_WHITE, tone_gamma = TONE_DEFAULT_GAMMA;
static void lut_build(int c) {
    if (c < 0) c = 0; if (c > 100) c = 100; lut_contrast = c;
    tone_lut(lut, c, tone_black, tone_white, tone_gamma);
    if (lut_invert) tone_lut_invert(lut);	/* eink-round11 */
}
static struct area_resizer resizer;	/* eink-round4: fixed-point area average (eink_logic.c area_resize, host-tested) */
static void resample(void) {
    if (lut_contrast < 0) lut_build(0);
    int land = gw > gh; out_w = land ? OH : OW; out_h = land ? OW : OH;
    memset(out, 255, sizeof out);	/* letterbox bars = white (paper) */
    int dw, dh, ox, oy; fit_geometry(gw, gh, out_w, out_h, fit, &ox, &oy, &dw, &dh);
    geo_ox = ox; geo_oy = oy; geo_dw = dw; geo_dh = dh;
    /* Same box filter as the pre-round4 double version (within one grey level, exact on flat areas), several times
     * faster: the 6 Oct logs show resize=100-104 ms per capture, paid twice between a finger lift and the settled update */
    if (area_resize(&resizer, gray, gw, gh, out, out_w, out_h, ox, oy, dw, dh, lut)) { static int w; if (!w++) LOG("WARN resize: out of memory"); }
}
#define NT ((OW / TS) * (OH / TS))
static uint8_t cur_t[NT], prev_t[NT], shown_t[NT]; static int have_prev, have_shown, cur_land, shown_land, prev_land;
/* Mean tiles can alias equal-area text changes. These private grey snapshots
 * preserve exact pixel damage and advance the shown baseline only after ACK. */
static uint8_t prev_pixels[OW * OH], shown_pixels[OW * OH];
static const char *shown_waveform;
static unsigned shown_policy_epoch;
static int pixel_damage(const uint8_t *a, const uint8_t *b) {
    /* A one-level grey change can cross a dither/quantization boundary. Compare
     * the private pre-dither image exactly only when coarse damage says zero. */
    return memcmp(a, b, OW * OH) != 0;
}
static void tiles(uint8_t *t) {
    int tw = out_w / TS, th = out_h / TS;
    for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) { unsigned s = 0;
        for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++) s += out[(size_t)(ty * TS + y) * out_w + tx * TS + x];
        t[ty * tw + tx] = (uint8_t)(s / (TS * TS)); }
}
static double diff_frac(const uint8_t *a, const uint8_t *b) { int c = 0; for (int i = 0; i < NT; i++) if (abs(a[i] - b[i]) > threshold) c++; return (double)c / NT; }

/* ---------------- a6l_epdd client (socket; one outstanding command, replies are lines) ---------------- */
static int epd = -1; static char rbuf[1024]; static size_t rlen; static double last_connect_try; static int reply_timeout_ms = 30000;
static int epd_connect(void) {
    if (epd >= 0 || dry) return 0;
    if (now() - last_connect_try < 2.0) return -1;
    last_connect_try = now();
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", epd_socket);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) { static int warned; if (!warned++) LOG("WARN a6l_epdd socket %s: %s (retrying every 2 s)", epd_socket, strerror(errno)); if (s >= 0) close(s); return -1; }
    /* r5 review fix F36: writes are bounded too (send_all() blocks otherwise on a stalled server) */
    struct timeval tv = {reply_timeout_ms / 1000, (reply_timeout_ms % 1000) * 1000}; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    epd = s; rlen = 0; LOG("connected to a6l_epdd (%s)", epd_socket);
    if (have_shown) LOG("panel picture unknown after (re)connect: resynchronising");
    have_shown = 0;	/* F36: a (restarted) server's panel content is not what we last sent */
    return 0;
}
static void epd_drop(void) { if (epd >= 0) close(epd); epd = -1; rlen = 0; }
static int send_all(const void *p, size_t n) {
    const uint8_t *b = p; while (n) { ssize_t w = send(epd, b, n, MSG_NOSIGNAL); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; } b += w; n -= (size_t)w; } return 0;
}
/* command queue: at most 2 (clear + frame); busy while one is outstanding */
struct qcmd { char line[96]; int frame; struct pol_action policy; unsigned policy_epoch; };
static struct pol_state *ack_policy;
static unsigned policy_epoch = 1;
static struct pol_action pending_policy;
static unsigned pending_policy_epoch;
static struct qcmd q[2]; static int qn, busy; static double cmd_t, dry_done_at;
static uint8_t qframe[OW * OH]; static int qfw, qfh; static char qappearance[96];
/* Real changed-pixel area, not 1/NT dirty sentinel: count only for an otherwise
 * eligible ordinary settled action, stopping when the proposed15% cap is met. */
static size_t exact_changes_up_to(const uint8_t *a,const uint8_t *b,size_t n,size_t cap) {
    size_t changed=0;
    for(size_t i=0;i<n;i++)if(a[i]!=b[i]&&++changed>=cap)break;
    return changed;
}
static void ordinary_small_auto(struct pol_state *ps,struct pol_action *a,double moving,
        const char *appearance) {
    if(!auto_small_regal||ps->cfg.fixed_fast||ps->cfg.reading||!ps->cfg.active_mode||strcmp(ps->cfg.active_mode,POL_FASTEST)||!ps->primed||ps->burst||ps->fast_on_panel||
       moving!=0||!have_shown||shown_land!=cur_land||shown_policy_epoch!=policy_epoch||
       !shown_waveform||(strcmp(shown_waveform,POL_QUALITY)&&strcmp(shown_waveform,POL_READING))||
       (a->kind!=POL_SHOW&&a->kind!=POL_CLEAN_SHOW)||!a->mode||strcmp(a->mode,POL_QUALITY))return;
    if(appearance[0]) {
        char ready[96];prop_get("vendor.eink.ready",ready,sizeof ready);
        if(strcmp(ready,appearance))return; /* exact new appearance must complete normally */
    }
    size_t cap=sizeof shown_pixels*15/100;
    size_t changed=exact_changes_up_to(out,shown_pixels,sizeof shown_pixels,cap);
    if(!changed||changed>=cap)return;
    a->kind=POL_SHOW;a->mode=POL_READING;
    LOG("Auto ordinary REGAL: changed=%zu/%zu, quality periodic force bypassed for small damage",changed,sizeof shown_pixels);
}

/* r5 review fix F36: the picture on the panel (shown_t) is only committed when a6l_epdd acknowledged the frame with OK.
 * pend_t = the frame in flight; any ERR, lost reply (timeout), write failure or disconnect invalidates the baseline
 * (have_shown = 0, so the current page is resent even when static) with a bounded backoff (1, 2, 4 .. 30 s). A failed
 * "clear" also drops the frame queued behind it (clear + frame is one sequence); a successful clear leaves a white panel. */
enum { INF_OTHER, INF_FRAME, INF_CLEAR, INF_WARM };	/* eink-round11: INF_WARM = "warm" (e-ink CRTC bring-up, no picture) */
static char pending_appearance[96];
static uint8_t pend_t[NT]; static int pend_land, inflight; static double retry_at, backoff;
static void touch_gate_page_shown(void);	/* eink-round10 */
/* eink-round11 reader sleep: the last page change on the e-ink (frame ACK) or rear-touch press, CLOCK_MONOTONIC s;
 * published as vendor.eink.activity while persist.sys.a6l.eink.reader_sleep = 1 (a6l_dualux's idle timer) */
static double activity_t, touch_activity_t; static int reader_sleep_on;
static void cmd_result(int ok, const char *why) {
    if (inflight == INF_WARM) {	/* eink-round11: no picture involved: the panel state, backoff and policy are untouched */
        if (!ok) LOG("e-ink warm-up not done (%s): the first frame brings the CRTC up", why ? why : "error");
        pending_policy = (struct pol_action){POL_NONE, NULL}; inflight = INF_OTHER; return;
    }
    if (ok) {
        if (inflight == INF_FRAME) { touch_gate_page_shown(); activity_t = now(); memcpy(shown_t, pend_t, NT); memcpy(shown_pixels, qframe, sizeof shown_pixels); shown_land = pend_land; have_shown = 1; backoff = 0;
            shown_waveform = pending_policy.mode; shown_policy_epoch = pending_policy_epoch;
            if (use_props && !dry && pending_appearance[0]) prop_set("vendor.eink.ready", pending_appearance); }
        else if (inflight == INF_CLEAR) have_shown = 0;
    } else {
        if (inflight == INF_CLEAR && qn && q[0].frame) { LOG("clear failed: frame queued behind it dropped"); qn = 0; }
        backoff = backoff > 0 ? (backoff * 2 > 30 ? 30 : backoff * 2) : 1; retry_at = now() + backoff;
        LOG("WARN panel picture unknown (%s): resend in %.0f s", why, backoff); have_shown = 0;
    }
    /* Successful queued actions become policy history only with their
     * raster ACK. A changed mode/new mirror epoch cannot inherit an old ACK. */
    if (ok && ack_policy && pending_policy.kind != POL_NONE) {
        if (pending_policy_epoch == policy_epoch) pol_sent(ack_policy, &pending_policy, now());
        /* An older mode's successful raster remains the physical baseline.
         * It does not claim the current policy is clean or schedule an extra
         * mode-change flash; fresh damage follows the current selected policy. */
    }
    pending_policy = (struct pol_action){POL_NONE, NULL};
    inflight = INF_OTHER;
}
static int epd_send(const struct qcmd *c) {
    pending_policy = c->policy; pending_policy_epoch = c->policy_epoch;
    if (dry) {
        printf("A6L_MIRROR CMD %s\n", c->line); fflush(stdout);
        if (c->frame && outdir) { static int k; char p[600]; snprintf(p, sizeof p, "%s/f%d.pgm", outdir, k++); FILE *f = fopen(p, "wb");
            if (f) { fprintf(f, "P5\n%d %d\n255\n", qfw, qfh); if (fwrite(qframe, 1, (size_t)qfw * qfh, f) != (size_t)qfw * qfh) LOG("WARN short %s", p); fclose(f); } }
        dry_done_at = now() + (strstr(c->line, "clear") ? 2.0 : strstr(c->line, "fastest") ? 0.55 : !strcmp(c->line, "warm") ? 0.4 : 0.85);
        inflight = c->frame ? INF_FRAME : !strncmp(c->line, "clear", 5) ? INF_CLEAR : !strcmp(c->line, "warm") ? INF_WARM : INF_OTHER;
        busy = 1; cmd_t = now(); return 0;
    }
    if (epd_connect()) return -1;
    char l[128]; int n = snprintf(l, sizeof l, "%s\n", c->line);
    if (send_all(l, (size_t)n) || (c->frame && send_all(qframe, (size_t)qfw * qfh))) { LOG("WARN a6l_epdd write failed: %s", strerror(errno)); epd_drop(); return -1; }
    inflight = c->frame ? INF_FRAME : !strncmp(c->line, "clear", 5) ? INF_CLEAR : !strcmp(c->line, "warm") ? INF_WARM : INF_OTHER;
    pending_appearance[0] = 0;
    if (c->frame) snprintf(pending_appearance, sizeof pending_appearance, "%s", qappearance);
    busy = 1; cmd_t = now(); LOG("cmd: %s", c->line); return 0;
}
static void queue_cmd(const char *line, int with_frame) {
    if (qn >= 2) return; q[qn].policy = (struct pol_action){POL_NONE, NULL}; q[qn].policy_epoch = 0; snprintf(q[qn].line, sizeof q[qn].line, "%s", line); q[qn].frame = with_frame; qn++;
}
/* Automatic settle uses the selected waveform and an immutable captured
 * raster. Bare refresh stays reserved for the manual stock-mode-preserving path. */
static int queue_policy_frame(const struct pol_action *a, const char *appearance, double capture_ms, double resize_ms) {
    if (qn >= 2) return 0;
    memcpy(qframe, out, (size_t)out_w * out_h); qfw = out_w; qfh = out_h;
    snprintf(qappearance, sizeof qappearance, "%s", appearance);
    if (appearance[0]) LOG("appearance %s: frame queued capture=%.0f ms resize=%.0f ms", appearance, capture_ms, resize_ms);
    char line[96];
    snprintf(line, sizeof line, "frame %d %d %s%s", out_w, out_h, a->mode,
             a->kind == POL_CLEAN_SHOW || a->kind == POL_REFRESH ? " force" : "");
    queue_cmd(line, 1); q[qn - 1].policy = *a; q[qn - 1].policy_epoch = policy_epoch;
    memcpy(pend_t, cur_t, NT); pend_land = cur_land;
    return 1;
}
/* Manual/key refresh retains the daemon's last waveform. Account for its
 * ghost cleanup at its own ACK, after any older automatic frame ACK. */
static int queue_manual_refresh(void) {
    if (qn >= 2) return 0;
    queue_cmd("refresh", 0);
    q[qn - 1].policy = (struct pol_action){POL_REFRESH, POL_QUALITY};
    q[qn - 1].policy_epoch = policy_epoch;
    return 1;
}
static void pump(void) {	/* send the next queued command when idle */
    if (busy || !qn) return;
    if (epd_send(&q[0])) { if (!dry && epd < 0) { qn = 0; inflight = INF_OTHER; cmd_result(0, "a6l_epdd unreachable"); } return; }
    memmove(&q[0], &q[1], sizeof q[0]); qn--;
}
/* eink-round4 pipelining: typical frame command -> reply time and capture + resize time (EMAs), used to start the next
 * capture so that it ends about when a6l_epdd replies */
static double est_reply_s = 0.9, est_capture_s = 0.25;
/* start the capture a little early: an early capture is only slightly older, a late one delays the next command */
static double precapture_due(void) { double lead = est_reply_s * 0.85 - est_capture_s - 0.03; return cmd_t + (lead > 0 ? lead : 0); }
static int on_reply(char *line, struct pol_state *ps) {	/* returns 1 if a command completed */
    double dur = now() - cmd_t;
    LOG("epdd: %s (%.2f s)", line, dur);
    if (inflight == INF_FRAME && !strncmp(line, "OK", 2) && dur > 0.05 && dur < 5) est_reply_s = 0.7 * est_reply_s + 0.3 * dur;
    int warm = inflight == INF_WARM;	/* eink-round11: a warm-up is not an update (no min-gap after it) */
    busy = 0; cmd_result(!strncmp(line, "OK", 2), "a6l_epdd error reply"); if (!warm) pol_done(ps, now()); return 1;
}
static void epd_input(struct pol_state *ps) {
    ssize_t r = recv(epd, rbuf + rlen, sizeof rbuf - 1 - rlen, 0);
    if (r <= 0) { LOG("WARN a6l_epdd closed the connection"); epd_drop(); busy = 0; qn = 0; cmd_result(0, "a6l_epdd disconnected"); return; }
    rlen += (size_t)r; rbuf[rlen] = 0; char *nl;
    while ((nl = strchr(rbuf, '\n'))) { *nl = 0; on_reply(rbuf, ps); size_t used = (size_t)(nl - rbuf) + 1; memmove(rbuf, rbuf + used, rlen - used); rlen -= used; rbuf[rlen] = 0; }
    if (rlen >= sizeof rbuf - 1) rlen = 0;
}

/* ---------------- input: e-ink key + rear touch bridge ---------------- */
static int test_bit(const unsigned long *b, int n) { return (b[n / (8 * sizeof(long))] >> (n % (8 * sizeof(long)))) & 1; }
static int find_input(int want_key, char *path, size_t n) {	/* want_key: device with KEY 616; else the 720x1440 MT touchscreen */
    DIR *d = opendir("/dev/input"); struct dirent *e; int found = -1;
    while (d && (e = readdir(d)) && found < 0) {
        if (strncmp(e->d_name, "event", 5)) continue; char p[300]; snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NONBLOCK); if (fd < 0) continue;
        char name[128] = {0}; ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
        if (!strcmp(name, "a6l-eink-rear-touch")) { close(fd); continue; }	/* our own uinput device */
        if (want_key) { unsigned long kb[(KEY_MAX + 1) / (8 * sizeof(long)) + 1] = {0};
            if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof kb), kb) >= 0 && test_bit(kb, KEY_EINK)) { found = fd; snprintf(path, n, "%s (%s)", p, name); } }
        else { struct input_absinfo ax, ay;
            if (!ioctl(fd, EVIOCGABS(ABS_MT_POSITION_X), &ax) && !ioctl(fd, EVIOCGABS(ABS_MT_POSITION_Y), &ay) && ax.maximum == 719 && ay.maximum == 1439) { found = fd; snprintf(path, n, "%s (%s)", p, name); } }
        if (found < 0) close(fd);
    }
    if (d) closedir(d);
    return found;
}
static int input_quiet;	/* r5 bug hunt E2: periodic re-scans only log success */
static int open_input(const char *spec, int want_key, const char *what) {
    char p[400] = ""; int fd = -1;
    if (!strcmp(spec, "none")) return -1;
    if (!strcmp(spec, "auto")) fd = find_input(want_key, p, sizeof p);
    else { fd = open(spec, O_RDONLY | O_CLOEXEC | O_NONBLOCK); snprintf(p, sizeof p, "%s", spec); }
    if (fd < 0) { if (!input_quiet) LOG("WARN %s: no input device (%s)", what, spec); } else LOG("%s: %s", what, p);
    return fd;
}
#define NSLOT 10
struct slot { int in_active, x, y, dirty, out_active, ignored, start_fx, start_fy, moving; };
static struct slot sl[NSLOT]; static int cur_slot, next_tid = 1, ui = -1, touch_fd = -1, forward;
static unsigned gesture_generation;
/* eink-round6: rear touch forwarding in its own thread. Before, the single loop read the rear touchscreen only between
 * captures, and one capture + resize blocks it ~200 ms (7 Oct: capture=148 ms resize=48 ms). A flick that started during a
 * capture reached Android as one burst (uinput stamps events when they are written): DOWN, MOVEs and UP within ~1 ms, or
 * the moves 200 ms after the DOWN. VelocityTracker then sees no usable velocity (all samples at one instant, or the
 * pointer "stopped"), so the launcher snaps the page back and a list does not fling: "the swipe is ignored, I have to
 * swipe real good" (slow long drags worked: once a drag moves, captures are held and events flow in real time).
 * The thread owns reading + forwarding (same mapping, same slots, same uinput device); everything the capture loop
 * shares with it (slots, forward flag, touch map, fd, gesture generation) is under touch_mu. Policy calls stay in the
 * capture loop: a release is handed over (release_pending) and the loop is woken through a pipe.
 * --touch-thread 0 restores the old single-loop reading (comparison / fallback). */
static pthread_mutex_t touch_mu = PTHREAD_MUTEX_INITIALIZER;
static int touch_threaded = 1, touch_wake[2] = {-1, -1}, release_pending, touch_th_started, capture_delay_ms;
static double release_pending_t; static volatile sig_atomic_t touch_stop; static pthread_t touch_th;
static void touch_lock(void) { pthread_mutex_lock(&touch_mu); }
static void touch_unlock(void) { pthread_mutex_unlock(&touch_mu); }
static void wake_capture_loop(void) { if (touch_wake[1] >= 0) { char c = 1; if (write(touch_wake[1], &c, 1) < 0) { /* full: already woken */ } } }
static int touch_any_drag(void) { for (int i = 0; i < NSLOT; i++) if (sl[i].out_active && sl[i].moving) return 1; return 0; }
/* eink-round10 (user, 7 Oct 19:24:47: LCD (page 2) -> e-ink: "the e-ink shows the lock screen, then page 1 and stays there;
 * the LCD is on page 1 too afterwards" - the launcher really changed page, no swipe was made on purpose). Forwarding
 * started with `forward = 1` at mirror ON, and touch_flush() opened a NEW forwarded contact for any rear slot that was
 * down and moved: a finger resting on the rear panel while the phone is turned over (the e-ink key is pressed with the
 * LCD facing the user, the e-ink faces the user only after the flip) became DOWN + MOVE on the launcher = a page swipe,
 * 1.2 s after the key (mirror ON 48.95), before the e-ink showed anything of the current UI (the user could not have
 * aimed at anything yet). Now:
 *   - contacts already down when forwarding starts (mirror ON, front screen back on) are ignored until lifted, as
 *     touch_release_all() already did for contacts down when forwarding stops;
 *   - after mirror ON, contacts that BEGIN before the first page is shown on the e-ink (frame ACK) are ignored until
 *     lifted too, bounded by touch_guard_ms (default 2000, sys.a6l.eink.touch_guard_ms, --touch-guard-ms; 0 = off).
 * Front screen back on (wake-up on the e-ink, no flip): only the first rule (the page may not change at all, no frame). */
static int touch_guard_ms = 2000, touch_gate; static double touch_gate_until; static unsigned touch_gate_ignored;
static void touch_enable(const char *why, int guard) {	/* under touch_mu */
    int held = 0; forward = 1;
    for (int i = 0; i < NSLOT; i++) if (sl[i].in_active && !sl[i].out_active && !sl[i].ignored) { sl[i].ignored = 1; held++; }
    touch_gate = guard && touch_guard_ms > 0; touch_gate_until = now() + touch_guard_ms / 1000.0; touch_gate_ignored = 0;
    LOG("rear touch: forwarded again (%s)%s%s", why, held ? ", contacts already down ignored until lifted" : "",
        touch_gate ? ", new contacts held until the e-ink shows its first page" : "");
}
static void touch_gate_open(const char *why) {	/* under touch_mu */
    if (!touch_gate) return;
    touch_gate = 0; LOG("rear touch: guard ended (%s), %u contact(s) held back", why, touch_gate_ignored);
}
static void touch_gate_page_shown(void) { touch_lock(); touch_gate_open("first page shown on the e-ink"); touch_unlock(); }
static int suppress_drag_frames(void) { return use_props && prop_int("sys.a6l.eink.no_animations", 0); }
static struct tmap tm;
/* eink-round11: e-ink warm-up during the LCD -> e-ink switch. a6l_dualux publishes the switch request
 * (vendor.dualux.prepare "<seq> eink") at the key and holds the mirror off until the themed frame is ready (7 Oct
 * 19:24:47: 1.11 s, most of it the launcher relaunch for the e-ink theme). While the mirror is off, the first sight of a
 * new e-ink request sends "warm" to a6l_epdd: the CRTC bring-up (~0.4 s) overlaps the handshake instead of delaying the
 * first frame. Only when Android has been awake for >= 1 s (a key pressed while Android sleeps on the LCD also wakes it:
 * the LCD powers up right then, and an e-ink modeset in an LCD power-up is what round 5 forbade); a6l_epdd refuses a
 * cold start while the LCD pipeline is released anyway. persist.sys.a6l.eink.prewarm = 0 turns it off. */
static char prewarm_seen[96]; static double awake_since = -1; static int prewarm_enabled = 1;
static void prewarm_check(double t) {
    char aw[8] = ""; prop_get("vendor.dualux.awake", aw, sizeof aw);
    int awake = !strcmp(aw, "1");
    if (!awake) awake_since = -1; else if (awake_since < 0) awake_since = t;
    char pr[96] = ""; prop_get("vendor.dualux.prepare", pr, sizeof pr); size_t l = strlen(pr);
    if (l < 6 || strcmp(pr + l - 5, " eink") || !strcmp(pr, prewarm_seen)) return;
    snprintf(prewarm_seen, sizeof prewarm_seen, "%s", pr);
    if (!prewarm_enabled || !prop_int("persist.sys.a6l.eink.prewarm", 1)) return;
    if (!awake || t - awake_since < 1.0) { LOG("appearance %s: no e-ink warm-up (Android %s: the LCD may be powering up)", pr, awake ? "just woke" : "asleep"); return; }
    if (busy || qn) { LOG("appearance %s: no e-ink warm-up (a6l_epdd busy)", pr); return; }
    queue_cmd("warm", 0); pump(); LOG("appearance %s: e-ink warm-up during the switch handshake", pr);
}
static int touch_debug;
static void emit(int type, int code, int val) {
    if (touch_debug) printf("A6L_MIRROR TOUCH_OUT %d %d %d\n", type, code, val);
    if (touch_debug && type == EV_SYN && code == SYN_REPORT) printf("A6L_MIRROR TOUCH_SYN t=%.6f\n", now());	/* eink-round6: latency tests */
    if (ui < 0) return; struct input_event ev; memset(&ev, 0, sizeof ev); ev.type = (uint16_t)type; ev.code = (uint16_t)code; ev.value = val;
    if (write(ui, &ev, sizeof ev) != sizeof ev) { static int w; if (!w++) LOG("WARN uinput write: %s", strerror(errno)); }
}
static int uinput_open(void) {
    int fd = open("/dev/uinput", O_WRONLY | O_CLOEXEC | O_NONBLOCK); if (fd < 0) { LOG("WARN /dev/uinput: %s (rear touch cannot be forwarded)", strerror(errno)); return -1; }
    ioctl(fd, UI_SET_EVBIT, EV_KEY); ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH); ioctl(fd, UI_SET_EVBIT, EV_ABS); ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
    struct { int code, max; } ax[] = {{ABS_X, front_w - 1}, {ABS_Y, front_h - 1}, {ABS_MT_SLOT, NSLOT - 1}, {ABS_MT_TRACKING_ID, 65535}, {ABS_MT_POSITION_X, front_w - 1}, {ABS_MT_POSITION_Y, front_h - 1}};
    for (unsigned i = 0; i < sizeof ax / sizeof ax[0]; i++) { struct uinput_abs_setup a; memset(&a, 0, sizeof a); a.code = (uint16_t)ax[i].code; a.absinfo.maximum = ax[i].max;
        ioctl(fd, UI_SET_ABSBIT, ax[i].code); if (ioctl(fd, UI_ABS_SETUP, &a)) LOG("WARN UI_ABS_SETUP %d: %s", ax[i].code, strerror(errno)); }
    struct uinput_setup us; memset(&us, 0, sizeof us); us.id.bustype = BUS_VIRTUAL; us.id.vendor = 0; us.id.product = 0; us.id.version = 1;
    snprintf(us.name, sizeof us.name, "a6l-eink-rear-touch");
    if (ioctl(fd, UI_DEV_SETUP, &us) || ioctl(fd, UI_DEV_CREATE)) { LOG("WARN uinput create: %s", strerror(errno)); close(fd); return -1; }
    LOG("uinput touchscreen a6l-eink-rear-touch %dx%d created", front_w, front_h); return fd;
}
/* eink-round2: the finger left a held drag; the policy waits for the app's settled frame (pol_cfg.release_quiet_ms) */
static void gesture_released(void) { release_pending = 1; release_pending_t = now(); wake_capture_loop(); }	/* eink-round6: applied by the capture loop */
static void touch_release_all(void) {
    if (touch_any_drag()) { gesture_generation++; gesture_released(); }
    int any = 0; for (int i = 0; i < NSLOT; i++) if (sl[i].out_active) { emit(EV_ABS, ABS_MT_SLOT, i); emit(EV_ABS, ABS_MT_TRACKING_ID, -1); sl[i].out_active = 0; any = 1; }
    for (int i = 0; i < NSLOT; i++) { sl[i].ignored = sl[i].in_active; sl[i].moving = 0; }	/* contacts still down stay ignored until lifted */
    if (any) { emit(EV_KEY, BTN_TOUCH, 0); emit(EV_SYN, SYN_REPORT, 0); }
}
static void touch_flush(void) {
    int changed = 0, down = 0, was_dragging = touch_any_drag();
    int slop = use_props ? prop_int("sys.a6l.eink.gesture_slop", 24) : 24;
    if (slop < 1 || slop > 512) slop = 24;
    for (int i = 0; i < NSLOT; i++) {
        struct slot *s = &sl[i]; if (!s->dirty) { down |= s->out_active; continue; } s->dirty = 0;
        if (!s->in_active) { if (s->out_active) { emit(EV_ABS, ABS_MT_SLOT, i); emit(EV_ABS, ABS_MT_TRACKING_ID, -1); changed = 1; } s->out_active = 0; s->ignored = 0; s->moving = 0; continue; }
        int fx, fy, inside = tmap_apply(&tm, s->x, s->y, &fx, &fy);
        if (touch_debug) printf("A6L_MIRROR TOUCH_IN slot %d raw %d %d -> front %d %d %s%s\n", i, s->x, s->y, fx, fy, inside ? "inside" : "outside", forward ? "" : " (not forwarded)");
        if (!forward || s->ignored) continue;
        if (!s->out_active && touch_gate && now() >= touch_gate_until) touch_gate_open("timeout");	/* eink-round10 */
        if (!s->out_active && touch_gate) { s->ignored = 1; touch_gate_ignored++; if (touch_debug) printf("A6L_MIRROR TOUCH_GUARD slot %d held back\n", i); continue; }
        if (!s->out_active) { if (!inside) { s->ignored = 1; continue; } touch_activity_t = now(); s->out_active = 1; s->start_fx = fx; s->start_fy = fy; s->moving = 0; emit(EV_ABS, ABS_MT_SLOT, i); emit(EV_ABS, ABS_MT_TRACKING_ID, next_tid++ & 0xffff); }
        else { int dx = fx - s->start_fx, dy = fy - s->start_fy; if ((long long)dx * dx + (long long)dy * dy > (long long)slop * slop) s->moving = 1; emit(EV_ABS, ABS_MT_SLOT, i); }
        emit(EV_ABS, ABS_MT_POSITION_X, fx); emit(EV_ABS, ABS_MT_POSITION_Y, fy); changed = 1; down = 1;
        if (i == 0 || !sl[0].out_active) { emit(EV_ABS, ABS_X, fx); emit(EV_ABS, ABS_Y, fy); }
    }
    for (int i = 0; i < NSLOT; i++) down |= sl[i].out_active;
    if (changed) { emit(EV_KEY, BTN_TOUCH, down); emit(EV_SYN, SYN_REPORT, 0); }
    if (was_dragging != touch_any_drag()) { gesture_generation++; if (was_dragging) gesture_released(); if (suppress_drag_frames()) LOG("gesture %u: %s, touch delivery continues", gesture_generation, touch_any_drag() ? "hold intermediate frames" : "released, capture final page"); }
}
/* eink-round6: after SYN_DROPPED the kernel's evdev buffer overflowed (reader too late). The ongoing contact gets no new
 * ABS_MT_TRACKING_ID, so marking every slot inactive (as before) lifted the finger and ignored the rest of the swipe.
 * Now: events are skipped up to the next SYN_REPORT, then every slot is re-read (EVIOCGMTSLOTS); only when that fails
 * (FIFO in host tests) are the contacts lifted as before. */
static int touch_dropping;
static void touch_resync(void) {
    struct { uint32_t code; int32_t v[NSLOT]; } q; int32_t id[NSLOT], x[NSLOT], y[NSLOT]; int ok = 1;
    q.code = ABS_MT_TRACKING_ID; if (ioctl(touch_fd, EVIOCGMTSLOTS(sizeof q), &q) < 0) ok = 0; else memcpy(id, q.v, sizeof id);
    q.code = ABS_MT_POSITION_X; if (ok && ioctl(touch_fd, EVIOCGMTSLOTS(sizeof q), &q) < 0) ok = 0; else memcpy(x, q.v, sizeof x);
    q.code = ABS_MT_POSITION_Y; if (ok && ioctl(touch_fd, EVIOCGMTSLOTS(sizeof q), &q) < 0) ok = 0; else memcpy(y, q.v, sizeof y);
    struct input_absinfo a;
    if (ok && !ioctl(touch_fd, EVIOCGABS(ABS_MT_SLOT), &a)) cur_slot = a.value >= 0 && a.value < NSLOT ? a.value : 0;
    for (int k = 0; k < NSLOT; k++) {
        if (!ok) { sl[k].in_active = 0; sl[k].dirty = 1; continue; }
        int act = id[k] >= 0; if (act != sl[k].in_active || (act && (x[k] != sl[k].x || y[k] != sl[k].y))) sl[k].dirty = 1;
        sl[k].in_active = act; if (act) { sl[k].x = x[k]; sl[k].y = y[k]; }
    }
    { static int w; if (w++ < 5) LOG("rear touch: events dropped by the kernel (reader late): %s", ok ? "slot states re-read, contacts kept" : "slot states unreadable, contacts lifted"); }
    touch_flush();
}
static void touch_input(void) {
    struct input_event ev[64]; ssize_t r = read(touch_fd, ev, sizeof ev);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) {	/* r5 bug hunt eink-display E2: device gone (driver re-probe, read error) */
        LOG("WARN rear touch read: %s: forwarded contacts lifted, device re-opened when it comes back", r ? strerror(errno) : "EOF");
        close(touch_fd); touch_fd = -1;
        touch_release_all();			/* else Android keeps the forwarded finger pressed for ever */
        memset(sl, 0, sizeof sl); cur_slot = 0;	/* the lost device's contact state is meaningless now */
        return; }
    for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++) {
        struct input_event *e = &ev[i];
        if (touch_dropping) { if (e->type == EV_SYN && e->code == SYN_REPORT) { touch_dropping = 0; touch_resync(); } continue; }
        if (e->type == EV_ABS) {
            if (e->code == ABS_MT_SLOT) { cur_slot = e->value >= 0 && e->value < NSLOT ? e->value : 0; continue; }
            struct slot *s = &sl[cur_slot];
            if (e->code == ABS_MT_TRACKING_ID) { s->in_active = e->value >= 0; s->dirty = 1; }
            else if (e->code == ABS_MT_POSITION_X) { s->x = e->value; s->dirty = 1; }
            else if (e->code == ABS_MT_POSITION_Y) { s->y = e->value; s->dirty = 1; }
        } else if (e->type == EV_SYN && e->code == SYN_REPORT) touch_flush();
        else if (e->type == EV_SYN && e->code == SYN_DROPPED) touch_dropping = 1;
    }
}
static void *touch_main(void *arg) {	/* eink-round6: reads + forwards the rear touch as it comes */
    (void)arg;
    while (!touch_stop) {
        touch_lock(); int fd = touch_fd; unsigned g0 = gesture_generation; touch_unlock();
        if (fd < 0) { usleep(20000); continue; }
        struct pollfd p = {fd, POLLIN, 0}; int r = poll(&p, 1, 100);
        if (r <= 0) continue;
        touch_lock(); int same = touch_fd == fd; if (same) touch_input(); int changed = gesture_generation != g0 || release_pending; touch_unlock();
        if (!same) usleep(10000);	/* the fd number was reused meanwhile: not ours */
        if (changed) wake_capture_loop();
    }
    return NULL;
}
static void touch_thread_start(void) {
    if (!touch_threaded) { LOG("rear touch: read by the capture loop (--touch-thread 0)"); return; }
    if (pipe(touch_wake) == 0) { fcntl(touch_wake[0], F_SETFL, O_NONBLOCK); fcntl(touch_wake[1], F_SETFL, O_NONBLOCK); fcntl(touch_wake[0], F_SETFD, FD_CLOEXEC); fcntl(touch_wake[1], F_SETFD, FD_CLOEXEC); }
    if (pthread_create(&touch_th, NULL, touch_main, NULL)) { LOG("WARN rear touch thread: %s: read by the capture loop", strerror(errno)); touch_threaded = 0; return; }
    touch_th_started = 1; LOG("rear touch: forwarded by its own thread (never waits for a capture)");
}
/* r5 bug hunt eink-display E2: open + grab the rear touchscreen (at start, then every 5 s while it is missing) */
static void touch_attach(void) {
    if (touch_fd >= 0 || !strcmp(touch_dev, "none")) return;
    touch_fd = open_input(touch_dev, 0, "rear touch");
    if (touch_fd < 0) return;
    if (ioctl(touch_fd, EVIOCGRAB, 1)) LOG("WARN EVIOCGRAB rear touch: %s", strerror(errno)); else LOG("rear touch grabbed (Android no longer sees it directly)");
    if (ui < 0) ui = uinput_open();
}
static struct key_state ks; static int key_fd = -1;
static enum key_action key_input(void) {
    struct input_event ev[32]; ssize_t r = read(key_fd, ev, sizeof ev); enum key_action act = KA_NONE;
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return KA_NONE;
    if (r <= 0) { LOG("WARN key device read: %s", r ? strerror(errno) : "EOF"); close(key_fd); key_fd = -1; return KA_NONE; }
    for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++) if (ev[i].type == EV_KEY && ev[i].code == KEY_EINK) { enum key_action a = key_event(&ks, ev[i].value, now()); if (a) act = a; }
    return act;
}

/* ---------------- settings from properties ---------------- */
#define P_MODE "persist.vendor.eink.mode"
#define P_READING "persist.vendor.eink.reading"
#define P_TRANSFORM "persist.vendor.eink.touch_transform"
#define P_STATE "vendor.eink.state"
static int mode = EINK_OFF, reading, mode_forced = -1, reading_forced = -1;
/* dualux (25 Sep): live refresh mode / clear interval / clear requests from a6l_dualux and the A6LDisplaySwitcher app */
#define P_REFRESH "persist.sys.a6l.eink.refresh"
#define P_CLEAR_EVERY "persist.sys.a6l.eink.clear_every"
#define P_CLEAR_REQ "vendor.eink.clear_req"
static char refresh_mode[32] = ""; static int clear_every_prop = -1; static char clear_req[32] = ""; static int clear_req_init;
static int read_live_props(int *refresh_changed, int *clear_every_changed) {	/* returns 1 when a clear was requested */
    char v[96]; int req = 0; *refresh_changed = *clear_every_changed = 0;
    if (!use_props) return 0;
    prop_get(P_REFRESH, v, sizeof v); if (strcmp(v, refresh_mode)) { snprintf(refresh_mode, sizeof refresh_mode, "%s", v); *refresh_changed = 1; }
    int ce = prop_int(P_CLEAR_EVERY, -1); if (ce != clear_every_prop) { clear_every_prop = ce; *clear_every_changed = ce >= 0; }
    prop_get(P_CLEAR_REQ, v, sizeof v); if (strcmp(v, clear_req)) { snprintf(clear_req, sizeof clear_req, "%s", v); req = clear_req_init; }
    int inv = prop_int("persist.sys.a6l.eink.invert", 0) == 1;	/* eink-round11 */
    if (inv != lut_invert) { lut_invert = inv; lut_build(lut_contrast < 0 ? 0 : lut_contrast); LOG("inverted rendering %s", inv ? "on" : "off"); have_shown = 0; }
    int ct = prop_int("persist.sys.a6l.eink.contrast", 0); ct = ct < 0 ? 0 : ct > 100 ? 100 : ct; if (ct != lut_contrast) { lut_build(ct); LOG("contrast %d", lut_contrast); have_shown = 0; }
    clear_req_init = 1; return req;
}
static char transform[64] = "", transform_forced[64] = "";
static void read_props(int *mode_changed, int *reading_changed) {
    char v[96];
    int m = mode_forced >= 0 ? mode_forced : use_props && prop_get(P_MODE, v, sizeof v) > 0 ? mode_parse(v, mode) : mode;
    int r = reading_forced >= 0 ? reading_forced : use_props ? prop_int(P_READING, 0) != 0 : reading;
    char t[64]; if (transform_forced[0]) snprintf(t, sizeof t, "%s", transform_forced); else if (use_props) { prop_get(P_TRANSFORM, t, sizeof t); } else snprintf(t, sizeof t, "%s", transform);
    *mode_changed = m != mode; *reading_changed = r != reading; mode = m; reading = r;
    if (strcmp(t, transform)) { snprintf(transform, sizeof transform, "%s", t); if (tmap_parse_transform(&tm, transform)) LOG("WARN bad touch transform '%s'", transform); else LOG("touch transform '%s'", transform); }
}
static void set_state(const char *s) { static char last[64]; if (strcmp(last, s)) { snprintf(last, sizeof last, "%s", s); if (use_props && !dry) prop_set(P_STATE, s); LOG("state: %s", s); } }

/* ---------------- eink-round4: pipelined capture and decision ---------------- */
struct staged_capture { int valid, frame; double t, tcap, tresample, tdamage, moving; unsigned gesture; char appearance[96]; };
static struct staged_capture staged;
static int pipeline = 1;		/* --pipeline 0|1 (rc persist.vendor.eink.pipeline): capture during the drive */
static double precap_cmd_t = -1;	/* the command during whose drive the pipelined capture was taken */
static int release_settling(const struct pol_state *ps) {	/* 0005 settle window after a held drag was released */
    return ps->release_t > 0 && ps->cfg.release_quiet_ms > 0 && (now() - ps->release_t) * 1000 < ps->cfg.release_max_ms;
}
static double capture_interval(const struct pol_state *ps) {
    /* the settle needs one unchanged capture pair after the lift: take them back to back, not interval_ms apart */
    return release_settling(ps) ? 0.03 : interval_ms / 1000.0;
}
/* Damage against the panel picture, appearance tagging, policy decision and queueing of the capture held in out/cur_t
 * (the latest one). Returns 1 when a frame was queued. */
static int decide_capture(struct pol_state *ps, const struct staged_capture *sc, const char *how) {
    char capture_appearance[96]; snprintf(capture_appearance, sizeof capture_appearance, "%s", sc->appearance);
    if (capture_appearance[0]) {	/* the tag must still be the current request at decision time */
        char current[96]; appearance_snapshot(current, sizeof current);
        if (strcmp(current, capture_appearance)) capture_appearance[0] = 0;
    }
    double vs_panel = have_shown && shown_land == cur_land ? diff_frac(cur_t, shown_t) : 1.0;
    if (have_shown && shown_land == cur_land && vs_panel == 0 && pixel_damage(out, shown_pixels)) {
        vs_panel = 1.0 / NT;
        LOG("frame %d: pixel damage missed by tile means", sc->frame);
    }
    /* An identical static page still needs a fresh, correctly tagged ACK
     * when preparing a new switch; tile equality is not appearance readiness. */
    if (capture_appearance[0]) {
        char panel_ready[96]; prop_get("vendor.eink.ready", panel_ready, sizeof panel_ready);
        if (strcmp(panel_ready, capture_appearance)) vs_panel = 1.0;
    }
    if (!dry && epd < 0) return 0;
    if (now() < retry_at) return 0;	/* F36: bounded backoff after a failed / unacknowledged update */
    struct pol_action a = appearance_policy_decide(ps, now(), vs_panel, busy || qn > 0, capture_appearance);
    ordinary_small_auto(ps, &a, sc->moving, capture_appearance);
    if (a.kind == POL_NONE) return 0;
    LOG("frame %d policy: kind=%d mode=%s fixed=%d moving=%.6f vs_panel=%.6f capture=%.0f ms resize=%.0f ms damage=%.3f ms gesture=%u%s%s",
        sc->frame, a.kind, a.mode ? a.mode : "none", ps->cfg.fixed_fast, sc->moving, vs_panel,
        sc->tcap * 1000, sc->tresample * 1000, sc->tdamage * 1000, sc->gesture, how[0] ? " " : "", how);
    queue_policy_frame(&a, capture_appearance, sc->tcap * 1000, sc->tresample * 1000);
    return 1;
}

int main(int argc, char **argv) {
    t_start = now(); pol_default_cfg(&pcfg);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--epd-socket")) epd_socket = v; else if (OPT("--out")) outdir = v; else if (OPT("--source")) source = v;
        else if (OPT("--interval")) interval_ms = atoi(v); else if (OPT("--quiet")) pcfg.quiet_ms = atoi(v); else if (OPT("--settle")) pcfg.settle_ms = atoi(v);
        else if (OPT("--min-gap")) pcfg.min_gap_ms = atoi(v); else if (OPT("--clear-every")) pcfg.clear_every = atoi(v); else if (OPT("--max-per-min")) pcfg.max_per_min = atoi(v);
        else if (OPT("--active-mode")) pcfg.active_mode = v;
        else if (OPT("--release-quiet")) pcfg.release_quiet_ms = atoi(v); else if (OPT("--release-max")) pcfg.release_max_ms = atoi(v);
        else if (OPT("--tone")) {
            int b, w, g;
            if (sscanf(v, "%d,%d,%d", &b, &w, &g) == 3 && b >= 0 && b <= 96 && w >= 160 && w <= 255 && g >= 50 && g <= 300) { tone_black = b; tone_white = w; tone_gamma = g; }
            else fprintf(stderr, "ignoring bad --tone '%s' (BLACK 0..96,WHITE 160..255,GAMMAx100 50..300)\n", v);
        }
        else if (OPT("--fit")) {
            if (!strcmp(v, "stretch")) fit = FIT_STRETCH;
            else if (!strcmp(v, "crop")) fit = FIT_CROP;
            else if (!strcmp(v, "letterbox")) fit = FIT_LETTERBOX;
            else { fprintf(stderr, "unknown fit: %s\n", v); return 2; }
        } else if (OPT("--threshold")) threshold = atoi(v);
        else if (OPT("--frames")) frames_max = atoi(v); else if (!strcmp(a, "--dry")) dry = 1; else if (OPT("--ns-pid")) ns_pid = atoi(v);
        else if (OPT("--screencap")) screencap = v; else if (OPT("--display")) display_id = v;
        else if (OPT("--mode")) mode_forced = mode_parse(v, EINK_MIRROR); else if (OPT("--reading")) reading_forced = atoi(v) != 0;
        else if (!strcmp(a, "--no-props")) use_props = 0; else if (OPT("--key-dev")) key_dev = v; else if (OPT("--touch-dev")) touch_dev = v;
        else if (OPT("--touch-transform")) snprintf(transform_forced, sizeof transform_forced, "%s", v);
        else if (OPT("--front")) { if (sscanf(v, "%dx%d", &front_w, &front_h) != 2) return 2; }
        else if (!strcmp(a,"--auto-small-regal")) auto_small_regal=1;
        else if (OPT("--copy-guard-kib")) guard_chunk = atoi(v) > 0 ? (size_t)atoi(v) * 1024 : 0;	/* round5: 0 = default (see pcopy_chunk) */
        else if (OPT("--pipeline")) pipeline = atoi(v) != 0;
        else if (OPT("--guard-max")) {	/* eink-round4: N discards / MS since the first one, then the newest capture is accepted */
            int nd, ms;
            if (sscanf(v, "%d,%d", &nd, &ms) == 2 && nd >= 0 && nd <= 100 && ms >= 0 && ms <= 10000) { guard_max_discards = nd; guard_max_ms = ms; }
            else fprintf(stderr, "ignoring bad --guard-max '%s' (N 0..100,MS 0..10000)\n", v);
        }
        else if (!strcmp(a, "--keep-crtc-front-off")) keep_crtc_front_off = 1;
        else if (OPT("--reply-timeout")) reply_timeout_ms = atoi(v) > 0 ? atoi(v) : 30000;
        else if (OPT("--wait-prop")) wait_prop = v; else if (!strcmp(a, "--touch-debug")) touch_debug = 1;
        else if (OPT("--touch-thread")) touch_threaded = atoi(v) != 0;	/* eink-round6 */
        else if (OPT("--touch-guard-ms")) touch_guard_ms = atoi(v);	/* eink-round10 */
        else if (OPT("--prewarm")) prewarm_enabled = atoi(v) != 0;	/* eink-round11 */
        else if (OPT("--capture-delay-ms")) capture_delay_ms = atoi(v);	/* eink-round6 host tests: a slow capture (phone: ~200 ms) */
        else if (OPT("--send")) {	/* one-shot client: send one command to a6l_epdd, print the reply (attended tests) */
            last_connect_try = -10; if (epd_connect()) return 1;
            char l[600]; int n = snprintf(l, sizeof l, "%s\n", v); if (send_all(l, (size_t)n)) return 1;
            struct pollfd pf = {epd, POLLIN, 0}; size_t got = 0; char rb[600] = {0};
            while (got < sizeof rb - 1 && poll(&pf, 1, 30000) == 1) { ssize_t r = recv(epd, rb + got, sizeof rb - 1 - got, 0); if (r <= 0) break; got += (size_t)r; if (memchr(rb, '\n', got)) break; }
            printf("%s", got ? rb : "(no reply)\n"); return strncmp(rb, "OK", 2) != 0;
        }
        else { fprintf(stderr, "usage: see source header (%s)\n", a); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN); setvbuf(stdout, NULL, _IOLBF, 0);
    if (outdir) mkdir(outdir, 0755);
    key_init(&ks, 800);
    tm.touch_w = 720; tm.touch_h = 1440; tm.panel_w = OW; tm.panel_h = OH; tm.front_w = front_w; tm.front_h = front_h;
    fit_geometry(front_w, front_h, OW, OH, fit, &tm.img_x, &tm.img_y, &tm.img_w, &tm.img_h);
    int mc, rc_; if (mode_forced < 0 && use_props) { char v[96]; if (prop_get(P_MODE, v, sizeof v) <= 0) { mode = EINK_OFF; } } read_props(&mc, &rc_);
    key_fd = open_input(key_dev, 1, "e-ink key");
    touch_attach();
    touch_thread_start();
    pcfg.reading = reading;
    if (use_props && !reading && !refresh_mode[0]) pol_apply_refresh_mode(&pcfg, "stock");	/* default before the first prop read */
    struct pol_state ps; pol_init(&ps, &pcfg, now()); ack_policy = &ps;
    cap_guard_init(&cguard, guard_max_discards, guard_max_ms);
    LOG("capture guard: every plane copy checked against its own buffer every %zu KiB%s; never-starve bound %d discards / %d ms; a plane flipping during every copy is stitched (eink-round5)",
        pcopy_chunk(guard_chunk, 0) / 1024, guard_chunk ? "" : " (--copy-guard-kib 0 is no longer honoured: default)", guard_max_discards, guard_max_ms);
    int active = 0, paused = 0, frame = 0, fails = 0; double next_cap = now(), next_props = now();
    LOG("start: source=%s interval=%d mode=%s reading=%d clear-every=%d active=%s fit=%s%s", source, interval_ms, mode_name(mode), reading, pcfg.clear_every, pcfg.active_mode, fit == FIT_STRETCH ? "stretch" : fit == FIT_CROP ? "crop" : "letterbox", dry ? " DRY" : "");
    LOG("tone: black clip %d, white clip %d, gamma %.2f; release settle %d ms (max %d ms)", tone_black, tone_white, tone_gamma / 100.0, pcfg.release_quiet_ms, pcfg.release_max_ms);
    while (!stop && (!frames_max || frame < frames_max)) {
        double t = now();
        { touch_lock(); int rp = release_pending; double rt = release_pending_t; release_pending = 0; touch_unlock();	/* eink-round6 */
          if (rp && ack_policy && suppress_drag_frames()) pol_gesture_released(ack_policy, rt); }
        /* mode transitions */
        if (t >= next_props) { next_props = t + (use_props && prop_int("sys.a6l.dualux.theme_sync", 0) ? 0.05 : 0.5); int m1, r1; read_props(&m1, &r1);
            if (use_props && !active && mode != EINK_MIRROR) prewarm_check(t);	/* eink-round11 */
            if (use_props) { reader_sleep_on = prop_int("persist.sys.a6l.eink.reader_sleep", 0) == 1;	/* eink-round11 */
                touch_lock(); double ta = touch_activity_t; touch_unlock(); if (ta > activity_t) activity_t = ta;
                static double published; if (reader_sleep_on && activity_t > published) { char b[32]; snprintf(b, sizeof b, "%.3f", activity_t); prop_set("vendor.eink.activity", b); published = activity_t; } }
            if (r1 && !refresh_mode[0]) { pcfg.reading = reading; ps.cfg.reading = reading; policy_epoch++; LOG("reading mode %s", reading ? "on" : "off"); }
            int rc1, ce1; if (read_live_props(&rc1, &ce1)) {
                LOG("ghost refresh requested (%s=%s)", P_CLEAR_REQ, clear_req);
                queue_manual_refresh();
            }
            if (ce1) { pcfg.clear_every = ps.cfg.clear_every = clear_every_prop; policy_epoch++; LOG("clear every %d clean updates", clear_every_prop); }
            if (rc1 || (r1 && refresh_mode[0])) { struct pol_cfg nc = pcfg;
                if (!pol_apply_refresh_mode(&nc, refresh_mode[0] ? refresh_mode : reading ? "partial" : "stock")) { pcfg = nc; ps.cfg = nc; policy_epoch++; LOG("refresh mode %s", refresh_mode[0] ? refresh_mode : "stock (default)"); }
                else LOG("WARN unknown %s '%s' (auto|quality|partial|fast|fastest)", P_REFRESH, refresh_mode); } }
        /* epdd retains its image/library state across power off and performs a
         * recovery clear itself after a failed drive. Switching sides needs no
         * unconditional clear-white + INIT sequence before showing the page. */
        /* eink-round6d: the REGAL history belongs to the panel, not to one e-ink session: keep it across mirror OFF/ON (before,
         * every screen switch reset it, so with frequent switches the periodic ghost cleanup never came) */
        if (mode == EINK_MIRROR && !active) { active = 1; if (!refresh_mode[0]) pcfg.reading = reading; { int keep_reading_n = ps.reading_n, keep_clean_n = ps.clean_n; pol_init(&ps, &pcfg, t); ps.reading_n = keep_reading_n; ps.clean_n = keep_clean_n; } policy_epoch++; have_prev = have_shown = 0; staged.valid = 0;
            if (use_props) touch_guard_ms = prop_int("sys.a6l.eink.touch_guard_ms", touch_guard_ms);
            touch_lock(); touch_enable("mirror ON", 1); touch_unlock(); LOG("mirror ON%s", reading ? " (reading)" : ""); next_cap = t; }
        if (mode != EINK_MIRROR && active) { active = 0; staged.valid = 0; policy_epoch++; touch_lock(); forward = 0; touch_release_all(); touch_unlock(); qn = 0; queue_cmd("power off", 0); LOG("mirror OFF (e-ink keeps the last picture)"); }
        if (!dry && epd < 0) epd_connect();
        { static double next_touch_scan; touch_lock(); if (touch_fd < 0 && t >= next_touch_scan) { next_touch_scan = t + 5; input_quiet = 1; touch_attach(); input_quiet = 0; } touch_unlock(); }
        set_state(!active ? "off" : epd < 0 && !dry ? "mirror-no-epdd" : paused ? "mirror-paused" : reading ? "mirror-reading" : "mirror");
        /* wait for input / replies / the next capture */
        struct pollfd p[4]; int n = 0, ik = -1, it = -1, ie = -1, iw = -1;
        if (key_fd >= 0) { ik = n; p[n++] = (struct pollfd){key_fd, POLLIN, 0}; }
        if (!touch_threaded && touch_fd >= 0) { it = n; p[n++] = (struct pollfd){touch_fd, POLLIN, 0}; }
        if (touch_wake[0] >= 0) { iw = n; p[n++] = (struct pollfd){touch_wake[0], POLLIN, 0}; }
        if (epd >= 0) { ie = n; p[n++] = (struct pollfd){epd, POLLIN, 0}; }
        double until = active && !busy && !qn ? (staged.valid ? t : next_cap) : t + 0.1; if (ks.down) until = t + 0.05;
        if (active && busy && pipeline && precap_cmd_t != cmd_t) {	/* eink-round4: wake for the pipelined capture */
            double due = precapture_due(); if (due < next_cap) due = next_cap;
            if (due < until) until = due > t ? due : t;
        } if (dry && busy && dry_done_at < until) until = dry_done_at; if (next_props < until) until = next_props;
        int to = (int)((until - now()) * 1000); if (to < 0) to = 0;
        int r = poll(p, (nfds_t)n, to);
        if (r < 0 && errno != EINTR) { LOG("FAIL poll: %s", strerror(errno)); break; }
        if (r > 0 && ie >= 0 && (p[ie].revents & (POLLIN | POLLHUP | POLLERR))) epd_input(&ps);
        if (r > 0 && it >= 0 && (p[it].revents & (POLLIN | POLLHUP | POLLERR))) { touch_lock(); touch_input(); touch_unlock(); }
        if (r > 0 && iw >= 0 && (p[iw].revents & POLLIN)) { char wb[64]; while (read(touch_wake[0], wb, sizeof wb) > 0) {} }
        enum key_action ka = KA_NONE;
        if (r > 0 && ik >= 0 && (p[ik].revents & (POLLIN | POLLHUP | POLLERR))) ka = key_input();
        if (!ka) ka = key_tick(&ks, now());
        if (ka == KA_TOGGLE) { int nm = mode == EINK_MIRROR ? EINK_OFF : EINK_MIRROR; LOG("e-ink key: %s", mode_name(nm));
            if (mode_forced >= 0) mode_forced = nm; mode = nm; if (use_props) prop_set(P_MODE, mode_name(nm)); }
        if (ka == KA_CLEAR) { LOG("e-ink key long press: refresh");
            queue_manual_refresh(); }
        if (dry && busy && now() >= dry_done_at) {
            if (inflight == INF_FRAME) est_reply_s = 0.7 * est_reply_s + 0.3 * (now() - cmd_t);
            int warm = inflight == INF_WARM; busy = 0; cmd_result(1, NULL); if (!warm) pol_done(&ps, now()); LOG("done (simulated)%s", warm ? ": warm" : ""); }
        if (!dry && busy && (now() - cmd_t) * 1000 > reply_timeout_ms) {	/* F36: lost reply: the connection is out of step */
            LOG("WARN a6l_epdd reply timeout (%.1f s): dropping the connection", now() - cmd_t);
            epd_drop(); busy = 0; qn = 0; cmd_result(0, "reply timeout"); pol_done(&ps, now()); }
        pump();
        /* eink-round4 pipelining. Before: nothing was captured while a6l_epdd drove an update, so every cycle paid
         * capture + resize (~240 ms, 6 Oct logs: capture=122-142 ms resize=100-104 ms) after the reply. Now one capture
         * is taken DURING the drive, timed to end about when the reply arrives (est_reply_s - est_capture_s), staged,
         * and decided at the reply: the next frame command leaves at once. During a release settle (0005) captures run
         * back to back while busy, so the settled page is staged too. A drag still holds captures. */
        if (active && staged.valid && !busy && !qn) {
            touch_lock(); unsigned gen_now = gesture_generation; int drag_now = touch_any_drag(); touch_unlock();
            int stale = now() - staged.t > 1.0 || staged.gesture != gen_now || (suppress_drag_frames() && drag_now);
            staged.valid = 0;
            if (stale) { next_cap = now(); continue; }
            if (decide_capture(&ps, &staged, "staged")) pump();
            next_cap = now() + capture_interval(&ps);
            continue;
        }
        if (!active || now() < next_cap) continue;
        int precapture = 0;
        if (busy && !qn && inflight == INF_WARM) {	/* eink-round11: capture while the CRTC comes up; sent at the reply */
            if (precap_cmd_t == cmd_t) continue;
            precapture = 1;
        } else if (busy || qn) {
            if (!pipeline || qn || inflight != INF_FRAME) continue;
            int settle = release_settling(&ps);
            if (!settle && (precap_cmd_t == cmd_t || now() < precapture_due())) continue;
            precapture = 1;
        }
        touch_lock(); int dragging = touch_any_drag(); unsigned capture_gesture = gesture_generation; touch_unlock();
        if (suppress_drag_frames() && dragging) { next_cap = now() + 0.02; continue; }
        /* capture + policy */
        char capture_appearance[96], after_appearance[96];
        appearance_snapshot(capture_appearance, sizeof capture_appearance);
        if (!appearance_scanout_barrier(capture_appearance)) { next_cap = now() + 0.02; continue; }
        double ivl = capture_interval(&ps);
        next_cap += ivl; if (next_cap < now()) next_cap = now() + ivl;
        double t0 = now(); int rc;
        if (capture_delay_ms > 0) usleep((useconds_t)capture_delay_ms * 1000);	/* host tests only */
        if (!strcmp(source, "screencap")) rc = capture_screencap();
        else if (!strcmp(source, "drm")) rc = capture_drm();
        else if (!strncmp(source, "file:", 5)) rc = capture_file(source + 5);
        else if (!strncmp(source, "files:", 6)) { static int last_ok; char pth[512]; snprintf(pth, sizeof pth, source + 6, frame);
            if (!access(pth, R_OK)) last_ok = frame; else snprintf(pth, sizeof pth, source + 6, last_ok); rc = capture_file(pth); }
        else { LOG("FAIL unknown source %s", source); return 2; }
        frame++;
        if (precapture) precap_cmd_t = cmd_t;
        if (rc == CAP_FRONT_OFF) { cap_guard_reset(&cguard); if (!paused) { KLOG("front screen off: paused (rear touch not forwarded)"); touch_lock(); forward = 0; touch_release_all(); touch_unlock(); } paused = 1;
            /* eink-round3: the e-ink CRTC follows the LCD CRTC, so no system suspend saves/restores an enabled lessee CRTC
             * (6 Oct 16:07: e-ink on across a suspend -> LCD scan-out of an unmapped buffer after the next LCD switch).
             * The picture stays on the panel; the next update does the bring-up again (as after idle-off). */
            if (front_follow_step(&front_ff, 1) && !keep_crtc_front_off) { queue_cmd("power off", 0); LOG("front screen off: e-ink CRTC off (no suspend with the lessee CRTC enabled)"); pump(); }
            continue; }
        front_follow_step(&front_ff, 0);
        if (rc == CAP_TORN) { fails = 0; continue; }	/* eink-round3: retried at the next interval; never sent, never a failure */
        if (rc) { if (++fails >= 40) { LOG("FAIL 40 consecutive capture failures: pausing 30 s"); fails = 0; next_cap = now() + 30; } continue; }
        /* eink-round6b: the LCD came back on, but a wake-up with the power key leaves the e-ink (dualux sets mode off with the
         * LCD state) and the props are read only every 0.5 s: the mirror sent one frame anyway, an e-ink cold modeset right in
         * the LCD power-on (7 Oct 12:14:04: "front screen on: resumed" 479.84 -> "e-ink CRTC 70 modeset" 480.19 -> "power
         * off" 481.43, with LCD vblank timeouts around it). Re-read the mode before the first frame after a pause. */
        if (paused && use_props && mode_forced < 0) { char mv[16]; if (prop_get(P_MODE, mv, sizeof mv) > 0 && mode_parse(mv, mode) != EINK_MIRROR) { next_props = 0; continue; } }
        if (paused) { KLOG("front screen on: resumed"); paused = 0; touch_lock(); touch_enable("front screen on", 0); touch_unlock(); }
        fails = 0; double tcap = now() - t0;
        double tr = now(); resample(); cur_land = out_w > out_h; tiles(cur_t); double tresample = now() - tr;
        est_capture_s = 0.7 * est_capture_s + 0.3 * (tcap + tresample);
        appearance_snapshot(after_appearance, sizeof after_appearance);
        if (strcmp(capture_appearance, after_appearance)) capture_appearance[0] = 0;
        /* Input may have arrived during the bounded CPU copy/resize. Drain it
         * before submission and discard pixels that span a drag transition. */
        touch_lock();
        if (!touch_threaded && touch_fd >= 0) { struct pollfd tp = {touch_fd, POLLIN, 0}; if (poll(&tp, 1, 0) > 0) touch_input(); }
        int drag_after = touch_any_drag(), gen_after = gesture_generation != capture_gesture;
        if (!cur_land && (gw != tm.front_w || gh != tm.front_h || geo_ox != tm.img_x || geo_dw != tm.img_w)) {	/* keep the touch map on the picture actually shown */
            tm.front_w = gw; tm.front_h = gh; tm.img_x = geo_ox; tm.img_y = geo_oy; tm.img_w = geo_dw; tm.img_h = geo_dh; }
        touch_unlock();
        if (suppress_drag_frames() && (drag_after || gen_after)) { next_cap = now(); continue; }
        double td = now();
        double moving = have_prev && prev_land == cur_land ? diff_frac(cur_t, prev_t) : 1.0;
        /* Equal tile averages can hide real text changes. Keep exact damage
         * for submission and quiet timing, but do not invent a motion burst. */
        int pixel_motion = have_prev && prev_land == cur_land && pixel_damage(out, prev_pixels);
        pol_observe(&ps, now(), moving);	/* eink-round4: motion history at capture time (decision may come later) */
        if (pixel_motion && moving == 0) ps.last_change = now();
        memcpy(prev_t, cur_t, NT); memcpy(prev_pixels, out, sizeof prev_pixels); have_prev = 1; prev_land = cur_land;
        struct staged_capture sc = {1, frame, now(), tcap, tresample, now() - td, moving, capture_gesture, ""};
        snprintf(sc.appearance, sizeof sc.appearance, "%s", capture_appearance);
        if (frame <= 3 || tcap + tresample > 0.75 || frame % 240 == 0) LOG("frame %d: %dx%d capture %.0f ms, resize/tiles %.0f ms, moving %.3f%s", frame, gw, gh, tcap * 1000, tresample * 1000, moving, precapture ? " (staged during the drive)" : "");
        if (precapture || busy || qn) { staged = sc; continue; }	/* the panel is still busy: decided at the reply */
        if (decide_capture(&ps, &sc, "")) pump();
    }
    while (!stop && busy && dry) { usleep(20000); if (now() >= dry_done_at) busy = 0; }
    if (touch_th_started) { touch_stop = 1; pthread_join(touch_th, NULL); }
    touch_lock(); if (ui >= 0) { touch_release_all(); ioctl(ui, UI_DEV_DESTROY); close(ui); } touch_unlock();
    LOG("exit after %d frames", frame);
    return 0;
}
