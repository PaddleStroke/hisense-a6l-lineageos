// SPDX-License-Identifier: Apache-2.0
/* a6l_eink_mirror — mirror the Hisense A6L front screen onto the rear e-ink (milestone M1, 23 Sep 2026).
 *
 * Polls the front screen, converts it to a 720x1440 portrait (or 1440x720 landscape) 8-bit PGM by area averaging,
 * detects changes on 8x8 tiles, and drives a6l_epdd (v2/v3) through its command FIFO with an automatic mode policy:
 *   - a discrete change (content quiet again after --quiet ms) -> one "show <pgm> quality";
 *   - content that keeps changing (>= 2 consecutive changed captures) -> burst: "show <pgm> fastest" (or --active-mode)
 *     at most every --min-gap ms, latest frame wins (only one command outstanding at a time);
 *   - when a burst settles (no change for --settle ms, default 1000) -> one quality update of the settled picture
 *     ("refresh" = forced GC16 when the settled picture is exactly the one last sent in fast mode, since the library
 *     skips repeated images);
 *   - every --clear-every quality updates (default 10) -> "clear" then the quality picture;
 *   - rate limit: never more than --max-per-min updates per minute (default 60), --min-gap between commands.
 * Completion of each command is taken from a6l_epdd's log (--epdd-log: counts "shown in" / "FAIL" lines), otherwise
 * estimated from the measured update durations.
 *
 * Front-screen sources (--source):
 *   screencap (default): runs /system/bin/screencap (raw RGBA) inside SurfaceFlinger's mount namespace (setns to
 *              /proc/<surfaceflinger>/ns/mnt, SF's own environment), so it works from the recovery shell next to the
 *              framework session (private /dev/binder, bind-mounted /system) without being part of it. Root only.
 *   drm:       reads the LCD CRTC's planes directly (GETFB2 + PRIME mmap; needs CAP_SYS_ADMIN, not DRM master) and
 *              composites them by zpos. Only linear 32/16-bit buffers (a UBWC/compressed modifier is refused).
 *   file:PATH  a raw screencap file (tests); files:PATTERN a printf pattern with %d (frame 0,1,2,... last repeats).
 *
 * usage: a6l_eink_mirror [--fifo /tmp/epd/cmd] [--epdd-log F] [--out /tmp/epd/mirror] [--source S] [--interval 250]
 *        [--quiet 300] [--settle 1000] [--min-gap 150] [--clear-every 10] [--max-per-min 60] [--active-mode fastest]
 *        [--fit letterbox|crop] [--threshold 6] [--frames N] [--dry] [--ns-pid N] [--screencap PATH] [--display ID]
 *   --dry: print commands (A6L_MIRROR CMD ...) instead of writing the FIFO; completion simulated.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifndef NO_DRM
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/dma-buf.h>
#endif

#define OW 720
#define OH 1440
#define TS 8
#define MAXRAW (64u << 20)

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static double t_start;
#define LOG(...) do { printf("[%.3f] A6L_MIRROR ", now() - t_start); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static const char *fifo = "/tmp/epd/cmd", *epdd_log, *outdir = "/tmp/epd/mirror", *source = "screencap", *screencap = "/system/bin/screencap", *active_mode = "fastest", *display_id;
static int interval_ms = 250, quiet_ms = 300, settle_ms = 1000, min_gap_ms = 150, clear_every = 10, max_per_min = 60, crop, threshold = 6, frames_max, dry, ns_pid = -1;
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }

/* ---------------- gray canvas (front screen, full resolution) ---------------- */
static uint8_t *gray; static int gw, gh;
static int gray_alloc(int w, int h) {
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192) return -1;
    if (w != gw || h != gh) { free(gray); gray = malloc((size_t)w * h); if (!gray) return -1; gw = w; gh = h; }
    return 0;
}
static inline uint8_t luma(int r, int g, int b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }

