// SPDX-License-Identifier: Apache-2.0
/* a6l_epdd — Hisense A6L rear e-paper service, productized for the installed LineageOS ROM (v4, agent eink3, 24 Sep 2026).
 * Base: device/hisense/a6l/diagnostic/a6l_epdd_v3.c (v2 + DRM lease). The update path (image conversion, library calls,
 * frame drive, clears) is unchanged from v2/v3, so the QEMU byte-identity proofs of v2/v3 carry over (re-checked in
 * docs/eink3-20260924.md with the same script).
 *
 * v4 (docs/eink3-20260924.md):
 *  - DRM ownership in the ROM: the patched drm_hwcomposer never attaches the e-ink connector
 *    (vendor.hwc.drm.ignore_connectors=mode:384x725) and hands out a DRM lease of it on the abstract socket
 *    @<vendor.hwc.drm.lease_socket> ("--lease hwc[:NAME]"). "--lease auto" (v3: pidfd_getfd from the master process) stays
 *    for the RAM session with the unpatched composer. With no composer at all we are master ourselves (v2 behaviour).
 *    The lease is re-requested when it is lost (composer restart): the update is driven again on the new lessee fd.
 *  - waveform sources tried in order (--waveform FILE, --nor-spidev auto|DEV): a copy in /mnt/vendor/persist or the cache
 *    in /data/vendor, else the panel's own SPI NOR read READ-ONLY through spidev (opcodes 0x9F/0x03 only, two identical
 *    passes, JEDEC must be the Macronix MX25U4033E), cached with --waveform-cache, else the vendor-image copy.
 *    NEVER any write/erase opcode: xfer() aborts on anything but 0x9F/0x05/0x03.
 *  - control socket (init "socket a6l_epd stream 0660 system system" -> --socket a6l_epd, or --listen PATH): one line per
 *    command, one reply line per command ("OK ..." / "ERR ..."). New commands: "frame W H MODE" followed by W*H raw 8-bit
 *    grey bytes (no file sharing between processes), "mode M", "status", "power off", "ping".
 *  - idle power: the e-ink CRTC (DSI1 stream, 85 Hz) is switched off after --idle-off S seconds without an update; the
 *    next update does the modeset + bridge bring-up again (e-paper keeps the picture without power).
 *  - wakelock "a6l_epdd" held while an update is driven (system suspend in the middle would leave half a waveform).
 *  - XON: DT-owned since V74 (default --xon-line -1 here; v2/v3 defaulted to 61).
 *
 * usage: a6l_epdd [options]
 *   --waveform F (repeatable, tried in order) --nor-spidev auto|/dev/spidevB.C --waveform-cache F  --lib libtcon_eink.so
 *   --lease hwc[:NAME]|auto|PID|none (repeatable, tried in order; default: master if possible, else hwc, else auto)
 *   --wait-drm S   keep retrying DRM/lease for S seconds at start (default 0)
 *   --socket NAME | --listen PATH | --fifo PATH | --script F | --show F   (command sources)
 *   --idle-off S (0 = never; default 0)  --no-startup-clear  --wakelock (default on with --socket/--listen)
 *   v2/v3 options: --mode --clear-every --temp --rot180 --no-dither --hold --dry --power --xon-line --lead --tail
 *                  --save-mode --mode-file
 *   commands: show <file> [mode] | frame <w> <h> [mode] + w*h bytes | clear [full|stock|init|gc] | refresh | mode <m> |
 *             sleep <s> | status | power off | ping | quit | frametest <p5 file> [mode] (test: a PGM through the frame path)
 *             modes: quality|picture|reading|partial|fast|fastest|a2|auto|N
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/gpio.h>
#include <linux/spi/spidev.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#ifdef A6L_ANDROID_LOG
#include <android/log.h>
#endif

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
static int mode = 2, temp_fallback = 25, rot180, dither = 1, lead = 10, tail = 20, xon_line = -1, dry, updates;
static const char *dry_prefix, *power_path;
static char power_buf[512];

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void logline(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("[%.3f] A6L_EPDD %s\n", now(), b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    __android_log_write(strstr(b, "FAIL") ? ANDROID_LOG_ERROR : strstr(b, "WARN") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "a6l_epdd", b);
#endif
}
#define LOG(...) logline(__VA_ARGS__)

/* ---------------- images (unchanged from v2/v3; load_pnm split into header read + img_from_grey) ---------------- */
static void img_fill(uint8_t v) { uint8_t *p = img.data; for (unsigned i = 0; i < IW * IH; i++) { p[4 * i] = p[4 * i + 1] = p[4 * i + 2] = v; p[4 * i + 3] = 0xff; } }
static void img_set(unsigned X, unsigned Y, uint8_t v) { uint8_t *q = (uint8_t *)img.data + 4 * (Y * IW + X); q[0] = q[1] = q[2] = v; q[3] = 0xff; }
static int tok(FILE *f) {	/* PNM header integer, skipping comments */
    int c, v = 0, got = 0;
    while ((c = fgetc(f)) != EOF) { if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; } if (c >= '0' && c <= '9') { v = v * 10 + c - '0'; got = 1; } else if (got) break; }
    return got ? v : -1;
}
static void img_from_grey(int16_t *g, int w, int h) {	/* g: w*h greys (0..255), consumed (dither works in place) */
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
}
static int size_ok(int w, int h) { return (w == IW && h == IH) || (w == IH && h == IW); }
static int load_pnm(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) { LOG("FAIL open %s: %s", path, strerror(errno)); return -1; }
    char m[3] = {0}; int ok = fread(m, 1, 2, f) == 2, w = tok(f), h = tok(f), mx = tok(f);
    int ch = m[1] == '5' ? 1 : m[1] == '6' ? 3 : 0;
    if (!ok || m[0] != 'P' || !ch || mx != 255 || !size_ok(w, h)) { LOG("FAIL %s: need 8-bit P5/P6 1440x720 or 720x1440 (got %c%c %dx%d max %d)", path, m[0], m[1], w, h, mx); fclose(f); return -1; }
    uint8_t *row = malloc((size_t)w * ch); int16_t *g = malloc((size_t)w * h * sizeof *g); if (!row || !g) { free(row); free(g); fclose(f); return -1; }
    for (int y = 0; y < h; y++) {
        if (fread(row, ch, w, f) != (size_t)w) { LOG("FAIL %s: short file", path); free(row); free(g); fclose(f); return -1; }
        for (int x = 0; x < w; x++) g[y * w + x] = ch == 1 ? row[x] : (int16_t)((row[3 * x] * 77 + row[3 * x + 1] * 150 + row[3 * x + 2] * 29) >> 8);
    }
    img_from_grey(g, w, h);
    free(g);
    free(row); fclose(f); return 0;
}
static int load_grey(const uint8_t *px, int w, int h) {	/* v4 "frame" command */
    if (!size_ok(w, h)) { LOG("FAIL frame %dx%d: need 1440x720 or 720x1440", w, h); return -1; }
    int16_t *g = malloc((size_t)w * h * sizeof *g); if (!g) return -1;
    for (size_t i = 0; i < (size_t)w * h; i++) g[i] = px[i];
    img_from_grey(g, w, h); free(g); return 0;
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
    int f = open(power_path, O_WRONLY | O_CLOEXEC); if (f < 0) { LOG("FAIL open %s: %s", power_path, strerror(errno)); return -1; }
    int r = write(f, on ? "1" : "0", 1) == 1 ? 0 : -1; if (r) LOG("FAIL rails %s: %s", on ? "on" : "off", strerror(errno)); close(f); return r;
}
static void find_power(void) {
    DIR *d = opendir("/sys/bus/mipi-dsi/devices"); struct dirent *e;
    while (d && (e = readdir(d))) { if (e->d_name[0] == '.') continue; snprintf(power_buf, sizeof power_buf, "/sys/bus/mipi-dsi/devices/%s/epd_power", e->d_name); if (!access(power_buf, W_OK)) { power_path = power_buf; break; } }
    if (d) closedir(d);
    if (power_path) { char rp[PATH_MAX]; if (realpath(power_path, rp)) LOG("rail switch %s (label this path for SELinux: sysfs_a6l_epd)", rp); }
}
static void bringup_status(char *out, size_t n) {
    out[0] = 0; if (!power_path) return; char p[520]; snprintf(p, sizeof p, "%.*s/bringup_status", (int)(strrchr(power_path, '/') - power_path), power_path);
    FILE *f = fopen(p, "r"); if (f) { if (!fgets(out, (int)n, f)) out[0] = 0; fclose(f); char *nl = strchr(out, '\n'); if (nl) *nl = 0; }
}
static int xon_fd = -1;
static void xon_hold(void) {
    if (xon_line < 0 || dry) return;
    int chip = open("/dev/gpiochip0", O_RDONLY | O_CLOEXEC); if (chip < 0) { LOG("WARN gpiochip0: %s", strerror(errno)); return; }
    struct gpio_v2_line_request rq; memset(&rq, 0, sizeof rq); rq.offsets[0] = (unsigned)xon_line; rq.num_lines = 1;
    rq.config.flags = GPIO_V2_LINE_FLAG_OUTPUT; rq.config.num_attrs = 1; rq.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    rq.config.attrs[0].attr.values = 1; rq.config.attrs[0].mask = 1; strcpy(rq.consumer, "a6l-epdd-xon");
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &rq)) LOG("WARN XON gpio%d request: %s", xon_line, strerror(errno)); else { xon_fd = rq.fd; LOG("XON gpio%d held high", xon_line); }
    close(chip);
}
static int use_wakelock;
static void wakelock(int on) {
    if (!use_wakelock || dry) return;
    int f = open(on ? "/sys/power/wake_lock" : "/sys/power/wake_unlock", O_WRONLY | O_CLOEXEC); if (f < 0) return;
    if (write(f, "a6l_epdd", 8) != 8) { static int warned; if (!warned++) LOG("WARN wakelock: %s", strerror(errno)); }
    close(f);
}

