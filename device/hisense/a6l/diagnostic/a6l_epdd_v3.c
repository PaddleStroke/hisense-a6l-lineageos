// SPDX-License-Identifier: Apache-2.0
/* a6l_epdd — Hisense A6L rear e-paper service (v3, 23 Sep 2026 evening; = v2 + DRM lease for the Android session).
 *
 * v3 changes (docs/eink-mirror-milestone-20260923.md), nothing else changed from v2 (sha256 b20e547c...):
 *  - --lease auto|PID: when another process (drm_hwcomposer) is DRM master of the card, obtain a DRM lease of the
 *    e-ink connector + CRTC + the primary planes that can feed that CRTC, from the master's own file: its fd is
 *    duplicated with pidfd_getfd() (root, CAP_SYS_PTRACE) and DRM_IOCTL_MODE_CREATE_LEASE is issued on it; the dup
 *    is closed at once. All DRM work then happens on the lessee fd (a master of its own lease). If we are master
 *    ourselves (nobody else is), the plain fd is used exactly as in v2.
 *  - --save-mode F: write the e-ink connector id + mode to F and exit (run BEFORE the connector is forced "off" in
 *    sysfs so that drm_hwcomposer does not attach it as a second display); --mode-file F: use that connector/mode even
 *    when the connector reports disconnected (status forced off => the kernel reports no modes).
 *
 * v2 changes (docs/eink-clear-prep-20260923.md):
 * v2 changes (docs/eink-clear-prep-20260923.md):
 *  - "clear" = the library's real INIT flash (the long 98-frame clear) on demand: libtcon_eink.so only runs its INIT
 *    waveform while handle+0x270 == 1 (set once by Init_Eink_SWTcon; ModeDecision_MirrorMode clears it only on call 2).
 *    v2 sets it before a white update and clears it afterwards. Variants: "clear" = "clear full" (forced white GC16
 *    from the current picture, 39 frames, then INIT, 99 frames: ~1.6 s), "clear stock" (INIT then white GC16, the
 *    order of calls 1+2 of a fresh handle), "clear init" (INIT only: leaves the library state stale, test use only),
 *    "clear gc" (V73 behaviour: force=1 white, 39 frames).
 *  - "refresh": what stock's /sys/class/graphics/fb1/epd_force_clear did (HWC ClearGhosting -> force=1 on the next
 *    ModeDecision): a forced GC16 redraw of the CURRENT picture (38 frames), no white flash to blank.
 *  - mode names from the disassembly: auto(0, library picks from content), fast(1), quality/picture(2),
 *    reading/partial(3; 3-5 identical), a2/fastest(8; any value >5 is the same, stock HWC default = 8).
 *
 * Runs the stock software TCON (vendor libtcon_eink.so, pure computation) ON THE PHONE with the panel's own waveform
 * (first 0x70080 bytes of the panel NOR) and scans the drive frames out on the e-ink DRM connector (384x725 @ 85 Hz),
 * one frame per vblank, exactly like a6l_epd_play did with the offline frames that drew in r131/r135/r137/r138.
 *
 * Proven facts this relies on (docs: claude/fresh-eye-eink-bridge-20260922.md):
 *  - library image = 1440 x 720 RGBA; library X runs right-to-left on the panel (landscape, camera on the right);
 *  - call 1 (force=1) = fixed full clear, call 2 always resets to white, the picture goes through from call 3;
 *  - panel rails only during an update, switched by the panel driver (sysfs epd_power, stock order); XON (gpio61) high.
 *
 * usage: a6l_epdd [options] [--show img.pgm]... [--fifo PATH]
 *   --waveform F   panel NOR image (default /tmp/epd/epd-nor.bin)
 *   --lib F        libtcon_eink.so (default: dlopen("libtcon_eink.so"))
 *   --mode M       refresh mode: quality|picture (2, default) | partial|reading (3) | fast (1) | fastest|a2 (8) | auto (0) | number
 *   --clear-every N  full clear (flash) before every N-th picture (0 = never, default)
 *   --temp N       fallback temperature in C when the TPS65185 hwmon is missing (default 25)
 *   --rot180       rotate every input 180 degrees; --no-dither: plain rounding (default: Floyd-Steinberg to 16 greys)
 *   --show F       show a PGM/PPM (1440x720 landscape as seen with the camera on the right, or 720x1440 portrait with
 *                  the camera on top); may be repeated; --hold S seconds between/after them (default 5)
 *   --script F     then run commands from a file; --fifo P: then serve commands from a FIFO forever
 *                  commands: "show <file> [quality|partial|fast|fastest|auto|N]", "clear [stock|init|full|gc]",
 *                  "refresh" (forced GC16 redraw of the current picture), "sleep <s>", "quit"
 *   --dry PREFIX   no DRM/power: write each update's frames to PREFIX-<n>.a6lepd, or as RLE text on stdout for "-"
 *   --power P      sysfs rail switch (default: first /sys/bus/mipi-dsi/devices/<any>/epd_power)
 *   --xon-line N   gpiochip0 line held high while running (default 61, -1 = none)
 *   --lead N / --tail N   idle frames after rails on / before rails off (default 10 / 20)
 *   --lease auto|PID   (v3) get a DRM lease from the DRM master process (drm_hwcomposer) instead of being master
 *   --save-mode F      (v3) write e-ink connector id + mode to F and exit; --mode-file F: use it (connector forced off)
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>

#define W 384
#define H 725
#define FRAME 0x10fe00u		/* 384 * 725 * 4 */
#define RING 5
#define FLASH 0x70080u
#define IW 1440
#define IH 720
#define RGBA (IW * IH * 4)
#define MAXF 400

