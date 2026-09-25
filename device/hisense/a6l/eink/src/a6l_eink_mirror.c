// SPDX-License-Identifier: Apache-2.0
/* a6l_eink_mirror — Hisense A6L rear e-ink controller for the installed ROM (v2, agent eink3, 24 Sep 2026).
 * Base: device/hisense/a6l/diagnostic/a6l_eink_mirror.c (M1: capture + resample + tile diff + policy).
 *
 * One vendor service that owns everything user-facing about the rear screen:
 *  - MODE (persist.vendor.eink.mode = off | mirror), toggled by the e-ink side key (KEY code 616, gpio-keys
 *    "A6L side keys"): short press = mirror on/off, long press (>= 800 ms) = full clear flash.
 *  - MIRROR: captures the front screen, converts it to the 720x1440 portrait e-ink picture and drives a6l_epdd over its
 *    control socket with the automatic waveform policy of eink_logic.c (quality on discrete changes, A2 bursts while
 *    scrolling, one clean update when it settles, periodic clears); READING (persist.vendor.eink.reading = 1): no A2,
 *    REGAL partial updates when the page is still, GC16 on page turns, periodic forced refresh.
 *    Front screen off (LCD CRTC inactive) -> pause (the e-paper keeps the last picture).
 *  - REAR TOUCH (ft5x06 on blsp_i2c7, 720x1440): always grabbed (EVIOCGRAB) so Android never sees raw rear touches.
 *    Mirror on: contacts are mapped onto the mirrored picture and re-injected on a uinput touchscreen
 *    "a6l-eink-rear-touch" sized like the front display (idc: internal, display 0). Mirror off: dropped.
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
 *        [--clear-every N] [--max-per-min N] [--active-mode M] [--fit letterbox|crop] [--threshold N] [--frames N]
 *        [--mode off|mirror] [--reading 0|1] [--no-props] [--key-dev auto|PATH|none] [--touch-dev auto|PATH|none]
 *        [--touch-transform T] [--front WxH] [--wait-prop NAME=VALUE] [--dry] [--ns-pid N] [--screencap P] [--out DIR]
 *        [--touch-debug] (print raw rear contacts "TOUCH_IN" and every injected event "TOUCH_OUT type code value")
 *   live properties (dualux, 25 Sep): persist.sys.a6l.eink.refresh (auto|quality|partial|fast|fastest, overrides .reading),
 *        persist.sys.a6l.eink.clear_every, vendor.eink.clear_req (any change = full clear + redraw; written by a6l_dualux),
 *        persist.sys.a6l.eink.contrast (0..100, black/white point stretch before the e-ink quantisation)
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

#define OW 720
#define OH 1440
#define TS 8
#define MAXRAW (64u << 20)
#define KEY_EINK 616

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double t_start;
static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("[%.3f] A6L_MIRROR %s\n", now() - t_start, b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    __android_log_write(strstr(b, "FAIL") ? ANDROID_LOG_ERROR : strstr(b, "WARN") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "a6l_eink", b);
#endif
}
#define LOG(...) logline(__VA_ARGS__)

static const char *epd_socket = "/dev/socket/a6l_epd", *outdir, *source = "drm", *screencap = "/system/bin/screencap", *display_id,
                  *key_dev = "auto", *touch_dev = "auto", *wait_prop;
static int interval_ms = 250, crop, threshold = 6, frames_max, dry, ns_pid = -1, use_props = 1, front_w = 1080, front_h = 2340;
static struct pol_cfg pcfg;
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

/* ---------------- properties ---------------- */
static int prop_get(const char *k, char *v, size_t n) {
#ifdef __ANDROID__
    char b[PROP_VALUE_MAX] = {0}; int l = __system_property_get(k, b); snprintf(v, n, "%s", b); return l;
#else
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
struct pl { uint32_t fb; int64_t zpos; int cx, cy, cw, ch; double sx, sy, sw, sh; };
static int cmp_pl(const void *a, const void *b) { const struct pl *x = a, *y = b; return x->zpos < y->zpos ? -1 : x->zpos > y->zpos; }
static int drm_allowed(void) {	/* --wait-prop NAME=VALUE: the card is only opened once the composer surely holds master */
    if (!wait_prop) return 1;
    const char *eq = strchr(wait_prop, '='); if (!eq) return 1;
    char k[96], v[96]; snprintf(k, sizeof k, "%.*s", (int)(eq - wait_prop), wait_prop); prop_get(k, v, sizeof v);
    if (strcmp(v, eq + 1)) { static double last; if (now() - last > 30) { last = now(); LOG("drm source: waiting for %s (now '%s')", wait_prop, v); } return 0; }
    return 1;
}
static int capture_drm(void) {
    if (drm_fd < 0 && !drm_allowed()) return CAP_FRONT_OFF;	/* treated like "front off": paused, not a failure */
    if (drm_fd < 0 && drm_open_once()) { LOG("WARN no KMS card with an LCD connector"); return -1; }
    uint32_t crtc = 0; int lw = 0, lh = 0;
    if (!lcd_crtc(&crtc, &lw, &lh)) return CAP_FRONT_OFF;
    if (lw != front_w || lh != front_h) { LOG("front display %dx%d", lw, lh); front_w = lw; front_h = lh; }
    if (gray_alloc(lw, lh)) return -1; memset(gray, 0, (size_t)gw * gh);
    drmModePlaneRes *pr = drmModeGetPlaneResources(drm_fd); struct pl pls[16]; int n = 0;
    for (unsigned i = 0; pr && i < pr->count_planes && n < 16; i++) { drmModePlane *p = drmModeGetPlane(drm_fd, pr->planes[i]);
        if (p && p->crtc_id == crtc && p->fb_id) { uint32_t id = p->plane_id; struct pl *q = &pls[n++]; q->fb = p->fb_id;
            q->zpos = (int64_t)prop_of(id, DRM_MODE_OBJECT_PLANE, "zpos", i); q->cx = (int)(int32_t)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0); q->cy = (int)(int32_t)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
            q->cw = (int)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_W", lw); q->ch = (int)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_H", lh);
            q->sx = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0) / 65536.0; q->sy = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0) / 65536.0;
            q->sw = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)lw << 16) / 65536.0; q->sh = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)lh << 16) / 65536.0; }
        drmModeFreePlane(p); }
    if (pr) drmModeFreePlaneResources(pr);
    if (!n) { LOG("WARN no plane on the LCD CRTC"); return -1; }
    qsort(pls, (size_t)n, sizeof pls[0], cmp_pl);
    for (int k = 0; k < n; k++) {
        struct pl *q = &pls[k]; drmModeFB2 *fb = drmModeGetFB2(drm_fd, q->fb); if (!fb) { LOG("WARN GETFB2 %u: %s", q->fb, strerror(errno)); return -1; }
        int ok = 1, bpp = 0, alpha = 0, bgr = 0; uint32_t f = fb->pixel_format;
        if (f == DRM_FORMAT_XRGB8888 || f == DRM_FORMAT_ARGB8888) { bpp = 4; alpha = f == DRM_FORMAT_ARGB8888; bgr = 1; }
        else if (f == DRM_FORMAT_XBGR8888 || f == DRM_FORMAT_ABGR8888) { bpp = 4; alpha = f == DRM_FORMAT_ABGR8888; }
        else if (f == DRM_FORMAT_RGB565) bpp = 2;
        if (!bpp) { static int w1; if (!w1++) LOG("WARN plane fb format %.4s unsupported", (char *)&f); ok = 0; }
        if ((fb->flags & DRM_MODE_FB_MODIFIERS) && fb->modifier != DRM_FORMAT_MOD_LINEAR) { static int warned; if (!warned++) LOG("WARN fb modifier 0x%llx (compressed/tiled): drm source cannot read it", (unsigned long long)fb->modifier); ok = 0; }
        if (!fb->handles[0]) { static int w2; if (!w2++) LOG("WARN GETFB2 returned no handle (needs CAP_SYS_ADMIN)"); ok = 0; }
        int dmafd = -1; uint8_t *map = MAP_FAILED; size_t len = (size_t)fb->pitches[0] * fb->height + fb->offsets[0];
        if (ok && !drmPrimeHandleToFD(drm_fd, fb->handles[0], DRM_CLOEXEC, &dmafd)) map = mmap(NULL, len, PROT_READ, MAP_SHARED, dmafd, 0);
        if (ok && map == MAP_FAILED) { LOG("WARN prime map fb %u: %s", q->fb, strerror(errno)); ok = 0; }
        if (ok) {
            struct dma_buf_sync sy = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ}; ioctl(dmafd, DMA_BUF_IOCTL_SYNC, &sy);
            double kx = q->sw / (q->cw ? q->cw : 1), ky = q->sh / (q->ch ? q->ch : 1);
            for (int y = q->cy < 0 ? 0 : q->cy; y < q->cy + q->ch && y < gh; y++) {
                int syy = (int)(q->sy + (y - q->cy + 0.5) * ky); if (syy < 0 || syy >= (int)fb->height) continue;
                const uint8_t *row = map + fb->offsets[0] + (size_t)syy * fb->pitches[0];
                for (int x = q->cx < 0 ? 0 : q->cx; x < q->cx + q->cw && x < gw; x++) {
                    int sxx = (int)(q->sx + (x - q->cx + 0.5) * kx); if (sxx < 0 || sxx >= (int)fb->width) continue;
                    const uint8_t *s = row + (size_t)sxx * bpp; uint8_t v, a = 255;
                    if (bpp == 4) { v = bgr ? luma(s[2], s[1], s[0]) : luma(s[0], s[1], s[2]); if (alpha) a = s[3]; }
                    else { uint16_t w16 = (uint16_t)(s[0] | s[1] << 8); v = luma((w16 >> 11) << 3, ((w16 >> 5) & 63) << 2, (w16 & 31) << 3); }
                    uint8_t *d = &gray[(size_t)y * gw + x];
                    int bl = v + (*d * (255 - a) + 127) / 255; *d = a == 255 ? v : (uint8_t)(bl > 255 ? 255 : bl);	/* premultiplied alpha */
                }
            }
            sy.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl(dmafd, DMA_BUF_IOCTL_SYNC, &sy);
        }
        if (map != MAP_FAILED) munmap(map, len);
        if (dmafd >= 0) close(dmafd);
        for (int h = 0; h < 4; h++) if (fb->handles[h]) { int dup = 0; for (int g = 0; g < h; g++) if (fb->handles[g] == fb->handles[h]) dup = 1;
            if (!dup) { struct drm_gem_close gc = {.handle = fb->handles[h]}; drmIoctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc); } }
        drmModeFreeFB2(fb);
        if (!ok) return -1;
    }
    return 0;
}
#else
static int capture_drm(void) { LOG("FAIL built without DRM"); return -1; }
#endif