/* ---------------- waveform: files, then the panel NOR (READ-ONLY), in the order given ---------------- */
/* Panel NOR = Macronix MX25U4033E (JEDEC c2 25 33, 512 KiB) behind BLSP2 QUP4 SPI (DT overlay a6l-eink-flash-read, spidev
 * bound through driver_override). Its power (gpio42 "epd_pwr_on") is an always-on regulator since V71, so no GPIO is
 * touched here. Safety by construction, as epd_nor_read.c: only these opcodes can ever be sent. */
static const uint8_t NOR_ALLOWED[] = {0x9f, 0x05, 0x03};
static int nor_xfer(int spi, const uint8_t *tx, size_t txn, uint8_t *rx, size_t rxn) {
    int ok = 0; for (size_t i = 0; i < sizeof NOR_ALLOWED; i++) ok |= tx[0] == NOR_ALLOWED[i];
    if (!ok) { fprintf(stderr, "A6L_EPDD NOR opcode 0x%02x REFUSED\n", tx[0]); abort(); }
    struct spi_ioc_transfer t[2]; memset(t, 0, sizeof t);
    t[0].tx_buf = (uintptr_t)tx; t[0].len = (uint32_t)txn; t[1].rx_buf = (uintptr_t)rx; t[1].len = (uint32_t)rxn;
    return ioctl(spi, SPI_IOC_MESSAGE(2), t) < 0 ? -errno : 0;
}
static int nor_find(char *dev, size_t n) {	/* spidev whose device is the e-ink flash node */
    DIR *d = opendir("/sys/class/spidev"); struct dirent *e; int found = -1;
    while (d && (e = readdir(d)) && found) {
        if (e->d_name[0] == '.') continue; char p[300], c[128] = {0};
        snprintf(p, sizeof p, "/sys/class/spidev/%s/device/of_node/compatible", e->d_name); FILE *f = fopen(p, "r");
        if (f) { size_t k = fread(c, 1, sizeof c - 1, f); fclose(f); (void)k; }
        if (strstr(c, "ed052tc2") || strstr(c, "a6l-epd-flash")) { snprintf(dev, n, "/dev/%s", e->d_name); found = 0; }
    }
    if (d) closedir(d);
    return found;
}
static int nor_read(const char *arg, uint8_t *dst) {	/* reads FLASH bytes twice, both passes must match */
    char dev[64]; if (!strcmp(arg, "auto")) { if (nor_find(dev, sizeof dev)) { LOG("waveform: no spidev bound to the e-ink flash (DT overlay a6l-eink-flash-read + driver_override=spidev)"); return -1; } }
    else snprintf(dev, sizeof dev, "%s", arg);
    int spi = open(dev, O_RDWR | O_CLOEXEC); if (spi < 0) { LOG("WARN waveform: open %s: %s", dev, strerror(errno)); return -1; }
    uint8_t md = SPI_MODE_0, bits = 8; uint32_t hz = 4000000; int rc = -1; uint8_t *b = NULL;
    if (ioctl(spi, SPI_IOC_WR_MODE, &md) || ioctl(spi, SPI_IOC_WR_BITS_PER_WORD, &bits) || ioctl(spi, SPI_IOC_WR_MAX_SPEED_HZ, &hz)) { LOG("WARN waveform: spi setup: %s", strerror(errno)); goto out; }
    uint8_t id[3] = {0}, op = 0x9f;
    if (nor_xfer(spi, &op, 1, id, 3)) { LOG("WARN waveform: JEDEC read failed"); goto out; }
    if (id[0] != 0xc2 || id[1] != 0x25 || id[2] != 0x33) { LOG("WARN waveform: unexpected NOR JEDEC %02x %02x %02x (want c2 25 33)", id[0], id[1], id[2]); goto out; }
    b = malloc(FLASH); if (!b) goto out;
    for (int pass = 0; pass < 2; pass++) {
        uint8_t *p = pass ? b : dst;
        for (uint32_t a = 0; a < FLASH; a += 4096) { uint8_t cmd[4] = {0x03, (uint8_t)(a >> 16), (uint8_t)(a >> 8), (uint8_t)a};
            uint32_t k = FLASH - a < 4096 ? FLASH - a : 4096; if (nor_xfer(spi, cmd, 4, p + a, k)) { LOG("WARN waveform: NOR read at 0x%x failed", a); goto out; } }
    }
    if (memcmp(b, dst, FLASH)) { LOG("WARN waveform: the two NOR passes differ"); goto out; }
    LOG("waveform: read 0x%x bytes from the panel NOR %s (JEDEC c2 25 33, two identical passes, read-only)", FLASH, dev); rc = 0;
out:
    free(b); close(spi); return rc;
}
static int file_read(const char *path, uint8_t *dst) {
    FILE *f = fopen(path, "rb"); if (!f) { LOG("waveform: %s: %s", path, strerror(errno)); return -1; }
    int ok = fread(dst, 1, FLASH, f) == FLASH; fclose(f);
    if (!ok) { LOG("WARN waveform: %s shorter than 0x%x", path, FLASH); return -1; }
    LOG("waveform: %s", path); return 0;
}
static void cache_write(const char *path, const uint8_t *src) {	/* our own cache file; never the NOR */
    char tmp[600]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    int f = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0640); if (f < 0) { LOG("WARN waveform cache %s: %s", tmp, strerror(errno)); return; }
    int ok = write(f, src, FLASH) == (ssize_t)FLASH && !fsync(f); close(f);
    if (!ok || rename(tmp, path)) { LOG("WARN waveform cache %s not written", path); unlink(tmp); return; }
    LOG("waveform cached in %s", path);
}
struct wsrc { int nor; const char *arg; };
static struct wsrc wsrcs[8]; static int nwsrc; static const char *wcache;
static int waveform_valid(uint8_t *flash);
static uint8_t *load_waveform(void) {
    uint8_t *flash = calloc(1, FLASH); if (!flash) return NULL;
    if (!nwsrc) { wsrcs[0].arg = "/tmp/epd/epd-nor.bin"; nwsrc = 1; }
    for (int i = 0; i < nwsrc; i++) {
        if (wsrcs[i].nor ? nor_read(wsrcs[i].arg, flash) : file_read(wsrcs[i].arg, flash)) continue;
        if (waveform_valid(flash)) { LOG("WARN waveform from %s rejected by the library", wsrcs[i].arg); continue; }
        if (wsrcs[i].nor && wcache) cache_write(wcache, flash);
        return flash;
    }
    free(flash); return NULL;
}