struct buf { void *data; uint32_t size; uint32_t pad; };
/* the library may try these on MediaTek-style paths; never run anything */
int system(const char *c) { fprintf(stderr, "A6L_EPDD refused system(%s)\n", c ? c : ""); return -1; }
FILE *popen(const char *c, const char *m) { (void)m; fprintf(stderr, "A6L_EPDD refused popen(%s)\n", c ? c : ""); return NULL; }

static void *(*tc_init)(struct buf *, int, uint32_t *, void *, uint32_t, void *);
static int (*tc_decide)(struct buf *, void *, int, int, int, int);
static uint8_t (*tc_update)(struct buf *, void *);
static void *handle;
static uint8_t *last_img;	/* v2: last picture in library layout, for "refresh" */
static int have_last;
#define H_INIT_FLAG(h) ((volatile uint32_t *)((uint8_t *)(h) + 0x270))	/* 1 = next update uses the INIT waveform */
#define H_CALLS(h) ((volatile uint32_t *)((uint8_t *)(h) + 0x274))	/* ModeDecision_MirrorMode call counter */
static struct buf ring[RING], img;
static uint32_t *frames[MAXF];
static int mode = 2, temp_fallback = 25, rot180, dither = 1, lead = 10, tail = 20, xon_line = 61, dry, updates;
static const char *dry_prefix, *power_path;
static char power_buf[512];

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#define LOG(...) do { printf("[%.3f] A6L_EPDD ", now()); printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

/* ---------------- images ---------------- */
static void img_fill(uint8_t v) { uint8_t *p = img.data; for (unsigned i = 0; i < IW * IH; i++) { p[4 * i] = p[4 * i + 1] = p[4 * i + 2] = v; p[4 * i + 3] = 0xff; } }
static void img_set(unsigned X, unsigned Y, uint8_t v) { uint8_t *q = (uint8_t *)img.data + 4 * (Y * IW + X); q[0] = q[1] = q[2] = v; q[3] = 0xff; }
static int tok(FILE *f) {	/* PNM header integer, skipping comments */
    int c, v = 0, got = 0;
    while ((c = fgetc(f)) != EOF) { if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; } if (c >= '0' && c <= '9') { v = v * 10 + c - '0'; got = 1; } else if (got) break; }
    return got ? v : -1;
}
static int load_pnm(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { LOG("FAIL open %s: %s", path, strerror(errno)); return -1; }
    char m[3] = {0}; int ok = fread(m, 1, 2, f) == 2, w = tok(f), h = tok(f), mx = tok(f);
    int ch = m[1] == '5' ? 1 : m[1] == '6' ? 3 : 0;
    if (!ok || m[0] != 'P' || !ch || mx != 255 || !((w == IW && h == IH) || (w == IH && h == IW))) { LOG("FAIL %s: need 8-bit P5/P6 1440x720 or 720x1440 (got %c%c %dx%d max %d)", path, m[0], m[1], w, h, mx); fclose(f); return -1; }
    uint8_t *row = malloc((size_t)w * ch); int16_t *g = malloc((size_t)w * h * sizeof *g); if (!row || !g) { free(row); free(g); fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        if (fread(row, ch, w, f) != (size_t)w) { LOG("FAIL %s: short file", path); free(row); free(g); fclose(f); return -1; }
        for (int x = 0; x < w; x++) g[y * w + x] = ch == 1 ? row[x] : (int16_t)((row[3 * x] * 77 + row[3 * x + 1] * 150 + row[3 * x + 2] * 29) >> 8);
    }
    if (dither) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {	/* Floyd-Steinberg to the panel's 16 levels (r145: 16 visible bands) */
        int old = g[y * w + x], q = (old < 0 ? 0 : old > 255 ? 255 : old); q = (q + 8) / 17 * 17; int e = old - q; g[y * w + x] = (int16_t)q;
        if (x + 1 < w) g[y * w + x + 1] += (int16_t)(e * 7 / 16);
        if (y + 1 < h) { if (x > 0) g[(y + 1) * w + x - 1] += (int16_t)(e * 3 / 16); g[(y + 1) * w + x] += (int16_t)(e * 5 / 16); if (x + 1 < w) g[(y + 1) * w + x + 1] += (int16_t)(e / 16); }
    }
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int v0 = g[y * w + x]; uint8_t v = (uint8_t)(v0 < 0 ? 0 : v0 > 255 ? 255 : v0);
        int xx = rot180 ? w - 1 - x : x, yy = rot180 ? h - 1 - y : y;
        if (w == IW) img_set(IW - 1 - xx, yy, v);	/* landscape as seen: library X is mirrored (r135) */
        else img_set(yy, xx, v);			/* portrait, camera on top: library(X,Y) = image(row X, col Y) */
    }
    free(g);
    free(row); fclose(f); return 0;
}