/* ---------------- conversion: area average into 720x1440 (or 1440x720) ---------------- */
static uint8_t out[OW * OH]; static int out_w = OW, out_h = OH, geo_ox, geo_oy, geo_dw, geo_dh;
static float *acc; static size_t acc_n;
/* dualux (25 Sep): contrast LUT (persist.sys.a6l.eink.contrast 0..100, stock "high contrast text"): linear stretch
 * between a black point and a white point (0 = identity; 100 = 0..60 -> black, 195..255 -> white) */
static uint8_t lut[256]; static int lut_contrast = -1;
static void lut_build(int c) {
    if (c < 0) c = 0; if (c > 100) c = 100; lut_contrast = c;
    int bp = c * 60 / 100, wp = 255 - c * 60 / 100;
    for (int i = 0; i < 256; i++) { int v = i <= bp ? 0 : i >= wp ? 255 : (i - bp) * 255 / (wp - bp); lut[i] = (uint8_t)v; }
}
static void resample(void) {
    if (lut_contrast < 0) lut_build(0);
    int land = gw > gh; out_w = land ? OH : OW; out_h = land ? OW : OH;
    memset(out, 255, sizeof out);	/* letterbox bars = white (paper) */
    int dw, dh, ox, oy; fit_geometry(gw, gh, out_w, out_h, crop, &ox, &oy, &dw, &dh);
    geo_ox = ox; geo_oy = oy; geo_dw = dw; geo_dh = dh;
    size_t need = (size_t)dw * gh; if (need > acc_n) { free(acc); acc = malloc(need * sizeof *acc); acc_n = acc ? need : 0; if (!acc) return; }
    double fx = (double)gw / dw, fy = (double)gh / dh;
    for (int x = 0; x < dw; x++) {
        double a = x * fx, b = a + fx; int i0 = (int)a, i1 = (int)b; if (i1 >= gw) i1 = gw - 1;
        for (int y = 0; y < gh; y++) { const uint8_t *r = gray + (size_t)y * gw; double s = 0;
            if (i0 == i1) s = r[i0] * fx; else { s += r[i0] * (i0 + 1 - a); for (int i = i0 + 1; i < i1; i++) s += r[i]; if (b > i1) s += r[i1] * (b - i1); }
            acc[(size_t)y * dw + x] = (float)(s / fx); }
    }
    for (int y = 0; y < dh; y++) {
        int yy = y + oy; if (yy < 0 || yy >= out_h) continue;
        double a = y * fy, b = a + fy; int j0 = (int)a, j1 = (int)b; if (j1 >= gh) j1 = gh - 1;
        for (int x = 0; x < dw; x++) { int xx = x + ox; if (xx < 0 || xx >= out_w) continue; double s;
            if (j0 == j1) s = acc[(size_t)j0 * dw + x] * fy; else { s = acc[(size_t)j0 * dw + x] * (j0 + 1 - a); for (int j = j0 + 1; j < j1; j++) s += acc[(size_t)j * dw + x]; if (b > j1) s += acc[(size_t)j1 * dw + x] * (b - j1); }
            int v = (int)(s / fy + 0.5); out[(size_t)yy * out_w + xx] = lut[v > 255 ? 255 : v < 0 ? 0 : v]; }
    }
}
#define NT ((OW / TS) * (OH / TS))
static uint8_t cur_t[NT], prev_t[NT], shown_t[NT]; static int have_prev, have_shown, cur_land, shown_land, prev_land;
static void tiles(uint8_t *t) {
    int tw = out_w / TS, th = out_h / TS;
    for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) { unsigned s = 0;
        for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++) s += out[(size_t)(ty * TS + y) * out_w + tx * TS + x];
        t[ty * tw + tx] = (uint8_t)(s / (TS * TS)); }
}
static double diff_frac(const uint8_t *a, const uint8_t *b) { int c = 0; for (int i = 0; i < NT; i++) if (abs(a[i] - b[i]) > threshold) c++; return (double)c / NT; }