/* ---------------- DRM ---------------- */
struct fb { uint32_t handle, id; uint32_t *map; };
static int dfd = -1, started; static uint32_t crtc, conn_id; static drmModeModeInfo mode_info; static struct fb idle, fa, fb2;
static unsigned last_seq, gaps, flips; static int pending, drm_lost;
static uint32_t *idle_pattern;	/* v4: idle frame content, kept to rebuild the idle fb on a new (lessee) fd */
static int mkfb(struct fb *f) {
    struct drm_mode_create_dumb c = {.width = W, .height = H, .bpp = 32};
    if (drmIoctl(dfd, DRM_IOCTL_MODE_CREATE_DUMB, &c) || c.pitch != W * 4) return -1;
    uint32_t hs[4] = {c.handle}, ps[4] = {c.pitch}, os[4] = {0};
    if (drmModeAddFB2(dfd, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, &f->id, 0)) return -1;
    struct drm_mode_map_dumb m = {.handle = c.handle}; if (drmIoctl(dfd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return -1;
    f->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, m.offset); f->handle = c.handle; return f->map == MAP_FAILED ? -1 : 0;
}
static int lost_errno(int e) { return e == EACCES || e == ENOENT || e == EPERM || e == ENODEV || e == EINVAL; }
static void on_flip(int f, unsigned seq, unsigned s, unsigned us, void *d) { (void)f; (void)s; (void)us; (void)d; if (flips && seq > last_seq + 1) gaps += seq - last_seq - 1; last_seq = seq; flips++; pending = 0; }
static int flip(uint32_t id) {
    drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
    if (drmModePageFlip(dfd, crtc, id, DRM_MODE_PAGE_FLIP_EVENT, 0)) { int e = errno; LOG("FAIL pageflip: %s", strerror(e)); if (lost_errno(e)) drm_lost = 1; return -1; }
    pending = 1;
    while (pending) { struct pollfd p = {.fd = dfd, .events = POLLIN}; if (poll(&p, 1, 1000) <= 0) { LOG("FAIL vblank timeout"); return -1; } drmHandleEvent(dfd, &ev); }
    return 0;
}
/* v3: saved connector + mode (binary: magic, connector id, drmModeModeInfo) */
struct mode_file { uint32_t magic, conn; drmModeModeInfo m; };
#define MODE_MAGIC 0x41364c4du
static const char *mode_file, *save_mode;
static const char *leases[6]; static int nleases;
static int drm_lease_pidfd(const char *arg, int crtc_index);
static int drm_lease_hwc(const char *name);
static int free_crtc(drmModeRes *res, drmModeConnector *con, int *crtc_index) {
    for (int e = 0; e < con->count_encoders; e++) { drmModeEncoder *enc = drmModeGetEncoder(dfd, con->encoders[e]); if (!enc) continue;
        for (int i = 0; i < res->count_crtcs; i++) if (enc->possible_crtcs & (1u << i)) { int used = 0;
            for (int j = 0; j < res->count_encoders; j++) { drmModeEncoder *o = drmModeGetEncoder(dfd, res->encoders[j]); if (o && o->encoder_id != enc->encoder_id && o->crtc_id == res->crtcs[i]) used = 1; drmModeFreeEncoder(o); }
            if (!used) { *crtc_index = i; drmModeFreeEncoder(enc); return (int)res->crtcs[i]; } }
        drmModeFreeEncoder(enc); }
    return 0;
}
/* v4 --no-master (ROM): never open /dev/dri/card* ourselves. The first opener of a DRM primary node becomes master
 * automatically when there is none; if that happened while the composer is (re)starting, the composer could not become
 * master and would lose both displays. So the connector is found in sysfs, and the only DRM fd we ever hold is the
 * lessee fd handed over by the composer. */
static int no_master;
static uint32_t sysfs_connector(char *name, size_t n) {
    DIR *d = opendir("/sys/class/drm"); struct dirent *e; uint32_t id = 0;
    while (d && (e = readdir(d)) && !id) {
        if (strncmp(e->d_name, "card", 4) || !strchr(e->d_name, '-')) continue;
        char p[320], buf[512] = {0}; snprintf(p, sizeof p, "/sys/class/drm/%s/modes", e->d_name); FILE *f = fopen(p, "r"); if (!f) continue;
        size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
        if (!strstr(buf, "384x725")) continue;
        snprintf(p, sizeof p, "/sys/class/drm/%s/connector_id", e->d_name); f = fopen(p, "r"); unsigned v = 0;
        if (f && fscanf(f, "%u", &v) == 1) { id = v; snprintf(name, n, "%s", e->d_name); }
        else LOG("WARN %s has the e-ink mode but no readable connector_id attribute", e->d_name);
        if (f) fclose(f);
    }
    if (d) closedir(d);
    return id;
}
static int drm_open_lessee_only(void) {
    char name[128] = ""; conn_id = sysfs_connector(name, sizeof name);
    if (!conn_id) { LOG("FAIL no e-ink connector (384x725) in /sys/class/drm (panel module loaded?)"); return -1; }
    const char *def[1] = {"hwc"}; const char **l = nleases ? leases : def; int nl = nleases ? nleases : 1, got = 0;
    for (int i = 0; i < nl && !got; i++) if (!strncmp(l[i], "hwc", 3)) got = !drm_lease_hwc(l[i][3] == ':' ? l[i] + 4 : "a6l.hwc.lease");
    if (!got) { LOG("FAIL no lease from the composer for %s (connector %u)", name, conn_id); return -1; }
    drmModeConnector *k = drmModeGetConnector(dfd, conn_id);
    int ok = k && k->count_modes && k->modes[0].hdisplay == W && k->modes[0].vdisplay == H;
    if (ok) mode_info = k->modes[0];
    drmModeFreeConnector(k);
    if (!ok || !crtc) { LOG("FAIL lessee sees no 384x725 mode / crtc (%u)", crtc); close(dfd); dfd = -1; return -1; }
    if (mkfb(&idle) || mkfb(&fa) || mkfb(&fb2)) { LOG("FAIL dumb fb on the lessee"); close(dfd); dfd = -1; return -1; }
    if (idle_pattern) memcpy(idle.map, idle_pattern, FRAME);
    LOG("DRM (lessee only) %s connector=%u crtc=%u mode=%s@%u", name, conn_id, crtc, mode_info.name, mode_info.vrefresh);
    return 0;
}
static int drm_open(void) {
    if (no_master) return drm_open_lessee_only();
    drmModeRes *res = NULL; drmModeConnector *con = NULL; struct mode_file mf = {0}; int rc = -1;
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
    crtc = (uint32_t)free_crtc(res, con, &crtc_index);
    /* ownership: master ourselves (no composer), else the leases in the order given (default: hwc, then pidfd auto) */
    if (!drmSetMaster(dfd)) { if (nleases) LOG("DRM master ourselves (no composer running): no lease needed"); }
    else {
        const char *def[2] = {"hwc", "auto"}; const char **l = nleases ? leases : def; int nl = nleases ? nleases : 2, got = 0;
        for (int i = 0; i < nl && !got; i++) {
            if (!strcmp(l[i], "none")) break;
            if (!strncmp(l[i], "hwc", 3)) got = !drm_lease_hwc(l[i][3] == ':' ? l[i] + 4 : "a6l.hwc.lease");
            else if (crtc) got = !drm_lease_pidfd(l[i], crtc_index);
        }
        if (!got) { LOG("FAIL not DRM master and no lease (composer patched? vendor.hwc.drm.lease_socket set?)"); goto out; }
    }
    if (!crtc) { LOG("FAIL no free crtc"); goto out; }
    if (mkfb(&idle) || mkfb(&fa) || mkfb(&fb2)) { LOG("FAIL dumb fb"); goto out; }
    if (idle_pattern) memcpy(idle.map, idle_pattern, FRAME);
    LOG("DRM connector=%u crtc=%u mode=%s@%u", conn_id, crtc, mode_info.name, mode_info.vrefresh);
    rc = 0;
out:
    drmModeFreeConnector(con); if (res) drmModeFreeResources(res);
    if (rc && dfd >= 0) { close(dfd); dfd = -1; }
    return rc;
}
static void drm_close(void) {	/* v4: lease lost / composer restarted: drop everything, the next update reopens */
    if (dfd >= 0) close(dfd);	/* fbs and dumb buffers go with the file; the mappings stay valid until munmap */
    if (idle.map && idle.map != MAP_FAILED) munmap(idle.map, FRAME);
    if (fa.map && fa.map != MAP_FAILED) munmap(fa.map, FRAME);
    if (fb2.map && fb2.map != MAP_FAILED) munmap(fb2.map, FRAME);
    memset(&idle, 0, sizeof idle); memset(&fa, 0, sizeof fa); memset(&fb2, 0, sizeof fb2);
    dfd = -1; started = 0; crtc = 0; drm_lost = 0; pending = 0;
}
/* v4: lease from the patched drm_hwcomposer (drm/LeaseServer.cpp): abstract SOCK_SEQPACKET, "LEASE <connector id>" ->
 * "OK lessee=.. connector=.. crtc=.. planes=.." + the lessee fd (SCM_RIGHTS). */
static int drm_lease_hwc(const char *name) {
    int s = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0); if (s < 0) return -1;
    struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX; size_t nl = strlen(name);
    if (nl + 1 > sizeof a.sun_path) { close(s); return -1; }
    memcpy(a.sun_path + 1, name, nl);
    if (connect(s, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl))) { LOG("lease hwc @%s: %s", name, strerror(errno)); close(s); return -1; }
    char req[48]; int n = snprintf(req, sizeof req, "LEASE %u", conn_id);
    char rep[256] = {0}; union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int))]; } cb; memset(&cb, 0, sizeof cb);
    struct iovec iov = {rep, sizeof rep - 1}; struct msghdr m = {0}; m.msg_iov = &iov; m.msg_iovlen = 1; m.msg_control = cb.b; m.msg_controllen = sizeof cb.b;
    struct pollfd p = {s, POLLIN, 0}; int lfd = -1; ssize_t r = -1;
    if (send(s, req, (size_t)n, MSG_NOSIGNAL) == n && poll(&p, 1, 3000) == 1) r = recvmsg(s, &m, MSG_CMSG_CLOEXEC);
    close(s);
    if (r <= 0) { LOG("WARN lease hwc @%s: no reply", name); return -1; }
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) memcpy(&lfd, CMSG_DATA(c), sizeof lfd);
    LOG("lease hwc @%s: %s", name, rep);
    if (strncmp(rep, "OK ", 3) || lfd < 0) { if (lfd >= 0) close(lfd); return -1; }
    char *cp = strstr(rep, "crtc="); if (cp) crtc = (uint32_t)strtoul(cp + 5, NULL, 10);
    if (dfd >= 0) close(dfd);
    dfd = lfd; return 0;
}
/* v3: lease from the current DRM master (unpatched drm_hwcomposer, RAM session). pidfd_getfd needs Linux >= 5.6 and
 * ptrace rights (root; SELinux permissive or a ptrace allow). */