/* ---------------- hardware helpers ---------------- */
static int temperature(void) {
    DIR *d = opendir("/sys/class/hwmon"); struct dirent *e; char p[300], name[64];
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/name", e->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
        int is = fgets(name, sizeof name, f) && !strncmp(name, "tps65185", 8); fclose(f); if (!is) continue;
        snprintf(p, sizeof p, "/sys/class/hwmon/%s/temp1_input", e->d_name); f = fopen(p, "r"); long mc;
        if (f && fscanf(f, "%ld", &mc) == 1) { fclose(f); closedir(d); int t = (int)(mc / 1000); return t < 0 ? 0 : t > 50 ? 50 : t; }
        if (f) fclose(f);
    }
    if (d) closedir(d);
    return temp_fallback;
}
static int power(int on) {
    if (dry) return 0;
    if (!power_path) { LOG("WARN no epd_power attribute: rails not switched"); return -1; }
    int f = open(power_path, O_WRONLY); if (f < 0) { LOG("FAIL open %s: %s", power_path, strerror(errno)); return -1; }
    int r = write(f, on ? "1" : "0", 1) == 1 ? 0 : -1; if (r) LOG("FAIL rails %s: %s", on ? "on" : "off", strerror(errno)); close(f); return r;
}
static void find_power(void) {
    DIR *d = opendir("/sys/bus/mipi-dsi/devices"); struct dirent *e;
    while (d && (e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(power_buf, sizeof power_buf, "/sys/bus/mipi-dsi/devices/%s/epd_power", e->d_name); if (!access(power_buf, W_OK)) { power_path = power_buf; break; } }
    if (d) closedir(d);
}
static void bringup_status(char *out, size_t n) {
    out[0] = 0; if (!power_path) return; char p[520]; snprintf(p, sizeof p, "%.*s/bringup_status", (int)(strrchr(power_path, '/') - power_path), power_path);
    FILE *f = fopen(p, "r"); if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; fclose(f); char *nl = strchr(out, '\n'); if (nl) *nl = 0; }
}
static int xon_fd = -1;
static void xon_hold(void) {
    if (xon_line < 0 || dry) return;
    int chip = open("/dev/gpiochip0", O_RDONLY); if (chip < 0) { LOG("WARN gpiochip0: %s", strerror(errno)); return; }
    struct gpio_v2_line_request rq; memset(&rq, 0, sizeof rq); rq.offsets[0] = (unsigned)xon_line; rq.num_lines = 1;
    rq.config.flags = GPIO_V2_LINE_FLAG_OUTPUT; rq.config.num_attrs = 1; rq.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    rq.config.attrs[0].attr.values = 1; rq.config.attrs[0].mask = 1; strcpy(rq.consumer, "a6l-epdd-xon");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &rq)) LOG("WARN XON gpio%d request: %s", xon_line, strerror(errno)); else { xon_fd = rq.fd; LOG("XON gpio%d held high", xon_line); }
    close(chip);
}