/* ---------------- source: screencap raw ---------------- */
static char **sf_env; static int sf_pid_cached = -1;
static int find_pid(const char *comm) {
    DIR *d = opendir("/proc"); struct dirent *e; int found = -1;
    while (d && (e = readdir(d))) { int pid = atoi(e->d_name); if (pid <= 0) continue; char p[64], c[64] = {0};
        snprintf(p, sizeof p, "/proc/%d/comm", pid); FILE *f = fopen(p, "r"); if (!f) continue;
        if (fgets(c, sizeof c, f)) { char *nl = strchr(c, '\n'); if (nl) *nl = 0; if (!strcmp(c, comm)) found = pid; } fclose(f); if (found > 0) break; }
    if (d) closedir(d); return found;
}
static char **read_environ(int pid) {	/* SF's environment (ANDROID_ROOT, BOOTCLASSPATH, ...), NULL-terminated */
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
/* raw screencap: u32 w, h, format [, dataspace] then w*h*bpp bytes (stride removed). header size from the length. */
static int parse_raw(void) {
    if (raw_len < 16) { LOG("FAIL screencap output too short (%zu bytes)", raw_len); return -1; }
    uint32_t w, h, f; memcpy(&w, raw, 4); memcpy(&h, raw + 4, 4); memcpy(&f, raw + 8, 4);
    int bpp = (f == 1 || f == 2 || f == 5) ? 4 : f == 3 ? 3 : f == 4 ? 2 : 0;
    if (!bpp || w == 0 || h == 0 || w > 8192 || h > 8192) { LOG("FAIL screencap header w=%u h=%u format=%u", w, h, f); return -1; }
    size_t px = (size_t)w * h * bpp; if (raw_len < px + 12) { LOG("FAIL screencap short: %zu < %zu", raw_len, px + 12); return -1; }
    size_t hdr = raw_len - px; if (hdr != 12 && hdr != 16) { LOG("WARN screencap header %zu bytes (expected 12/16)", hdr); if (hdr > 64) return -1; }
    if (gray_alloc((int)w, (int)h)) return -1;
    const uint8_t *s = raw + hdr;
    for (size_t i = 0; i < (size_t)w * h; i++, s += bpp) {
        if (f == 5) gray[i] = luma(s[2], s[1], s[0]);		/* BGRA */
        else if (bpp == 4 || bpp == 3) gray[i] = luma(s[0], s[1], s[2]);	/* RGBA / RGBX / RGB */
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

/* ---------------- source: DRM planes of the LCD CRTC ---------------- */
#ifndef NO_DRM
static int drm_fd = -1; static uint32_t lcd_crtc; static int lcd_w, lcd_h;
static uint64_t prop_of(uint32_t obj, uint32_t type, const char *name, uint64_t def) {
    drmModeObjectProperties *pp = drmModeObjectGetProperties(drm_fd, obj, type); uint64_t v = def;
    for (unsigned j = 0; pp && j < pp->count_props; j++) { drmModePropertyRes *p = drmModeGetProperty(drm_fd, pp->props[j]);
        if (p && !strcmp(p->name, name)) v = pp->prop_values[j]; drmModeFreeProperty(p); }
    drmModeFreeObjectProperties(pp); return v;
}
static int drm_init(void) {
    for (int c = 0; c < 4 && !lcd_crtc; c++) {
        char path[32]; snprintf(path, sizeof path, "/dev/dri/card%d", c); int fd = open(path, O_RDWR | O_CLOEXEC); if (fd < 0) continue;
        drmDropMaster(fd);	/* never keep master by accident (would lock the composer out) */
        drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);
        drmModeRes *res = drmModeGetResources(fd);
        for (int i = 0; res && i < res->count_connectors && !lcd_crtc; i++) { drmModeConnector *k = drmModeGetConnector(fd, res->connectors[i]);
            if (k && k->connection == DRM_MODE_CONNECTED && k->count_modes && k->modes[0].hdisplay != 384 && k->encoder_id) {
                drmModeEncoder *e = drmModeGetEncoder(fd, k->encoder_id); if (e && e->crtc_id) { drmModeCrtc *cr = drmModeGetCrtc(fd, e->crtc_id);
                    if (cr && cr->mode_valid) { lcd_crtc = e->crtc_id; lcd_w = cr->mode.hdisplay; lcd_h = cr->mode.vdisplay; drm_fd = fd; LOG("DRM source: %s crtc %u %dx%d", path, lcd_crtc, lcd_w, lcd_h); }
                    drmModeFreeCrtc(cr); } drmModeFreeEncoder(e); }
            drmModeFreeConnector(k); }
        if (res) drmModeFreeResources(res);
        if (!lcd_crtc) close(fd);
    }
    if (!lcd_crtc) { LOG("FAIL no active LCD CRTC"); return -1; }
    return 0;
}
struct pl { uint32_t fb; int64_t zpos; int cx, cy, cw, ch; double sx, sy, sw, sh; };
static int cmp_pl(const void *a, const void *b) { const struct pl *x = a, *y = b; return x->zpos < y->zpos ? -1 : x->zpos > y->zpos; }
static int capture_drm(void) {
    if (drm_fd < 0 && drm_init()) return -1;
    if (gray_alloc(lcd_w, lcd_h)) return -1; memset(gray, 0, (size_t)gw * gh);
    drmModePlaneRes *pr = drmModeGetPlaneResources(drm_fd); struct pl pls[16]; int n = 0;
    for (unsigned i = 0; pr && i < pr->count_planes && n < 16; i++) { drmModePlane *p = drmModeGetPlane(drm_fd, pr->planes[i]);
        if (p && p->crtc_id == lcd_crtc && p->fb_id) { uint32_t id = p->plane_id; struct pl *q = &pls[n++]; q->fb = p->fb_id;
            q->zpos = (int64_t)prop_of(id, DRM_MODE_OBJECT_PLANE, "zpos", i); q->cx = (int)(int32_t)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_X", 0); q->cy = (int)(int32_t)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_Y", 0);
            q->cw = (int)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_W", lcd_w); q->ch = (int)prop_of(id, DRM_MODE_OBJECT_PLANE, "CRTC_H", lcd_h);
            q->sx = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_X", 0) / 65536.0; q->sy = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_Y", 0) / 65536.0;
            q->sw = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_W", (uint64_t)lcd_w << 16) / 65536.0; q->sh = prop_of(id, DRM_MODE_OBJECT_PLANE, "SRC_H", (uint64_t)lcd_h << 16) / 65536.0; }
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
        if (!bpp) { LOG("WARN plane fb format %.4s unsupported", (char *)&f); ok = 0; }
        if ((fb->flags & DRM_MODE_FB_MODIFIERS) && fb->modifier != DRM_FORMAT_MOD_LINEAR) { static int warned; if (!warned++) LOG("WARN fb modifier 0x%llx (compressed/tiled): use --source screencap", (unsigned long long)fb->modifier); ok = 0; }
        if (!fb->handles[0]) { LOG("WARN GETFB2 returned no handle (needs CAP_SYS_ADMIN)"); ok = 0; }
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
static uint8_t out[OW * OH]; static int out_w = OW, out_h = OH;
static float *acc; static size_t acc_n;
static void resample(void) {
    int land = gw > gh; out_w = land ? OH : OW; out_h = land ? OW : OH;
    memset(out, 255, sizeof out);	/* letterbox bars = white (paper) */
    double sc = crop ? (double)out_w / gw > (double)out_h / gh ? (double)out_w / gw : (double)out_h / gh
                     : (double)out_w / gw < (double)out_h / gh ? (double)out_w / gw : (double)out_h / gh;
    int dw = (int)(gw * sc + 0.5), dh = (int)(gh * sc + 0.5); if (dw > out_w && !crop) dw = out_w; if (dh > out_h && !crop) dh = out_h;
    int ox = (out_w - dw) / 2, oy = (out_h - dh) / 2;	/* negative when cropping */
    /* pass 1: horizontal area average of every source row into dw columns */
    size_t need = (size_t)dw * gh; if (need > acc_n) { free(acc); acc = malloc(need * sizeof *acc); acc_n = acc ? need : 0; if (!acc) return; }
    double fx = (double)gw / dw, fy = (double)gh / dh;
    for (int x = 0; x < dw; x++) {
        double a = x * fx, b = a + fx; int i0 = (int)a, i1 = (int)b; if (i1 >= gw) i1 = gw - 1;
        for (int y = 0; y < gh; y++) { const uint8_t *r = gray + (size_t)y * gw; double s = 0;
            if (i0 == i1) s = r[i0] * fx; else { s += r[i0] * (i0 + 1 - a); for (int i = i0 + 1; i < i1; i++) s += r[i]; if (b > i1) s += r[i1] * (b - i1); }
            acc[(size_t)y * dw + x] = (float)(s / fx); }
    }
    /* pass 2: vertical */
    for (int y = 0; y < dh; y++) {
        int yy = y + oy; if (yy < 0 || yy >= out_h) continue;
        double a = y * fy, b = a + fy; int j0 = (int)a, j1 = (int)b; if (j1 >= gh) j1 = gh - 1;
        for (int x = 0; x < dw; x++) { int xx = x + ox; if (xx < 0 || xx >= out_w) continue; double s;
            if (j0 == j1) s = acc[(size_t)j0 * dw + x] * fy; else { s = acc[(size_t)j0 * dw + x] * (j0 + 1 - a); for (int j = j0 + 1; j < j1; j++) s += acc[(size_t)j * dw + x]; if (b > j1) s += acc[(size_t)j1 * dw + x] * (b - j1); }
            int v = (int)(s / fy + 0.5); out[(size_t)yy * out_w + xx] = (uint8_t)(v > 255 ? 255 : v < 0 ? 0 : v); }
    }
}

/* ---------------- change detection on 8x8 tile means ---------------- */
#define NT ((OW / TS) * (OH / TS))
static uint8_t cur_t[NT], prev_t[NT], shown_t[NT]; static int have_prev, have_shown, cur_land, shown_land, prev_land;
static void tiles(uint8_t *t) {
    int tw = out_w / TS, th = out_h / TS;
    for (int ty = 0; ty < th; ty++) for (int tx = 0; tx < tw; tx++) { unsigned s = 0;
        for (int y = 0; y < TS; y++) for (int x = 0; x < TS; x++) s += out[(size_t)(ty * TS + y) * out_w + tx * TS + x];
        t[ty * tw + tx] = (uint8_t)(s / (TS * TS)); }
}
static double diff_frac(const uint8_t *a, const uint8_t *b) { int c = 0; for (int i = 0; i < NT; i++) if (abs(a[i] - b[i]) > threshold) c++; return (double)c / NT; }

/* ---------------- epdd command channel + completion tracking ---------------- */
static int ff = -1; static FILE *logf; static int outstanding; static double cmd_t, cmd_deadline, done_t;
static double est_fast = 0.55, est_quality = 0.85, est_clear = 2.0;	/* seconds, refined from the epdd log */
static int fifo_open(void) {
    if (ff >= 0) return 0; if (dry) return 0;
    ff = open(fifo, O_WRONLY | O_NONBLOCK | O_CLOEXEC); if (ff < 0) return -1;	/* ENXIO until a6l_epdd reads it */
    fcntl(ff, F_SETFL, fcntl(ff, F_GETFL) & ~O_NONBLOCK); LOG("connected to %s", fifo); return 0;
}
static int send_cmd(const char *c, int expect_updates, double est) {
    if (dry) printf("A6L_MIRROR CMD %s\n", c);
    else { if (fifo_open()) { LOG("WARN fifo %s not ready: %s", fifo, strerror(errno)); return -1; }
        char line[700]; int n = snprintf(line, sizeof line, "%s\n", c);
        if (write(ff, line, (size_t)n) != n) { LOG("WARN fifo write: %s (a6l_epdd gone?)", strerror(errno)); close(ff); ff = -1; return -1; } }
    LOG("cmd: %s", c); outstanding = expect_updates; cmd_t = now(); cmd_deadline = cmd_t + (logf ? 12.0 : est);
    return 0;
}
static void poll_done(void) {
    if (!outstanding) return;
    if (logf) { char l[1024];
        while (outstanding && fgets(l, sizeof l, logf)) {
            if (strstr(l, " shown in ") || strstr(l, "FAIL")) { outstanding--; if (strstr(l, "FAIL")) LOG("epdd reported: %s", l); }
            else if (strstr(l, "unknown command")) { outstanding = 0; LOG("epdd: %s", l); } }
        clearerr(logf); }
    if (outstanding && now() > cmd_deadline) { if (logf) LOG("WARN no completion seen after %.1f s, continuing", now() - cmd_t); outstanding = 0; }
    if (!outstanding) { done_t = now(); LOG("done in %.2f s", done_t - cmd_t); }
}
static int write_pgm(char *path, size_t n) {
    static int k; snprintf(path, n, "%s/f%d.pgm", outdir, k ^= 1); char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) { LOG("FAIL %s: %s", tmp, strerror(errno)); return -1; }
    fprintf(f, "P5\n%d %d\n255\n", out_w, out_h); int ok = fwrite(out, 1, (size_t)out_w * out_h, f) == (size_t)out_w * out_h; if (fclose(f)) ok = 0;
    if (!ok || rename(tmp, path)) { LOG("FAIL write %s", path); return -1; } return 0;
}

int main(int argc, char **argv) {
    t_start = now();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && ++i)
        if (OPT("--fifo")) fifo = v; else if (OPT("--epdd-log")) epdd_log = v; else if (OPT("--out")) outdir = v; else if (OPT("--source")) source = v;
        else if (OPT("--interval")) interval_ms = atoi(v); else if (OPT("--quiet")) quiet_ms = atoi(v); else if (OPT("--settle")) settle_ms = atoi(v);
        else if (OPT("--min-gap")) min_gap_ms = atoi(v); else if (OPT("--clear-every")) clear_every = atoi(v); else if (OPT("--max-per-min")) max_per_min = atoi(v);
        else if (OPT("--active-mode")) active_mode = v; else if (OPT("--fit")) crop = !strcmp(v, "crop"); else if (OPT("--threshold")) threshold = atoi(v);
        else if (OPT("--frames")) frames_max = atoi(v); else if (!strcmp(a, "--dry")) dry = 1; else if (OPT("--ns-pid")) ns_pid = atoi(v);
        else if (OPT("--screencap")) screencap = v; else if (OPT("--display")) display_id = v;
        else { fprintf(stderr, "usage: see source header (%s)\n", a); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN); setvbuf(stdout, NULL, _IOLBF, 0);
    mkdir(outdir, 0755);
    if (epdd_log) { logf = fopen(epdd_log, "r"); if (!logf) LOG("WARN epdd log %s: %s (using time estimates)", epdd_log, strerror(errno)); else fseek(logf, 0, SEEK_END); }
    LOG("start: source=%s interval=%d quiet=%d settle=%d min-gap=%d clear-every=%d max-per-min=%d active=%s fit=%s%s",
        source, interval_ms, quiet_ms, settle_ms, min_gap_ms, clear_every, max_per_min, active_mode, crop ? "crop" : "letterbox", dry ? " DRY" : "");
    double last_change = 0, win_t = now(); int consec = 0, burst = 0, fast_on_panel = 0, quality_n = 0, win_n = 0, frame = 0, fails = 0;
    char path[600], cmd[700];
    while (!stop && (!frames_max || frame < frames_max)) {
        double t0 = now(); int rc;
        if (!strcmp(source, "screencap")) rc = capture_screencap();
        else if (!strcmp(source, "drm")) rc = capture_drm();
        else if (!strncmp(source, "file:", 5)) rc = capture_file(source + 5);
        else if (!strncmp(source, "files:", 6)) { static int last_ok; char p[512]; snprintf(p, sizeof p, source + 6, frame);
            if (!access(p, R_OK)) last_ok = frame; else snprintf(p, sizeof p, source + 6, last_ok); rc = capture_file(p); }
        else { LOG("FAIL unknown source %s", source); return 2; }
        frame++;
        poll_done();
        if (rc) { if (++fails >= 40) { LOG("FAIL 40 consecutive capture failures, exiting"); return 1; } usleep((useconds_t)interval_ms * 4000); continue; }
        fails = 0; double tcap = now() - t0;
        resample(); cur_land = out_w > out_h; tiles(cur_t);
        double moving = have_prev && prev_land == cur_land ? diff_frac(cur_t, prev_t) : 1.0;
        if (moving > 0) { last_change = now(); consec++; } else consec = 0;
        memcpy(prev_t, cur_t, NT); have_prev = 1; prev_land = cur_land;
        double dshown = have_shown && shown_land == cur_land ? diff_frac(cur_t, shown_t) : 1.0;
        double quiet = (now() - last_change) * 1000;
        if (frame <= 3 || frame % 40 == 0) LOG("frame %d: %dx%d capture %.0f ms, moving %.3f, vs panel %.3f", frame, gw, gh, tcap * 1000, moving, dshown);
        if (now() - win_t >= 60) { win_t = now(); win_n = 0; }
        int can = !outstanding && (now() - done_t) * 1000 >= min_gap_ms && win_n < max_per_min;
        if (can) {
            const char *what = NULL; int quality = 0;
            if (dshown > 0 && quiet >= (burst ? settle_ms : quiet_ms)) { what = "quality"; quality = 1; }
            else if (dshown > 0 && (burst || consec >= 2)) { if (!burst) LOG("burst start"); burst = 1; what = active_mode; }
            else if (dshown == 0 && fast_on_panel && quiet >= settle_ms) { what = "refresh"; quality = 1; }
            if (what) {
                if (quality && clear_every > 0 && ++quality_n % clear_every == 0) {
                    if (write_pgm(path, sizeof path)) break;
                    snprintf(cmd, sizeof cmd, "clear"); if (!send_cmd(cmd, 2, est_clear)) { win_n += 2;
                        while (!stop && outstanding) { usleep(20000); poll_done(); }
                        snprintf(cmd, sizeof cmd, "show %s quality", path); send_cmd(cmd, 1, est_quality); win_n++; }
                } else if (!strcmp(what, "refresh")) { snprintf(cmd, sizeof cmd, "refresh"); send_cmd(cmd, 1, est_quality); win_n++; }
                else { if (write_pgm(path, sizeof path)) break; snprintf(cmd, sizeof cmd, "show %s %s", path, what); send_cmd(cmd, 1, quality ? est_quality : est_fast); win_n++; }
                memcpy(shown_t, cur_t, NT); have_shown = 1; shown_land = cur_land;
                fast_on_panel = !quality; if (quality) { if (burst) LOG("burst end (settled %.0f ms)", quiet); burst = 0; }
            }
        }
        double tl = now();
        while (!stop && outstanding && now() - tl < interval_ms / 1000.0) { usleep(20000); poll_done(); }
        double el = (now() - t0) * 1000; if (el < interval_ms) usleep((useconds_t)((interval_ms - el) * 1000));
    }
    while (!stop && outstanding) { usleep(20000); poll_done(); }
    LOG("exit after %d frames", frame);
    return 0;
}