#ifndef __NR_pidfd_open
#define __NR_pidfd_open 434
#endif
#ifndef __NR_pidfd_getfd
#define __NR_pidfd_getfd 438
#endif
static int drm_lease_pidfd(const char *lease_arg, int crtc_index) {
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
    if (fstat(dfd, &me)) return -1;
    int want = strcmp(lease_arg, "auto") ? atoi(lease_arg) : 0, lfd = -1;
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
            if (!fstat(mfd, &st) && st.st_rdev == me.st_rdev) { uint32_t lessee = 0; lfd = drmModeCreateLease(mfd, objs, n, O_CLOEXEC, &lessee);
                if (lfd < 0) lfd = -1; else LOG("lease %u created with %d objects", lessee, n);
                LOG("pid %d fd %s (%s): %s", pid, fe->d_name, tgt, lfd >= 0 ? "master, lease granted" : strerror(errno)); }
            close(mfd);
        }
        if (pidfd >= 0) close(pidfd); closedir(fdd);
    }
    if (pd) closedir(pd);
    if (lfd < 0) { LOG("WARN no DRM master process granted a lease (pidfd path)"); return -1; }
    close(dfd); dfd = lfd; return 0;
}
static int drm_start(void) {	/* modeset = panel prepare+enable = kernel bring-up (V73 panel driver) */
    char st[200];
    for (int attempt = 1; attempt <= 3; attempt++) {
        if (drmModeSetCrtc(dfd, crtc, idle.id, 0, 0, &conn_id, 1, &mode_info)) { int e = errno; LOG("FAIL setcrtc: %s", strerror(e)); if (lost_errno(e)) drm_lost = 1; return -1; }
        for (int i = 0; i < 10; i++) flip(idle.id);
        bringup_status(st, sizeof st); LOG("bring-up %d: %s", attempt, st[0] ? st : "(no status attribute)");
        if (!st[0] || strstr(st, "bridge_ok=1")) return 0;
        drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); sleep(1);
    }
    return -1;
}
static void crtc_off(const char *why) {
    if (dry || dfd < 0 || !crtc || !started) return;
    drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); started = 0; LOG("e-ink CRTC off (%s)", why);
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
static double last_update_t; static int last_ms, fails_total, lib_only;
static int drive(int n) {	/* scan out the n frames of the current update; rails only around the drive (v2) */
    if (!started) { if (drm_start()) return -1; started = 1; }	/* modeset with the real strobe pattern, as a6l_epd_play did */
    unsigned g0 = gaps; int rc = 0; double t1 = now();
    if (power(1)) LOG("WARN rails not switched on");
    for (int i = 0; i < lead && !rc; i++) rc = flip(idle.id);
    unsigned gd = gaps;
    for (int k = 0; k < n && !rc; k++) { struct fb *f = (k & 1) ? &fb2 : &fa; memcpy(f->map, frames[k], FRAME); rc = flip(f->id); }
    gd = gaps - gd;
    for (int i = 0; i < tail && !rc; i++) rc = flip(idle.id);
    power(0);
    last_ms = (int)((now() - t1) * 1000);
    LOG("update %d shown in %d ms: %s, missed vblanks during drive=%u (total %u)", updates, last_ms, rc ? "FAILED" : "ok", gd, gaps - g0);
    return rc;
}
static int run_update(int force, int m, const char *what) {
    int t = temperature(); double t0 = now();
    int nf = tc_decide(&img, handle, t, t, force, m), n = 0; uint8_t more = 1;
    while (more && n < MAXF) { struct buf *b = &ring[n % RING]; more = tc_update(b, handle);
        if (!frames[n] && !(frames[n] = malloc(FRAME))) { LOG("FAIL out of memory"); return -1; }
        memcpy(frames[n], b->data, FRAME); n++; }
    updates++; LOG("update %d (%s): mode=%d force=%d temp=%dC decision=%d frames=%d generated in %.0f ms", updates, what, m, force, t, nf, n, (now() - t0) * 1000);
    if (dry) { write_a6lepd(n); return 0; }
    if (lib_only) return 0;
    if (!idle_pattern) { if (!(idle_pattern = malloc(FRAME))) return -1; for (unsigned i = 0; i < FRAME / 4; i++) idle_pattern[i] = frames[0][i] & 0xffffff00u; }
    int rc = -1;
    wakelock(1);
    for (int attempt = 0; attempt < 2 && rc; attempt++) {
        if (dfd < 0 && drm_open()) break;
        if (idle.map && !started) memcpy(idle.map, idle_pattern, FRAME);
        rc = drive(n);
        if (rc && drm_lost) { LOG("WARN DRM access lost (lease revoked / composer restarted?): re-acquiring and driving again"); drm_close(); }
        else break;
    }
    wakelock(0);
    last_update_t = now(); if (rc) fails_total++;
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
/* Mode map (v2): 2 = GC16 full (39 frames), 0/3/4/5 = REGAL 16-grey partial (39), 1 = fast partial fewer greys (23),
 * >5 = A2 black/white (10). 0 = the library's own content analysis. Switching mode costs one transition update. */
static int mode_by_name(const char *n, int def) {
    if (!n || !*n) return def;
    if (!strcmp(n, "quality") || !strcmp(n, "picture")) return 2; if (!strcmp(n, "partial") || !strcmp(n, "reading")) return 3;
    if (!strcmp(n, "fast")) return 1; if (!strcmp(n, "fastest") || !strcmp(n, "a2")) return 8; if (!strcmp(n, "auto")) return 0;
    return atoi(n);
}
static int clear_every, since_clear;
static int show_loaded(int m, const char *what) {
    memcpy(last_img, img.data, RGBA); have_last = 1; return run_update(0, m, what);
}
static int show(const char *path, int m) {
    if (clear_every > 0 && ++since_clear >= clear_every) { since_clear = 0; clear_full(); }
    if (load_pnm(path)) return -1; return show_loaded(m, path);
}

/* ---------------- commands (v4: one reply per command) ---------------- */
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static double t_boot;
/* returns 0/-1; reply gets "OK ..." or "ERR ..." (without newline). For "frame", *need = bytes of pixels to read first. */
static int exec_cmd(const char *line, const uint8_t *payload, char *reply, size_t rn);
static int exec_cmd(const char *line, const uint8_t *payload, char *reply, size_t rn) {
    char path[512], mname[32] = ""; int m, rc = 0, w = 0, h = 0;
    if (!strcmp(line, "ping")) { snprintf(reply, rn, "OK pong"); return 0; }
    if (!strcmp(line, "quit")) { stop = 1; snprintf(reply, rn, "OK quitting"); return 0; }
    if (!strcmp(line, "status")) {
        char st[200]; bringup_status(st, sizeof st);
        snprintf(reply, rn, "OK updates=%d fails=%d last_ms=%d crtc=%s drm=%s mode=%d idle_s=%.0f uptime_s=%.0f bringup=%s", updates, fails_total, last_ms,
                 started ? "on" : "off", dfd >= 0 ? (no_master ? "lessee" : "open") : "closed", mode, last_update_t ? now() - last_update_t : -1, now() - t_boot, st[0] ? st : "-");
        return 0;
    }
    if (!strcmp(line, "power off")) { crtc_off("requested"); snprintf(reply, rn, "OK crtc off"); return 0; }
    if (!strncmp(line, "mode ", 5)) { mode = mode_by_name(line + 5, mode); snprintf(reply, rn, "OK mode=%d", mode); return 0; }
    if (!strcmp(line, "clear")) rc = clear_full();
    else if (!strncmp(line, "clear ", 6)) rc = clear_kind(line + 6);
    else if (!strcmp(line, "refresh")) rc = refresh();
    else if (!strncmp(line, "sleep ", 6)) { sleep((unsigned)atoi(line + 6)); rc = 0; }
    else if (sscanf(line, "frame %d %d %31s", &w, &h, mname) >= 2) {
        if (!payload) { snprintf(reply, rn, "ERR frame needs a pixel payload (socket only)"); return -1; }
        m = mode_by_name(mname, mode);
        if (clear_every > 0 && ++since_clear >= clear_every) { since_clear = 0; clear_full(); }
        rc = load_grey(payload, w, h); if (!rc) rc = show_loaded(m, "frame");
    }
    else if (sscanf(line, "frametest %511s %31s", path, mname) >= 1) {	/* test hook: a P5 file through the "frame" path */
        FILE *f = fopen(path, "rb"); char mg[3] = {0}; int ok = f && fread(mg, 1, 2, f) == 2 && mg[0] == 'P' && mg[1] == '5';
        int fw = ok ? tok(f) : -1, fh = ok ? tok(f) : -1, mx = ok ? tok(f) : -1; uint8_t *px = NULL;
        ok = ok && mx == 255 && size_ok(fw, fh) && (px = malloc((size_t)fw * fh)) && fread(px, 1, (size_t)fw * fh, f) == (size_t)fw * fh;
        if (f) fclose(f);
        if (!ok) { free(px); snprintf(reply, rn, "ERR frametest %s: need P5 720x1440/1440x720", path); return -1; }
        char hdr[64]; snprintf(hdr, sizeof hdr, "frame %d %d %s", fw, fh, mname[0] ? mname : "");
        rc = exec_cmd(hdr, px, reply, rn); free(px); return rc;
    }
    else if (sscanf(line, "show %511s %31s", path, mname) >= 1) { m = mode_by_name(mname, mode); rc = show(path, m); }
    else { LOG("unknown command '%s'", line); snprintf(reply, rn, "ERR unknown command"); return -1; }
    if (rc) snprintf(reply, rn, "ERR update failed (see log)");
    else snprintf(reply, rn, "OK shown in %d ms (update %d)", last_ms, updates);
    return rc;
}
static void commands(FILE *q) {	/* script / FIFO: same commands, replies only logged */
    char line[600], reply[512];
    while (!stop && fgets(line, sizeof line, q)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (!line[0] || line[0] == '#') continue;
        exec_cmd(line, NULL, reply, sizeof reply); LOG("reply: %s", reply);
    }
}
/* socket server: stream clients, "\n"-terminated lines; "frame W H [MODE]\n" is followed by W*H bytes */
#define MAXCL 4
struct client { int fd; size_t len, need; char *buf; char hdr[128]; };
static struct client cl[MAXCL];
static void client_drop(struct client *c) { if (c->fd >= 0) close(c->fd); free(c->buf); memset(c, 0, sizeof *c); c->fd = -1; }
static void client_reply(struct client *c, const char *r) { char b[512]; int n = snprintf(b, sizeof b, "%s\n", r); if (send(c->fd, b, (size_t)n, MSG_NOSIGNAL) != n) LOG("WARN reply lost"); }
static void client_input(struct client *c) {
    if (!c->buf && !(c->buf = malloc(IW * IH + 700))) { client_drop(c); return; }
    size_t cap = IW * IH + 700; ssize_t r = recv(c->fd, c->buf + c->len, cap - c->len, 0);
    if (r <= 0) { client_drop(c); return; }
    c->len += (size_t)r;
    for (;;) {
        if (c->need) {	/* waiting for a frame payload */
            if (c->len < c->need) return;
            char reply[512]; exec_cmd(c->hdr, (uint8_t *)c->buf, reply, sizeof reply); client_reply(c, reply);
            memmove(c->buf, c->buf + c->need, c->len - c->need); c->len -= c->need; c->need = 0; continue;
        }
        char *nl = memchr(c->buf, '\n', c->len);
        if (!nl) { if (c->len >= 600) { client_reply(c, "ERR line too long"); client_drop(c); } return; }
        *nl = 0; size_t used = (size_t)(nl - c->buf) + 1; int w = 0, h = 0;
        if (sscanf(c->buf, "frame %d %d", &w, &h) == 2) {
            if (!size_ok(w, h)) { client_reply(c, "ERR frame size must be 720x1440 or 1440x720"); client_drop(c); return; }
            snprintf(c->hdr, sizeof c->hdr, "%s", c->buf); c->need = (size_t)w * h;
        } else if (c->buf[0] && c->buf[0] != '#') { char reply[512]; exec_cmd(c->buf, NULL, reply, sizeof reply); client_reply(c, reply); }
        memmove(c->buf, c->buf + used, c->len - used); c->len -= used;
    }
}
static int idle_off_s;
static void serve(int lfd) {
    for (int i = 0; i < MAXCL; i++) cl[i].fd = -1;
    while (!stop) {
        struct pollfd p[MAXCL + 1]; int n = 0; p[n++] = (struct pollfd){lfd, POLLIN, 0};
        for (int i = 0; i < MAXCL; i++) if (cl[i].fd >= 0) p[n++] = (struct pollfd){cl[i].fd, POLLIN, 0};
        int r = poll(p, (nfds_t)n, 1000);
        if (r < 0 && errno != EINTR) { LOG("FAIL poll: %s", strerror(errno)); break; }
        if (idle_off_s > 0 && started && last_update_t && now() - last_update_t > idle_off_s) crtc_off("idle");
        if (r <= 0) continue;
        if (p[0].revents & POLLIN) { int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC); if (c >= 0) { int k = 0; while (k < MAXCL && cl[k].fd >= 0) k++;
                if (k == MAXCL) { const char *b = "ERR busy\n"; if (send(c, b, strlen(b), MSG_NOSIGNAL) < 0) {} close(c); } else { memset(&cl[k], 0, sizeof cl[k]); cl[k].fd = c; } } }
        for (int j = 1; j < n; j++) if (p[j].revents & (POLLIN | POLLHUP | POLLERR)) for (int i = 0; i < MAXCL; i++) if (cl[i].fd == p[j].fd) { client_input(&cl[i]); break; }
    }
    for (int i = 0; i < MAXCL; i++) if (cl[i].fd >= 0) client_drop(&cl[i]);
}
static int listen_path(const char *path) {
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); struct sockaddr_un a; memset(&a, 0, sizeof a); a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof a.sun_path, "%s", path); unlink(path);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4)) { LOG("FAIL listen %s: %s", path, strerror(errno)); if (s >= 0) close(s); return -1; }
    chmod(path, 0660); return s;
}
static int init_socket(const char *name) {	/* init "socket NAME stream ..." -> env ANDROID_SOCKET_NAME=fd (same as android_get_control_socket) */
    char k[96]; snprintf(k, sizeof k, "ANDROID_SOCKET_%s", name); for (char *c = k; *c; c++) if (*c == '-' || *c == '.') *c = '_';
    const char *v = getenv(k); if (!v) { LOG("FAIL no init socket %s (env %s)", name, k); return -1; }
    int s = atoi(v); fcntl(s, F_SETFD, FD_CLOEXEC); if (listen(s, 4) && errno != EINVAL) LOG("WARN listen init socket: %s", strerror(errno)); return s;
}