/* ---------------- DRM ---------------- */
struct fb { uint32_t handle, id; uint32_t *map; };
static int dfd = -1, started; static uint32_t crtc, conn_id; static drmModeModeInfo mode_info; static struct fb idle, fa, fb2;
static unsigned last_seq, gaps, flips; static int pending;
static int mkfb(struct fb *f) {
    struct drm_mode_create_dumb c = {.width = W, .height = H, .bpp = 32};
    if (drmIoctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &c) || c.pitch != W * 4) return -1;
    uint32_t hs[4] = {c.handle}, ps[4] = {c.pitch}, os[4] = {0};
    if (drmModeAddFB2(dfd, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, &f->id, 0)) return -1;
    struct drm_mode_map_dumb m = {.handle = c.handle}; if (drmIoctl(dfd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return -1;
    f->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, m.offset); f->handle = c.handle; return f->map == MAP_FAILED ? -1 : 0;
}
static void on_flip(int f, unsigned seq, unsigned s, unsigned us, void *d) { (void)f; (void)s; (void)us; (void)d; if (flips && seq > last_seq + 1) gaps += seq - last_seq - 1; last_seq = seq; flips++; pending = 0; }
static int flip(uint32_t id) {
    drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
    if (drmModePageFlip(dfd, crtc, id, DRM_MODE_PAGE_FLIP_EVENT, 0)) { LOG("FAIL pageflip: %s", strerror(errno)); return -1; }
    pending = 1;
    while (pending) { struct pollfd p = {.fd = dfd, .events = POLLIN}; if (poll(&p, 1, 1000) <= 0) { LOG("FAIL vblank timeout"); return -1; } drmHandleEvent(dfd, &ev); }
    return 0;
}
/* v3: saved connector + mode (binary: magic, connector id, drmModeModeInfo) */
struct mode_file { uint32_t magic, conn; drmModeModeInfo m; };
#define MODE_MAGIC 0x41364c4du
static const char *mode_file, *save_mode, *lease_arg;
static int drm_lease(drmModeRes *res, int crtc_index);
static int drm_open(void) {
    drmModeRes *res = NULL; drmModeConnector *con = NULL; struct mode_file mf = {0};
    if (mode_file) { FILE *f = fopen(mode_file, "rb"); int ok = f && fread(&mf, sizeof mf, 1, f) == 1 && mf.magic == MODE_MAGIC && mf.m.hdisplay == W && mf.m.vdisplay == H; if (f) fclose(f);
        if (!ok) { LOG("FAIL mode file %s unreadable or not 384x725", mode_file); return -1; } LOG("mode file: connector %u %s@%u", mf.conn, mf.m.name, mf.m.vrefresh); }
    for (int c = 0; c < 4 && !con; c++) {
        char path[32]; snprintf(path, sizeof path, "/dev/dri/card%d", c); dfd = open(path, O_RDWR | O_CLOEXEC); if (dfd < 0) continue;
        res = drmModeGetResources(dfd);
        for (int i = 0; res && i < res->count_connectors; i++) { drmModeConnector *k = drmModeGetConnector(dfd, res->connectors[i]);
            if (k && mode_file && k->connector_id == mf.conn && k->connector_type == DRM_MODE_CONNECTOR_DSI) { con = k; break; }	/* v3: may be forced "off" (no modes) */
            if (k && !mode_file && k->connection == DRM_MODE_CONNECTED && k->count_modes && k->modes[0].hdisplay == W && k->modes[0].vdisplay == H) { con = k; break; } drmModeFreeConnector(k); }
        if (!con) { if (res) drmModeFreeResources(res); res = NULL; close(dfd); dfd = -1; }
    }
    if (!con) { LOG("FAIL no %s384x725 connector", mode_file ? "(mode-file) " : "connected "); return -1; }
    if (mode_file) mode_info = mf.m; else mode_info = con->modes[0];
    conn_id = con->connector_id;
    if (save_mode) { mf.magic = MODE_MAGIC; mf.conn = conn_id; mf.m = mode_info; FILE *f = fopen(save_mode, "wb"); int ok = f && fwrite(&mf, sizeof mf, 1, f) == 1; if (f && fclose(f)) ok = 0;
        LOG("%s mode file %s: connector %u %s@%u", ok ? "wrote" : "FAIL write", save_mode, conn_id, mode_info.name, mode_info.vrefresh); drmDropMaster(dfd); close(dfd); exit(ok ? 0 : 1); }
    int crtc_index = -1;
    for (int e = 0; e < con->count_encoders && !crtc; e++) { drmModeEncoder *enc = drmModeGetEncoder(dfd, con->encoders[e]); if (!enc) continue;
        for (int i = 0; i < res->count_crtcs && !crtc; i++) if (enc->possible_crtcs & (1u << i)) { int used = 0;
            for (int j = 0; j < res->count_encoders; j++) { drmModeEncoder *o = drmModeGetEncoder(dfd, res->encoders[j]); if (o && o->encoder_id != enc->encoder_id && o->crtc_id == res->crtcs[i]) used = 1; drmModeFreeEncoder(o); }
            if (!used) { crtc = res->crtcs[i]; crtc_index = i; } }
        drmModeFreeEncoder(enc); }
    if (!crtc) { LOG("FAIL no free crtc"); return -1; }
    if (lease_arg && !drmIsMaster(dfd)) { if (drm_lease(res, crtc_index)) return -1; }	/* v3: dfd is now the lessee */
    else if (drmSetMaster(dfd)) LOG("WARN setmaster: %s", strerror(errno));
    else if (lease_arg) LOG("lease not needed: this process is DRM master (no composer running?)");
    if (mkfb(&idle) || mkfb(&fa) || mkfb(&fb2)) { LOG("FAIL dumb fb"); return -1; }
    LOG("DRM connector=%u crtc=%u mode=%s@%u", conn_id, crtc, mode_info.name, mode_info.vrefresh);
    return 0;
}
/* v3: lease from the current DRM master (drm_hwcomposer). pidfd_getfd needs Linux >= 5.6 and ptrace rights (root). */
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif
static int try_lease_fd(int mfd, uint32_t *objs, int n) {
    uint32_t lessee = 0; int l = drmModeCreateLease(mfd, objs, n, O_CLOEXEC, &lessee);
    if (l < 0) return -1;
    LOG("lease %u created with %d objects", lessee, n); return l;
}
static int drm_lease(drmModeRes *res, int crtc_index) {
    uint32_t objs[16]; int n = 0; struct stat me;
    objs[n++] = conn_id; objs[n++] = crtc;
    drmSetClientCap(dfd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    drmModePlaneRes *pr = drmModeGetPlaneResources(dfd);
    for (unsigned i = 0; pr && i < pr->count_planes && n < 16; i++) {
        drmModePlane *pl = drmModeGetPlane(dfd, pr->planes[i]); if (!pl) continue;
        int primary = 0; drmModeObjectProperties *pp = drmModeObjectGetProperties(dfd, pl->plane_id, DRM_MODE_OBJECT_PLANE);
        for (unsigned j = 0; pp && j < pp->count_props; j++) { drmModePropertyRes *p = drmModeGetProperty(dfd, pp->props[j]);
            if (p && !strcmp(p->name, "type") && pp->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) primary = 1; drmModeFreeProperty(p); }
        drmModeFreeObjectProperties(pp);
        if (primary && (pl->possible_crtcs & (1u << crtc_index))) objs[n++] = pl->plane_id;	/* crtc->primary is one of these */
        drmModeFreePlane(pl);
    }
    if (pr) drmModeFreePlaneResources(pr);
    (void)res;
    if (fstat(dfd, &me)) return -1;
    int want = lease_arg && strcmp(lease_arg, "auto") ? atoi(lease_arg) : 0, lfd = -1;
    DIR *pd = opendir("/proc"); struct dirent *pe;
    while (lfd < 0 && pd && (pe = readdir(pd))) {
        int pid = atoi(pe->d_name); if (pid <= 1 || pid == getpid() || (want && pid != want)) continue;
        char fdp[64]; snprintf(fdp, sizeof fdp, "/proc/%d/fd", pid); DIR *fdd = opendir(fdp); if (!fdd) continue;
        int pidfd = -1; struct dirent *fe;
        while (lfd < 0 && (fe = readdir(fdd))) {
            char lp[128], tgt[128]; ssize_t k; snprintf(lp, sizeof lp, "%s/%s", fdp, fe->d_name);
            if ((k = readlink(lp, tgt, sizeof tgt - 1)) <= 0) continue; tgt[k] = 0; if (strncmp(tgt, "/dev/dri/card", 13)) continue;
            if (pidfd < 0 && (pidfd = (int)syscall(__NR_pidfd_open, pid, 0)) < 0) { LOG("WARN pidfd_open %d: %s", pid, strerror(errno)); break; }
            int mfd = (int)syscall(__NR_pidfd_getfd, pidfd, atoi(fe->d_name), 0); struct stat st;
            if (mfd < 0) { LOG("WARN pidfd_getfd %d/%s: %s", pid, fe->d_name, strerror(errno)); continue; }
            if (!fstat(mfd, &st) && st.st_rdev == me.st_rdev) { lfd = try_lease_fd(mfd, objs, n);
                LOG("pid %d fd %s (%s): %s", pid, fe->d_name, tgt, lfd >= 0 ? "master, lease granted" : strerror(errno)); }
            close(mfd);
        }
        if (pidfd >= 0) close(pidfd); closedir(fdd);
    }
    if (pd) closedir(pd);
    if (lfd < 0) { LOG("FAIL no DRM master process granted a lease (is the composer running? root/ptrace allowed?)"); return -1; }
    close(dfd); dfd = lfd; return 0;
}
static int drm_start(void) {	/* modeset = panel prepare+enable = kernel bring-up (V73 panel driver) */
    char st[200];
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (drmModeSetCrtc(dfd, crtc, idle.id, 0, 0, &conn_id, 1, &mode_info)) { LOG("FAIL setcrtc: %s", strerror(errno)); return -1; }
        for (int i = 0; i < 10; i++) flip(idle.id);
        bringup_status(st, sizeof st); LOG("bring-up %d: %s", attempt, st[0] ? st : "(no status attribute)");
        if (!st[0] || strstr(st, "bridge_ok=1")) return 0;
        drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); sleep(1);
    }
    return -1;
}