/* ---------------- a6l_epdd client (socket; one outstanding command, replies are lines) ---------------- */
static int epd = -1; static char rbuf[1024]; static size_t rlen; static double last_connect_try;
static int epd_connect(void) {
    if (epd >= 0 || dry) return 0;
    if (now() - last_connect_try < 2.0) return -1;
    last_connect_try = now();
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", epd_socket);
    if (s < 0 || connect(s, (struct sockaddr *)&a, sizeof a)) { static int warned; if (!warned++) LOG("WARN a6l_epdd socket %s: %s (retrying every 2 s)", epd_socket, strerror(errno)); if (s >= 0) close(s); return -1; }
    epd = s; rlen = 0; LOG("connected to a6l_epdd (%s)", epd_socket); return 0;
}
static void epd_drop(void) { if (epd >= 0) close(epd); epd = -1; rlen = 0; }
static int send_all(const void *p, size_t n) {
    const uint8_t *b = p; while (n) { ssize_t w = send(epd, b, n, MSG_NOSIGNAL); if (w <= 0) { if (w < 0 && errno == EINTR) continue; return -1; } b += w; n -= (size_t)w; } return 0;
}
/* command queue: at most 2 (clear + frame); busy while one is outstanding */
struct qcmd { char line[96]; int frame; };
static struct qcmd q[2]; static int qn, busy; static double cmd_t, dry_done_at;
static uint8_t qframe[OW * OH]; static int qfw, qfh;
static int epd_send(const struct qcmd *c) {
    if (dry) {
        printf("A6L_MIRROR CMD %s\n", c->line); fflush(stdout);
        if (c->frame && outdir) { static int k; char p[600]; snprintf(p, sizeof p, "%s/f%d.pgm", outdir, k++); FILE *f = fopen(p, "wb");
            if (f) { fprintf(f, "P5\n%d %d\n255\n", qfw, qfh); if (fwrite(qframe, 1, (size_t)qfw * qfh, f) != (size_t)qfw * qfh) LOG("WARN short %s", p); fclose(f); } }
        dry_done_at = now() + (strstr(c->line, "clear") ? 2.0 : strstr(c->line, "fastest") ? 0.55 : 0.85);
        busy = 1; cmd_t = now(); return 0;
    }
    if (epd_connect()) return -1;
    char l[128]; int n = snprintf(l, sizeof l, "%s\n", c->line);
    if (send_all(l, (size_t)n) || (c->frame && send_all(qframe, (size_t)qfw * qfh))) { LOG("WARN a6l_epdd write failed: %s", strerror(errno)); epd_drop(); return -1; }
    busy = 1; cmd_t = now(); LOG("cmd: %s", c->line); return 0;
}
static void queue_cmd(const char *line, int with_frame) {
    if (qn >= 2) return; snprintf(q[qn].line, sizeof q[qn].line, "%s", line); q[qn].frame = with_frame; qn++;
}
static void pump(void) {	/* send the next queued command when idle */
    if (busy || !qn) return;
    if (epd_send(&q[0])) { if (!dry && epd < 0) { qn = 0; } return; }
    memmove(&q[0], &q[1], sizeof q[0]); qn--;
}
static int on_reply(char *line, struct pol_state *ps) {	/* returns 1 if a command completed */
    LOG("epdd: %s (%.2f s)", line, now() - cmd_t);
    busy = 0; pol_done(ps, now()); return 1;
}
static void epd_input(struct pol_state *ps) {
    ssize_t r = recv(epd, rbuf + rlen, sizeof rbuf - 1 - rlen, 0);
    if (r <= 0) { LOG("WARN a6l_epdd closed the connection"); epd_drop(); busy = 0; qn = 0; return; }
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
static int open_input(const char *spec, int want_key, const char *what) {
    char p[400] = ""; int fd = -1;
    if (!strcmp(spec, "none")) return -1;
    if (!strcmp(spec, "auto")) fd = find_input(want_key, p, sizeof p);
    else { fd = open(spec, O_RDONLY | O_CLOEXEC | O_NONBLOCK); snprintf(p, sizeof p, "%s", spec); }
    if (fd < 0) LOG("WARN %s: no input device (%s)", what, spec); else LOG("%s: %s", what, p);
    return fd;
}
#define NSLOT 10
struct slot { int in_active, x, y, dirty, out_active, ignored; };
static struct slot sl[NSLOT]; static int cur_slot, next_tid = 1, ui = -1, touch_fd = -1, forward;
static struct tmap tm;
static int touch_debug;
static void emit(int type, int code, int val) {
    if (touch_debug) printf("A6L_MIRROR TOUCH_OUT %d %d %d\n", type, code, val);
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
static void touch_release_all(void) {
    int any = 0; for (int i = 0; i < NSLOT; i++) if (sl[i].out_active) { emit(EV_ABS, ABS_MT_SLOT, i); emit(EV_ABS, ABS_MT_TRACKING_ID, -1); sl[i].out_active = 0; any = 1; }
    for (int i = 0; i < NSLOT; i++) sl[i].ignored = sl[i].in_active;	/* contacts still down stay ignored until lifted */
    if (any) { emit(EV_KEY, BTN_TOUCH, 0); emit(EV_SYN, SYN_REPORT, 0); }
}
static void touch_flush(void) {
    int changed = 0, down = 0;
    for (int i = 0; i < NSLOT; i++) {
        struct slot *s = &sl[i]; if (!s->dirty) { down |= s->out_active; continue; } s->dirty = 0;
        if (!s->in_active) { if (s->out_active) { emit(EV_ABS, ABS_MT_SLOT, i); emit(EV_ABS, ABS_MT_TRACKING_ID, -1); changed = 1; } s->out_active = 0; s->ignored = 0; continue; }
        int fx, fy, inside = tmap_apply(&tm, s->x, s->y, &fx, &fy);
        if (touch_debug) printf("A6L_MIRROR TOUCH_IN slot %d raw %d %d -> front %d %d %s%s\n", i, s->x, s->y, fx, fy, inside ? "inside" : "outside", forward ? "" : " (not forwarded)");
        if (!forward || s->ignored) continue;
        if (!s->out_active) { if (!inside) { s->ignored = 1; continue; } s->out_active = 1; emit(EV_ABS, ABS_MT_SLOT, i); emit(EV_ABS, ABS_MT_TRACKING_ID, next_tid++ & 0xffff); }
        else emit(EV_ABS, ABS_MT_SLOT, i);
        emit(EV_ABS, ABS_MT_POSITION_X, fx); emit(EV_ABS, ABS_MT_POSITION_Y, fy); changed = 1; down = 1;
        if (i == 0 || !sl[0].out_active) { emit(EV_ABS, ABS_X, fx); emit(EV_ABS, ABS_Y, fy); }
    }
    for (int i = 0; i < NSLOT; i++) down |= sl[i].out_active;
    if (changed) { emit(EV_KEY, BTN_TOUCH, down); emit(EV_SYN, SYN_REPORT, 0); }
}
static void touch_input(void) {
    struct input_event ev[64]; ssize_t r = read(touch_fd, ev, sizeof ev);
    if (r < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (r <= 0) { LOG("WARN rear touch read: %s", r ? strerror(errno) : "EOF"); close(touch_fd); touch_fd = -1; return; }
    for (int i = 0; i < (int)(r / (ssize_t)sizeof ev[0]); i++) {
        struct input_event *e = &ev[i];
        if (e->type == EV_ABS) {
            if (e->code == ABS_MT_SLOT) { cur_slot = e->value >= 0 && e->value < NSLOT ? e->value : 0; continue; }
            struct slot *s = &sl[cur_slot];
            if (e->code == ABS_MT_TRACKING_ID) { s->in_active = e->value >= 0; s->dirty = 1; }
            else if (e->code == ABS_MT_POSITION_X) { s->x = e->value; s->dirty = 1; }
            else if (e->code == ABS_MT_POSITION_Y) { s->y = e->value; s->dirty = 1; }
        } else if (e->type == EV_SYN && e->code == SYN_REPORT) touch_flush();
        else if (e->type == EV_SYN && e->code == SYN_DROPPED) { for (int k = 0; k < NSLOT; k++) { sl[k].in_active = 0; sl[k].dirty = 1; } touch_flush(); }
    }
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

int main(int argc, char **argv) {
    t_start = now(); pol_default_cfg(&pcfg);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--epd-socket")) epd_socket = v; else if (OPT("--out")) outdir = v; else if (OPT("--source")) source = v;
        else if (OPT("--interval")) interval_ms = atoi(v); else if (OPT("--quiet")) pcfg.quiet_ms = atoi(v); else if (OPT("--settle")) pcfg.settle_ms = atoi(v);
        else if (OPT("--min-gap")) pcfg.min_gap_ms = atoi(v); else if (OPT("--clear-every")) pcfg.clear_every = atoi(v); else if (OPT("--max-per-min")) pcfg.max_per_min = atoi(v);
        else if (OPT("--active-mode")) pcfg.active_mode = v; else if (OPT("--fit")) crop = !strcmp(v, "crop"); else if (OPT("--threshold")) threshold = atoi(v);
        else if (OPT("--frames")) frames_max = atoi(v); else if (!strcmp(a, "--dry")) dry = 1; else if (OPT("--ns-pid")) ns_pid = atoi(v);
        else if (OPT("--screencap")) screencap = v; else if (OPT("--display")) display_id = v;
        else if (OPT("--mode")) mode_forced = mode_parse(v, EINK_MIRROR); else if (OPT("--reading")) reading_forced = atoi(v) != 0;
        else if (!strcmp(a, "--no-props")) use_props = 0; else if (OPT("--key-dev")) key_dev = v; else if (OPT("--touch-dev")) touch_dev = v;
        else if (OPT("--touch-transform")) snprintf(transform_forced, sizeof transform_forced, "%s", v);
        else if (OPT("--front")) { if (sscanf(v, "%dx%d", &front_w, &front_h) != 2) return 2; }
        else if (OPT("--wait-prop")) wait_prop = v; else if (!strcmp(a, "--touch-debug")) touch_debug = 1;
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
    fit_geometry(front_w, front_h, OW, OH, crop, &tm.img_x, &tm.img_y, &tm.img_w, &tm.img_h);
    int mc, rc_; if (mode_forced < 0 && use_props) { char v[96]; if (prop_get(P_MODE, v, sizeof v) <= 0) { mode = EINK_OFF; } } read_props(&mc, &rc_);
    key_fd = open_input(key_dev, 1, "e-ink key");
    touch_fd = open_input(touch_dev, 0, "rear touch");
    if (touch_fd >= 0) { if (ioctl(touch_fd, EVIOCGRAB, 1)) LOG("WARN EVIOCGRAB rear touch: %s", strerror(errno)); else LOG("rear touch grabbed (Android no longer sees it directly)"); }
    if (touch_fd >= 0 && strcmp(touch_dev, "none")) ui = uinput_open();
    pcfg.reading = reading;
    struct pol_state ps; pol_init(&ps, &pcfg, now());
    int active = 0, paused = 0, frame = 0, fails = 0, enter_clear = 0; double next_cap = now(), next_props = now();
    LOG("start: source=%s interval=%d mode=%s reading=%d clear-every=%d active=%s fit=%s%s", source, interval_ms, mode_name(mode), reading, pcfg.clear_every, pcfg.active_mode, crop ? "crop" : "letterbox", dry ? " DRY" : "");
    while (!stop && (!frames_max || frame < frames_max)) {
        double t = now();
        /* mode transitions */
        if (t >= next_props) { next_props = t + 0.5; int m1, r1; read_props(&m1, &r1);
            if (r1 && !refresh_mode[0]) { pcfg.reading = reading; ps.cfg.reading = reading; LOG("reading mode %s", reading ? "on" : "off"); }
            int rc1, ce1; if (read_live_props(&rc1, &ce1)) { LOG("clear requested (%s=%s)", P_CLEAR_REQ, clear_req); if (qn < 2) queue_cmd("clear", 0); if (active) have_shown = 0; }
            if (ce1) { pcfg.clear_every = ps.cfg.clear_every = clear_every_prop; LOG("clear every %d clean updates", clear_every_prop); }
            if (rc1 || (r1 && refresh_mode[0])) { struct pol_cfg nc = pcfg;
                if (!pol_apply_refresh_mode(&nc, refresh_mode[0] ? refresh_mode : reading ? "partial" : "auto")) { pcfg = nc; ps.cfg = nc; LOG("refresh mode %s", refresh_mode[0] ? refresh_mode : "auto (default)"); }
                else LOG("WARN unknown %s '%s' (auto|quality|partial|fast|fastest)", P_REFRESH, refresh_mode); } }
        if (mode == EINK_MIRROR && !active) { active = 1; enter_clear = 1; if (!refresh_mode[0]) pcfg.reading = reading; pol_init(&ps, &pcfg, t); have_prev = have_shown = 0; forward = 1; LOG("mirror ON%s", reading ? " (reading)" : ""); next_cap = t; }
        if (mode != EINK_MIRROR && active) { active = 0; forward = 0; touch_release_all(); qn = 0; queue_cmd("power off", 0); LOG("mirror OFF (e-ink keeps the last picture)"); }
        if (!dry && epd < 0) epd_connect();
        set_state(!active ? "off" : epd < 0 && !dry ? "mirror-no-epdd" : paused ? "mirror-paused" : reading ? "mirror-reading" : "mirror");
        /* wait for input / replies / the next capture */
        struct pollfd p[3]; int n = 0, ik = -1, it = -1, ie = -1;
        if (key_fd >= 0) { ik = n; p[n++] = (struct pollfd){key_fd, POLLIN, 0}; }
        if (touch_fd >= 0) { it = n; p[n++] = (struct pollfd){touch_fd, POLLIN, 0}; }
        if (epd >= 0) { ie = n; p[n++] = (struct pollfd){epd, POLLIN, 0}; }
        double until = active ? next_cap : t + 0.5; if (ks.down) until = t + 0.05; if (dry && busy && dry_done_at < until) until = dry_done_at; if (next_props < until) until = next_props;
        int to = (int)((until - now()) * 1000); if (to < 0) to = 0;
        int r = poll(p, (nfds_t)n, to);
        if (r < 0 && errno != EINTR) { LOG("FAIL poll: %s", strerror(errno)); break; }
        if (r > 0 && ie >= 0 && (p[ie].revents & (POLLIN | POLLHUP | POLLERR))) epd_input(&ps);
        if (r > 0 && it >= 0 && (p[it].revents & (POLLIN | POLLHUP | POLLERR))) touch_input();
        enum key_action ka = KA_NONE;
        if (r > 0 && ik >= 0 && (p[ik].revents & (POLLIN | POLLHUP | POLLERR))) ka = key_input();
        if (!ka) ka = key_tick(&ks, now());
        if (ka == KA_TOGGLE) { int nm = mode == EINK_MIRROR ? EINK_OFF : EINK_MIRROR; LOG("e-ink key: %s", mode_name(nm));
            if (mode_forced >= 0) mode_forced = nm; mode = nm; if (use_props) prop_set(P_MODE, mode_name(nm)); }
        if (ka == KA_CLEAR) { LOG("e-ink key long press: clear"); if (qn < 2) queue_cmd("clear", 0); if (active) { have_shown = 0; } }
        if (dry && busy && now() >= dry_done_at) { busy = 0; pol_done(&ps, now()); LOG("done (simulated)"); }
        pump();
        if (!active || now() < next_cap) continue;
        /* capture + policy */
        next_cap += interval_ms / 1000.0; if (next_cap < now()) next_cap = now() + interval_ms / 1000.0;
        double t0 = now(); int rc;
        if (!strcmp(source, "screencap")) rc = capture_screencap();
        else if (!strcmp(source, "drm")) rc = capture_drm();
        else if (!strncmp(source, "file:", 5)) rc = capture_file(source + 5);
        else if (!strncmp(source, "files:", 6)) { static int last_ok; char pth[512]; snprintf(pth, sizeof pth, source + 6, frame);
            if (!access(pth, R_OK)) last_ok = frame; else snprintf(pth, sizeof pth, source + 6, last_ok); rc = capture_file(pth); }
        else { LOG("FAIL unknown source %s", source); return 2; }
        frame++;
        if (rc == CAP_FRONT_OFF) { if (!paused) { LOG("front screen off: paused (rear touch not forwarded)"); forward = 0; touch_release_all(); } paused = 1; continue; }
        if (rc) { if (++fails >= 40) { LOG("FAIL 40 consecutive capture failures: pausing 30 s"); fails = 0; next_cap = now() + 30; } continue; }
        if (paused) { LOG("front screen on: resumed"); paused = 0; forward = 1; }
        fails = 0; double tcap = now() - t0;
        resample(); cur_land = out_w > out_h; tiles(cur_t);
        if (!cur_land && (gw != tm.front_w || gh != tm.front_h || geo_ox != tm.img_x || geo_dw != tm.img_w)) {	/* keep the touch map on the picture actually shown */
            tm.front_w = gw; tm.front_h = gh; tm.img_x = geo_ox; tm.img_y = geo_oy; tm.img_w = geo_dw; tm.img_h = geo_dh; }
        double moving = have_prev && prev_land == cur_land ? diff_frac(cur_t, prev_t) : 1.0;
        memcpy(prev_t, cur_t, NT); have_prev = 1; prev_land = cur_land;
        double vs_panel = have_shown && shown_land == cur_land ? diff_frac(cur_t, shown_t) : 1.0;
        if (frame <= 3 || frame % 240 == 0) LOG("frame %d: %dx%d capture %.0f ms, moving %.3f, vs panel %.3f", frame, gw, gh, tcap * 1000, moving, vs_panel);
        if (!dry && epd < 0) continue;
        struct pol_action a = pol_step(&ps, now(), moving, vs_panel, busy || qn > 0);
        if (enter_clear && !busy && !qn) { a.kind = POL_CLEAR_THEN_SHOW; a.mode = reading ? POL_QUALITY : POL_QUALITY; enter_clear = 0; }
        if (a.kind == POL_NONE) continue;
        char line[96];
        if (a.kind == POL_REFRESH) queue_cmd("refresh", 0);
        else {
            memcpy(qframe, out, (size_t)out_w * out_h); qfw = out_w; qfh = out_h;
            if (a.kind == POL_CLEAR_THEN_SHOW) queue_cmd("clear", 0);
            snprintf(line, sizeof line, "frame %d %d %s", out_w, out_h, a.mode); queue_cmd(line, 1);
        }
        pol_sent(&ps, &a, now()); memcpy(shown_t, cur_t, NT); have_shown = 1; shown_land = cur_land;
        pump();
    }
    while (!stop && busy && dry) { usleep(20000); if (now() >= dry_done_at) busy = 0; }
    if (ui >= 0) { touch_release_all(); ioctl(ui, UI_DEV_DESTROY); close(ui); }
    LOG("exit after %d frames", frame);
    return 0;
}