static int waveform_valid(uint8_t *flash) {
    uint32_t cfg[3] = {IW, IH, 50}; static uint8_t info[0x400];
    memset(info, 0, sizeof info);
    handle = tc_init(ring, RING, cfg, flash, FLASH, info);
    if (!handle) return -1;
    LOG("TCON ready: panel %.15s waveform %.31s", (char *)info, (char *)info + 47);
    return 0;
}

int main(int argc, char **argv) {
    const char *lib = "libtcon_eink.so", *fifo = NULL, *script = NULL, *sock_name = NULL, *listen_at = NULL; const char *shows[32]; int nshow = 0, hold = 5, wait_drm = 0, startup_clear = 1, wl = -1;
    t_boot = now();
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(a, "--waveform") && v) { if (nwsrc < 8) { wsrcs[nwsrc].nor = 0; wsrcs[nwsrc++].arg = v; } i++; }
        else if (!strcmp(a, "--nor-spidev") && v) { if (nwsrc < 8) { wsrcs[nwsrc].nor = 1; wsrcs[nwsrc++].arg = v; } i++; }
        else if (!strcmp(a, "--waveform-cache") && v) wcache = argv[++i];
        else if (!strcmp(a, "--lib") && v) lib = argv[++i];
        else if (!strcmp(a, "--mode") && v) mode = mode_by_name(argv[++i], 2); else if (!strcmp(a, "--clear-every") && v) clear_every = atoi(argv[++i]); else if (!strcmp(a, "--temp") && v) temp_fallback = atoi(argv[++i]);
        else if (!strcmp(a, "--rot180")) rot180 = 1; else if (!strcmp(a, "--no-dither")) dither = 0; else if (!strcmp(a, "--show") && v && nshow < 32) shows[nshow++] = argv[++i];
        else if (!strcmp(a, "--hold") && v) hold = atoi(argv[++i]); else if (!strcmp(a, "--fifo") && v) fifo = argv[++i]; else if (!strcmp(a, "--script") && v) script = argv[++i];
        else if (!strcmp(a, "--dry") && v) { dry = 1; dry_prefix = argv[++i]; } else if (!strcmp(a, "--power") && v) power_path = argv[++i];
        else if (!strcmp(a, "--xon-line") && v) xon_line = atoi(argv[++i]); else if (!strcmp(a, "--lead") && v) lead = atoi(argv[++i]);
        else if (!strcmp(a, "--tail") && v) tail = atoi(argv[++i]);
        else if (!strcmp(a, "--lease") && v) { if (nleases < 6) leases[nleases++] = v; i++; } else if (!strcmp(a, "--mode-file") && v) mode_file = argv[++i];
        else if (!strcmp(a, "--save-mode") && v) save_mode = argv[++i];
        else if (!strcmp(a, "--no-master")) no_master = 1; else if (!strcmp(a, "--wait-drm") && v) wait_drm = atoi(argv[++i]);
        else if (!strcmp(a, "--socket") && v) sock_name = argv[++i]; else if (!strcmp(a, "--listen") && v) listen_at = argv[++i];
        else if (!strcmp(a, "--idle-off") && v) idle_off_s = atoi(argv[++i]); else if (!strcmp(a, "--no-startup-clear")) startup_clear = 0;
        else if (!strcmp(a, "--wakelock")) wl = 1; else if (!strcmp(a, "--no-wakelock")) wl = 0;
        else { fprintf(stderr, "usage: see source header (%s)\n", a); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN); setvbuf(stdout, NULL, _IOLBF, 0);
    use_wakelock = wl >= 0 ? wl : (sock_name || listen_at);
    if (save_mode) return drm_open() ? 1 : 0;	/* v3: only record connector + mode (drm_open exits) */
    int lfd = -1;	/* take the socket early so clients can connect (they get replies once we are ready) */
    if (sock_name) lfd = init_socket(sock_name); else if (listen_at) lfd = listen_path(listen_at);
    if ((sock_name || listen_at) && lfd < 0) return 1;
    /* library + waveform */
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL); if (!h) { LOG("FAIL dlopen %s: %s", lib, dlerror()); return 1; }
    tc_init = dlsym(h, "Init_Eink_SWTcon"); tc_decide = dlsym(h, "ModeDecision_MirrorMode"); tc_update = dlsym(h, "Update_Display_Image");
    if (!tc_init || !tc_decide || !tc_update) { LOG("FAIL symbols"); return 1; }
    for (int i = 0; i < RING; i++) { ring[i].data = calloc(1, FRAME); ring[i].size = FRAME; if (!ring[i].data) return 1; }
    img.data = calloc(1, RGBA); img.size = RGBA; last_img = malloc(RGBA); if (!img.data || !last_img) return 1;
    uint8_t *flash = load_waveform();
    if (!flash) { LOG("FAIL no usable waveform (tried %d sources)", nwsrc); return 1; }
    if (!dry) {
        if (!power_path) find_power(); LOG("rail switch: %s", power_path ? power_path : "NONE"); xon_hold();
        for (int t = 0; drm_open(); t++) {
            if (t >= wait_drm || stop) { LOG("FAIL DRM not available%s", wait_drm ? " (gave up waiting)" : ""); if (lfd < 0) return 1; break; }
            sleep(1);
        }
    }
    /* start-up: call 1 = full clear (INIT: +0x270 set by Init), call 2 = the library's reset to white (both shown).
     * Always run through the library (its state machine needs calls 1+2); without DRM they fail and are retried by the
     * first real update's re-acquire. --no-startup-clear skips only the flash on the panel, not the library calls. */
    if (startup_clear || dry) {
        img_fill(0xff); if (run_update(1, 0, "clear") && lfd < 0) goto out;
        img_fill(0xff); if (run_update(0, mode, "reset-to-white") && lfd < 0) goto out;
    } else { lib_only = 1; img_fill(0xff); run_update(1, 0, "clear(library only)"); img_fill(0xff); run_update(0, mode, "reset-to-white(library only)"); lib_only = 0; }
    LOG("after start-up: init flag=%u calls=%u", *H_INIT_FLAG(handle), *H_CALLS(handle));
    for (int i = 0; i < nshow && !stop; i++) { if (show(shows[i], mode)) LOG("show %s failed", shows[i]); for (int s = 0; s < hold && !stop; s++) sleep(1); }
    if (script) { FILE *q = fopen(script, "r"); if (!q) LOG("FAIL script %s", script); else { commands(q); fclose(q); } }
    if (lfd >= 0) { LOG("serving %s%s", sock_name ? "init socket " : "", sock_name ? sock_name : listen_at); serve(lfd); close(lfd); }
    else if (fifo) {
        mkfifo(fifo, 0660); LOG("serving %s", fifo);
        while (!stop) { FILE *q = fopen(fifo, "r"); if (!q) { LOG("FAIL fifo %s", fifo); break; } commands(q); fclose(q); }
    }
out:
    if (!dry) { power(0); if (crtc && dfd >= 0) drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); }
    if (xon_fd >= 0) { struct gpio_v2_line_values v = {0, 1}; ioctl(xon_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v); close(xon_fd); }
    LOG("exit");
    return 0;
}