/* ---------------- updates ---------------- */
static void write_a6lepd(int n) {	/* --dry -: RLE lines on stdout (A6L_EINK_RLE <update> <frame> count:value ..., as the probe) */
    char p[512]; FILE *f = NULL; int text = !strcmp(dry_prefix, "-");
    if (!text) { snprintf(p, sizeof p, "%s-%d.a6lepd", dry_prefix, updates); f = fopen(p, "wb"); if (!f) { LOG("FAIL %s", p); return; }
        uint32_t h[3] = {W, H, (uint32_t)n}; fwrite("A6LEPD1\n", 1, 8, f); fwrite(h, 4, 3, f); }
    for (int k = 0; k < n; k++) { const uint32_t *q = frames[k]; uint32_t runs = 0, i = 0, tot = FRAME / 4;
        if (text) { printf("A6L_EINK_RLE %d %d", updates, k); while (i < tot) { uint32_t j = i + 1; while (j < tot && q[j] == q[i]) j++; printf(" %x:%x", j - i, q[i]); i = j; } printf("\n"); continue; }
        while (i < tot) { uint32_t j = i + 1; while (j < tot && q[j] == q[i]) j++; runs++; i = j; }
        fwrite(&runs, 4, 1, f); i = 0; while (i < tot) { uint32_t j = i + 1; while (j < tot && q[j] == q[i]) j++; uint32_t cv[2] = {j - i, q[i]}; fwrite(cv, 4, 2, f); i = j; } }
    if (f) { fclose(f); LOG("wrote %s (%d frames)", p, n); } else fflush(stdout);
}
static int run_update(int force, int m, const char *what) {
    int t = temperature(); double t0 = now();
    int nf = tc_decide(&img, handle, t, t, force, m), n = 0; uint8_t more = 1;
    while (more && n < MAXF) { struct buf *b = &ring[n % RING]; more = tc_update(b, handle);
        if (!frames[n] && !(frames[n] = malloc(FRAME))) { LOG("FAIL out of memory"); return -1; }
        memcpy(frames[n], b->data, FRAME); n++; }
    updates++; LOG("update %d (%s): mode=%d force=%d temp=%dC decision=%d frames=%d generated in %.0f ms", updates, what, m, force, t, nf, n, (now() - t0) * 1000);
    if (dry) { write_a6lepd(n); return 0; }
    static int have_idle; if (!have_idle) { for (unsigned i = 0; i < FRAME / 4; i++) idle.map[i] = frames[0][i] & 0xffffff00u; have_idle = 1; }
    if (!started) { if (drm_start()) return -1; started = 1; }	/* modeset with the real strobe pattern, as a6l_epd_play did */
    unsigned g0 = gaps; int rc = 0; double t1 = now();
    if (power(1)) LOG("WARN rails not switched on");
    for (int i = 0; i < lead && !rc; i++) rc = flip(idle.id);
    unsigned gd = gaps;
    for (int k = 0; k < n && !rc; k++) { struct fb *f = (k & 1) ? &fb2 : &fa; memcpy(f->map, frames[k], FRAME); rc = flip(f->id); }
    gd = gaps - gd;
    for (int i = 0; i < tail && !rc; i++) rc = flip(idle.id);
    power(0);
    LOG("update %d shown in %.0f ms: %s, missed vblanks during drive=%u (total %u)", updates, (now() - t1) * 1000, rc ? "FAILED" : "ok", gd, gaps - g0);
    return rc;
}
/* v2 clears. Library facts (disassembly of the stock libtcon_eink.so, 23 Sep):
 *  ModeDecision_MirrorMode: if calls(+0x274) != 0 && +0x270 != 0 -> Reset_Panel(input := white), force := 0, and
 *  +0x270 := 0 only when calls == 1; ModeDecision_MirrorMode_Lib: +0x270 == 1 -> internal mode 0 = INIT waveform;
 *  Update_Display_Image_Lib: +0x270 == 1 -> INIT frames (whole panel, image ignored). Only Init_Eink_SWTcon ever sets
 *  +0x270, so without this poke the long flash only happens on the first update after init. */
static int clear_init(const char *what) {
    uint32_t calls = *H_CALLS(handle), flag = *H_INIT_FLAG(handle);
    if (flag > 1 || calls != (uint32_t)updates) { LOG("WARN handle layout unexpected (flag=%u calls=%u updates=%d): INIT clear refused, using force clear", flag, calls, updates); img_fill(0xff); return run_update(1, 2, what); }
    img_fill(0xff); *H_INIT_FLAG(handle) = 1;
    int rc = run_update(0, 2, what);
    *H_INIT_FLAG(handle) = 0;	/* the library would keep flashing otherwise (it only clears the flag on call 2) */
    return rc;
}
static int clear_white_gc(const char *what) { img_fill(0xff); return run_update(1, 2, what); }
static int clear_kind(const char *k) {
    /* default = "full": white GC16 first (from the real current picture), then INIT from white, so each waveform starts
     * from the state the library assumes. QEMU rc01clear: "full" and "stock" both leave the library in the same state
     * as start-up (the next picture is byte-identical to the r137 update3); "init" alone does NOT (stale state). */
    if (!k || !*k || !strcmp(k, "full")) { int rc = clear_white_gc("clear-white"); return rc ? rc : clear_init("clear-init"); }
    if (!strcmp(k, "stock")) { int rc = clear_init("clear-init"); return rc ? rc : clear_white_gc("clear-white"); }
    if (!strcmp(k, "init")) return clear_init("clear-init");
    if (!strcmp(k, "gc")) return clear_white_gc("clear-gc");
    LOG("unknown clear kind '%s'", k); return -1;
}
static int clear_full(void) { return clear_kind("full"); }
static int refresh(void) {	/* stock epd_force_clear: forced GC16 redraw of the current picture */
    if (!have_last) return clear_white_gc("refresh-white");
    memcpy(img.data, last_img, RGBA); return run_update(1, 2, "refresh");
}
/* Mode map. Measured in QEMU with the real waveform (r147): 2 = full-screen 16-grey refresh (39 frames),
 * 0/3/4/5 = 16-grey partial (changed pixels only, 39 frames), 1 = fast partial with fewer greys (23 frames),
 * 6 = fastest partial, black/white (10 frames). Switching mode costs one full transition update (39-79 frames).
 * Disassembly (v2): the mode argument is dispatched as 0 -> content analysis (library picks GC16 / REGAL / DU / A2
 * from histogram + changed-area ratios), 1 -> waveform 1, 2 -> waveform 2 (GC16), 3/4/5 -> waveform 5 (REGAL, the
 * stock "reading" = 3), anything > 5 -> waveform 6 (A2; stock HWC's default epd_display_mode is 8).
 * force=1 only means "update even if the image is unchanged + full GC16 (38 frames)"; the long flash is clear_init(). */
static int mode_by_name(const char *n, int def) {
    if (!n || !*n) return def;
    if (!strcmp(n, "quality") || !strcmp(n, "picture")) return 2; if (!strcmp(n, "partial") || !strcmp(n, "reading")) return 3;
    if (!strcmp(n, "fast")) return 1; if (!strcmp(n, "fastest") || !strcmp(n, "a2")) return 8; if (!strcmp(n, "auto")) return 0;
    return atoi(n);
}
static int clear_every, since_clear;
static int show(const char *path, int m) {
    if (clear_every > 0 && ++since_clear >= clear_every) { since_clear = 0; clear_full(); }
    if (load_pnm(path)) return -1; memcpy(last_img, img.data, RGBA); have_last = 1; return run_update(0, m, path);
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static void commands(FILE *q) {	/* "show <file> [mode]", "clear [stock|init|full|gc]", "refresh", "sleep <s>", "quit" */
    char line[600];
    while (!stop && fgets(line, sizeof line, q)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0; char path[512], mname[32] = ""; int m;
        if (!line[0] || line[0] == '#') continue;
        if (!strcmp(line, "quit")) { stop = 1; break; }
        else if (!strcmp(line, "clear")) clear_full();
        else if (!strncmp(line, "clear ", 6)) clear_kind(line + 6);
        else if (!strcmp(line, "refresh")) refresh();
        else if (!strncmp(line, "sleep ", 6)) sleep((unsigned)atoi(line + 6));
        else if (sscanf(line, "show %511s %31s", path, mname) >= 1) { m = mode_by_name(mname, mode); show(path, m); }
        else LOG("unknown command '%s'", line);
    }
}

int main(int argc, char **argv) {
    const char *wf = "/tmp/epd/epd-nor.bin", *lib = "libtcon_eink.so", *fifo = NULL, *script = NULL; const char *shows[32]; int nshow = 0, hold = 5;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--waveform") && v) wf = argv[++i]; else if (!strcmp(a, "--lib") && v) lib = argv[++i];
        else if (!strcmp(a, "--mode") && v) mode = mode_by_name(argv[++i], 2); else if (!strcmp(a, "--clear-every") && v) clear_every = atoi(argv[++i]); else if (!strcmp(a, "--temp") && v) temp_fallback = atoi(argv[++i]);
        else if (!strcmp(a, "--rot180")) rot180 = 1; else if (!strcmp(a, "--no-dither")) dither = 0; else if (!strcmp(a, "--show") && v && nshow < 32) shows[nshow++] = argv[++i];
        else if (!strcmp(a, "--hold") && v) hold = atoi(argv[++i]); else if (!strcmp(a, "--fifo") && v) fifo = argv[++i]; else if (!strcmp(a, "--script") && v) script = argv[++i];
        else if (!strcmp(a, "--dry") && v) { dry = 1; dry_prefix = argv[++i]; } else if (!strcmp(a, "--power") && v) power_path = argv[++i];
        else if (!strcmp(a, "--xon-line") && v) xon_line = atoi(argv[++i]); else if (!strcmp(a, "--lead") && v) lead = atoi(argv[++i]);
        else if (!strcmp(a, "--tail") && v) tail = atoi(argv[++i]);
        else if (!strcmp(a, "--lease") && v) lease_arg = argv[++i]; else if (!strcmp(a, "--mode-file") && v) mode_file = argv[++i];
        else if (!strcmp(a, "--save-mode") && v) save_mode = argv[++i];
        else { fprintf(stderr, "usage: see source header (%s)\n", a); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); setvbuf(stdout, NULL, _IOLBF, 0);
    if (save_mode) return drm_open() ? 1 : 0;	/* v3: only record connector + mode (drm_open exits) */
    /* waveform + library */
    uint8_t *flash = calloc(1, FLASH); FILE *f = fopen(wf, "rb");
    if (!flash || !f || fread(flash, 1, FLASH, f) != FLASH) { LOG("FAIL waveform %s", wf); return 1; } fclose(f);
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL); if (!h) { LOG("FAIL dlopen %s: %s", lib, dlerror()); return 1; }
    tc_init = dlsym(h, "Init_Eink_SWTcon"); tc_decide = dlsym(h, "ModeDecision_MirrorMode"); tc_update = dlsym(h, "Update_Display_Image");
    if (!tc_init || !tc_decide || !tc_update) { LOG("FAIL symbols"); return 1; }
    for (int i = 0; i < RING; i++) { ring[i].data = calloc(1, FRAME); ring[i].size = FRAME; if (!ring[i].data) return 1; }
    img.data = calloc(1, RGBA); img.size = RGBA; last_img = malloc(RGBA); if (!img.data || !last_img) return 1;
    uint32_t cfg[3] = {IW, IH, 50}; uint8_t info[0x400] = {0};
    handle = tc_init(ring, RING, cfg, flash, FLASH, info);
    if (!handle) { LOG("FAIL Init_Eink_SWTcon rejected the waveform"); return 1; }
    LOG("TCON ready: panel %.15s waveform %.31s", (char *)info, (char *)info + 47);
    if (!dry) { if (!power_path) find_power(); LOG("rail switch: %s", power_path ? power_path : "NONE"); xon_hold(); if (drm_open()) return 1; }
    /* start-up: call 1 = full clear (INIT: +0x270 set by Init), call 2 = the library's reset to white (both shown) */
    img_fill(0xff); if (run_update(1, 0, "clear")) goto out;
    img_fill(0xff); if (run_update(0, mode, "reset-to-white")) goto out;
    LOG("after start-up: init flag=%u calls=%u", *H_INIT_FLAG(handle), *H_CALLS(handle));
    for (int i = 0; i < nshow && !stop; i++) { if (show(shows[i], mode)) LOG("show %s failed", shows[i]); for (int s = 0; s < hold && !stop; s++) sleep(1); }
    if (script) { FILE *q = fopen(script, "r"); if (!q) LOG("FAIL script %s", script); else { commands(q); fclose(q); } }
    if (fifo) {
        mkfifo(fifo, 0660); LOG("serving %s", fifo);
        while (!stop) { FILE *q = fopen(fifo, "r"); if (!q) { LOG("FAIL fifo %s", fifo); break; } commands(q); fclose(q); }
    }
out:
    if (!dry) { power(0); if (crtc) drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); }
    if (xon_fd >= 0) { struct gpio_v2_line_values v = {0, 1}; ioctl(xon_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v); close(xon_fd); }
    LOG("exit");
    return 0;
}
