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
 *   --crtc-wakelock 0|1 (default 1): wakelock "a6l_epdd_crtc" while the e-ink CRTC is on (eink-round3: never suspend
 *                  with the lessee CRTC enabled; timed idle-off + 15 s, released by power off / idle-off / exit)
 *   v2/v3 options: --mode --clear-every --temp --rot180 --no-dither --dither fs|ordered|none --hold --dry --power --xon-line --lead --tail
 *   --overlap-gen 0|1 (eink-round2, experimental, default 0): generate the waveform frames while rails-on + lead scans run
 *   --chain 0|1 (eink-round4, default 0, rc 1): reply right after the waveform; keep the rails up over idle frames for the
 *                  tail time and start a frame that arrives meanwhile without rails-off/on and lead scans
 *                  --save-mode --mode-file
 *   commands: show <file> [mode] | frame <w> <h> [mode] + w*h bytes | clear [full|stock|init|gc] | refresh | mode <m> |
 *             sleep <s> | status | power off | ping | quit | frametest <p5 file> [mode] (test: a PGM through the frame path)
 *             modes: quality|picture|reading|partial|fast|fastest|a2|auto|N;
 *             frame ... clean = legacy forced mode-2 redraw;
 *             frame W H MODE force = stock-style ghost refresh retaining MODE, without white/INIT.
 *   eink-lockscreen (6 Oct 2026, firmware/extracted/eink-lockscreen-20261006): two commands for a6l_einklock, the e-ink
 *   lock screen drawn while Android sleeps on the e-ink:
 *     lockframe W H [MODE] [force] + W*H bytes: like "frame" (default MODE reading = REGAL, only changed pixels driven), but
 *             the picture the panel showed before the first lock frame (the mirror's) is kept, and the e-ink CRTC is
 *             switched off right after the update (its no-suspend lock released: the system may suspend between ticks);
 *     lock restore [off]: the lock ended; if the lock picture is still on the panel (no other picture since), redraw the
 *             kept picture (REGAL) so the panel matches what the mirror believes it shows — the mirror only sends pages
 *             that differ from ITS last frame. "off": switch the CRTC off afterwards (not awake on the e-ink).
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
#include <pthread.h>
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
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
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
static int last_mode = 2; /* Preserve the requested waveform across stock-style force clears. */
static int have_last;
#define H_INIT_FLAG(h) ((volatile uint32_t *)((uint8_t *)(h) + 0x270))	/* 1 = next update uses the INIT waveform */
#define H_CALLS(h) ((volatile uint32_t *)((uint8_t *)(h) + 0x274))	/* ModeDecision_MirrorMode call counter */
static struct buf ring[RING], img;
static uint32_t *frames[MAXF];
static int mode = 2, temp_fallback = 25, rot180, dither = 2 /* ordered: position-stable, see eink-stock-analysis-20261006 */, lead = 10, tail = 20, xon_line = -1, dry, updates;
static const char *dry_prefix, *power_path;
static char power_buf[512];

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
#ifdef A6L_ANDROID_LOG
/* Match Java elapsedRealtime across suspend; scheduling still uses now(). */
static double boot_now(void) { struct timespec t; if (clock_gettime(CLOCK_BOOTTIME, &t)) return now(); return t.tv_sec + t.tv_nsec / 1e9; }
#endif
static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#include <stdarg.h>
static void logline(const char *fmt, ...) {
    char b[1024]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    printf("[%.3f] A6L_EPDD %s\n", now(), b); fflush(stdout);
#ifdef A6L_ANDROID_LOG
    char stamped[sizeof b + 128];
    snprintf(stamped, sizeof stamped, "boot_ms=%.3f mono_ms=%.3f %s", boot_now() * 1000, now() * 1000, b);
    __android_log_write(strstr(b, "FAIL") ? ANDROID_LOG_ERROR : strstr(b, "WARN") ? ANDROID_LOG_WARN : ANDROID_LOG_INFO, "a6l_epdd", stamped);
#endif
}
#define LOG(...) logline(__VA_ARGS__)
/* eink-round5: display-transition markers in the kernel log ("<6>a6l_epdd: ..."; /dev/kmsg is root:system 0620, printk.devkmsg=on).
 * One timeline with the DPU/DSI/SMMU/PM messages for the round-5 repro (kmsg streamed to the laptop / fsync'ed on the
 * phone). Android builds only: host tests never write the host's kernel log. */
static void kmark(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void kmark(const char *fmt, ...) {
#ifdef __ANDROID__
    static int fd = -2;
    if (fd == -2) fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char b[300]; int n = snprintf(b, sizeof b, "<6>a6l_epdd: "); va_list ap; va_start(ap, fmt);
    int m = vsnprintf(b + n, sizeof b - (size_t)n - 1, fmt, ap); va_end(ap);
    if (m < 0) return;
    n += m; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n'; if (write(fd, b, (size_t)n) < 0) { /* best effort */ }
#else
    (void)fmt;
#endif
}
#define KLOG(...) do { LOG(__VA_ARGS__); kmark(__VA_ARGS__); } while (0)

/* ---------------- images (unchanged from v2/v3; load_pnm split into header read + img_from_grey) ---------------- */
static void img_fill(uint8_t v) { uint8_t *p = img.data; for (unsigned i = 0; i < IW * IH; i++) { p[4 * i] = p[4 * i + 1] = p[4 * i + 2] = v; p[4 * i + 3] = 0xff; } }
static void img_set(unsigned X, unsigned Y, uint8_t v) { uint8_t *q = (uint8_t *)img.data + 4 * (Y * IW + X); q[0] = q[1] = q[2] = v; q[3] = 0xff; }
static int tok(FILE *f) {	/* PNM header integer, skipping comments */
    int c, v = 0, got = 0;
    while ((c = fgetc(f)) != EOF) { if (c == '#') { while ((c = fgetc(f)) != EOF && c != '\n') {} continue; } if (c >= '0' && c <= '9') { v = v * 10 + c - '0'; got = 1; } else if (got) break; }
    return got ? v : -1;
}
/* Anchored in logical input coordinates. No diffusion, frame state or RNG. */
static const uint8_t bayer8[8][8] = {
    { 0,48,12,60, 3,51,15,63}, {32,16,44,28,35,19,47,31},
    { 8,56, 4,52,11,59, 7,55}, {40,24,36,20,43,27,39,23},
    { 2,50,14,62, 1,49,13,61}, {34,18,46,30,33,17,45,29},
    {10,58, 6,54, 9,57, 5,53}, {42,26,38,22,41,25,37,21}
};
static const char *dither_name(int value) {
    return value == 2 ? "ordered" : value == 1 ? "fs" : "none";
}
static int dither_parse(const char *name) {
    if (!strcmp(name,"fs")) return 1;
    if (!strcmp(name,"ordered")) return 2;
    if (!strcmp(name,"none")) return 0;
    return -1;
}
static int ordered16(int value, unsigned x, unsigned y) {
    int g = value < 0 ? 0 : value > 255 ? 255 : value;
    int level = g / 17, remainder = g % 17;
    /* Midpoint thresholds: mean bias <= 17/128 gray units over an 8x8 cell. */
    if (128 * remainder > 17 * (2 * bayer8[y & 7][x & 7] + 1)) level++;
    return level * 17;
}
static double input_dither_ms, input_pack_ms;
static void img_from_grey(int16_t *g, int w, int h) {	/* g: w*h greys (0..255), consumed (dither works in place) */
    double t0 = now();
    if (dither == 1) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {	/* Floyd-Steinberg to the panel's 16 levels (r145: 16 visible bands) */
        int old = g[y * w + x], q = (old < 0 ? 0 : old > 255 ? 255 : old); q = (q + 8) / 17 * 17; int e = old - q; g[y * w + x] = (int16_t)q;
        if (!e) continue; /* Exact diffusion by zero changes no neighbour. */
        if (x + 1 < w) g[y * w + x + 1] += (int16_t)(e * 7 / 16);
        if (y + 1 < h) { if (x > 0) g[(y + 1) * w + x - 1] += (int16_t)(e * 3 / 16); g[(y + 1) * w + x] += (int16_t)(e * 5 / 16); if (x + 1 < w) g[(y + 1) * w + x + 1] += (int16_t)(e / 16); }
    }
    else if (dither == 2) for (int y = 0; y < h; y++) for (int x = 0; x < w; x++)
        g[y * w + x] = (int16_t)ordered16(g[y * w + x], (unsigned)x, (unsigned)y);
    input_dither_ms = (now() - t0) * 1000; t0 = now();
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        int v0 = g[y * w + x]; uint8_t v = (uint8_t)(v0 < 0 ? 0 : v0 > 255 ? 255 : v0);
        int xx = rot180 ? w - 1 - x : x, yy = rot180 ? h - 1 - y : y;
        if (w == IW) img_set(IW - 1 - xx, yy, v);	/* landscape as seen: library X is mirrored (r135) */
        else img_set(yy, xx, v);			/* portrait, camera on top: library(X,Y) = image(row X, col Y) */
    }
    input_pack_ms = (now() - t0) * 1000;
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
/* Explicit one-slot diagnostic. No file IO unless armed through the existing
 * authenticated daemon socket; snapshots are private and files never overwrite. */
#ifndef A6L_TRACE_DIR
#define A6L_TRACE_DIR "/data/vendor/epd"
#endif
enum { TRACE_OFF, TRACE_ARMED, TRACE_CAPTURED, TRACE_DONE };
static struct {
    int state, w, h, mode, force, update, frames, decision, temperature, drive_attempted;
    uint8_t *input, *post;
    char token[33], command[128];
    double input_ms, post_ms;
} exact_trace;
static int trace_getprop(const char *key, char *value, size_t size) {
#ifdef A6L_TRACE_TEST
    return test_trace_getprop(key,value,size);
#elif defined(__ANDROID__)
    char buffer[PROP_VALUE_MAX] = {0};
    int n=__system_property_get(key,buffer);
    snprintf(value,size,"%s",buffer); return n;
#else
    (void)key; if (size) value[0]=0; return 0;
#endif
}
static int trace_debug_build(void) {
#if defined(__ANDROID__) || defined(A6L_TRACE_TEST)
    /* This retained userdebug ROM deliberately disables adb root/debuggable.
     * The readonly build type is the native gate; shell arming requires the existing permissive development policy. */
    char value[16]; trace_getprop("ro.build.type",value,sizeof value);
    return !strcmp(value,"userdebug") || !strcmp(value,"eng");
#else
    return 1; /* Explicit host socket/test hook, never used in Android builds. */
#endif
}
static double trace_boot_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_BOOTTIME, &t)) clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}
static int trace_arm(const char *token) {
    size_t n = strlen(token);
    if (!trace_debug_build() || exact_trace.state != TRACE_OFF || !n || n > 32) return -1;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)token[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return -1;
    }
    uint8_t *input = malloc(IW * IH), *post = malloc(IW * IH);
    if (!input || !post) { free(input); free(post); return -1; }
    memset(&exact_trace, 0, sizeof exact_trace);
    exact_trace.input = input; exact_trace.post = post;
    memcpy(exact_trace.token, token, n+1);
    exact_trace.state = TRACE_ARMED;
    LOG("exact trace %s: armed for one fully received frame", token);
    return 0;
}
static int trace_cancel(void) {
    if (exact_trace.state != TRACE_ARMED) return -1;
    free(exact_trace.input); free(exact_trace.post);
    memset(&exact_trace, 0, sizeof exact_trace);
    return 0;
}
static void trace_poll(void) {
    if (exact_trace.state == TRACE_CAPTURED || exact_trace.state == TRACE_DONE || !trace_debug_build()) return;
    char token[96]; static char last[96];
    trace_getprop("vendor.a6l.eink.trace_once",token,sizeof token);
    if (!strcmp(token,last)) return;
    snprintf(last,sizeof last,"%s",token);
    if (!token[0]) return; /* Empty means disabled, not an implicit cancel. */
    if (!strcmp(token,"cancel")) { if (exact_trace.state == TRACE_ARMED) trace_cancel(); return; }
    if (trace_arm(token)) LOG("exact trace: arming refused (invalid token, occupied slot, or allocation failure)");
}
static void trace_capture(const uint8_t *payload, int w, int h, int m,
                          int force, const char *command) {
    if (exact_trace.state != TRACE_ARMED) return;
    if (!((w == IW && h == IH) || (w == IH && h == IW))) return;
    memcpy(exact_trace.input, payload, (size_t)w * h);
    exact_trace.w = w; exact_trace.h = h; exact_trace.mode = m;
    exact_trace.force = force; exact_trace.update = updates + 1;
    snprintf(exact_trace.command, sizeof exact_trace.command, "%s", command);
    for (size_t i=0; exact_trace.command[i]; i++)
        if ((unsigned char)exact_trace.command[i] < 32 ||
            (unsigned char)exact_trace.command[i] > 126) exact_trace.command[i] = '?';
    exact_trace.input_ms = trace_boot_ms();
    exact_trace.state = TRACE_CAPTURED;
}
/* Inverse of img_from_grey/img_set: restore the original logical orientation.
 * This is the actual packed image, before stock ModeDecision may modify it. */
static void trace_post(void) {
    if (exact_trace.state != TRACE_CAPTURED) return;
    int w=exact_trace.w, h=exact_trace.h;
    const uint8_t *packed = img.data;
    for (int y=0; y<h; y++) for (int x=0; x<w; x++) {
        int xx=rot180 ? w-1-x : x, yy=rot180 ? h-1-y : y;
        size_t index = w == IW ? (size_t)yy * IW + IW-1-xx
                              : (size_t)xx * IW + yy;
        exact_trace.post[(size_t)y*w+x] = packed[4*index];
    }
    exact_trace.post_ms = trace_boot_ms();
}
static int trace_write_all(int fd, const void *data, size_t bytes) {
    const uint8_t *p=data;
    while (bytes) {
        ssize_t n=write(fd, p, bytes);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p+=n; bytes-=(size_t)n;
    }
    return 0;
}
static void trace_finish(int rc, const char *reply, int physical_attempted) {
    if (exact_trace.state != TRACE_CAPTURED) return;
    double completed_ms=trace_boot_ms(), write_start=now();
    const char *names[]={"trace-input.pgm", "trace-post.pgm", "trace-meta.txt"};
    int fds[]={-1,-1,-1}, created[]={0,0,0}, ok=exact_trace.post_ms > 0;
    char paths[3][sizeof(A6L_TRACE_DIR)+40];
    for (int i=0; i<3; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/%s", A6L_TRACE_DIR, names[i]);
        if (!ok) continue;
        fds[i]=open(paths[i], O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fds[i] < 0) ok=0; else created[i]=1;
    }
    char header[64]; int hn=snprintf(header,sizeof header,"P5\n%d %d\n255\n",exact_trace.w,exact_trace.h);
    size_t bytes=(size_t)exact_trace.w*exact_trace.h;
    if (ok && (trace_write_all(fds[0],header,(size_t)hn) ||
               trace_write_all(fds[0],exact_trace.input,bytes) ||
               trace_write_all(fds[1],header,(size_t)hn) ||
               trace_write_all(fds[1],exact_trace.post,bytes))) ok=0;
    char metadata[2048];
    int mn=snprintf(metadata,sizeof metadata,
        "version=1\ntoken=%s\ncommand=%s\nwidth=%d\nheight=%d\nmode=%d\nforce=%d\n"
        "update=%d\nframes=%d\ndecision=%d\ntemperature_c=%d\nrot180=%d\ndither=%d\n"
        "input_boot_ms=%.3f\npost_boot_ms=%.3f\ncompleted_boot_ms=%.3f\n"
        "physical_attempted=%d\nupdate_rc=%d\nphysical_ok=%d\nreply_generated=%s\n"
        "mirror_reply_observed=unknown_use_matching_update_log\n"
        "post_stage=after_dither_pack_before_stock_mode_decision\n"
        "write_ms=%.3f\ndump_complete=1\n",
        exact_trace.token,exact_trace.command,exact_trace.w,exact_trace.h,
        exact_trace.mode,exact_trace.force,exact_trace.update,exact_trace.frames,
        exact_trace.decision,exact_trace.temperature,rot180,dither,
        exact_trace.input_ms,exact_trace.post_ms,completed_ms,physical_attempted,
        rc,physical_attempted && !rc,reply,(now()-write_start)*1000);
    if (ok && (mn < 0 || (size_t)mn >= sizeof metadata || trace_write_all(fds[2],metadata,(size_t)mn))) ok=0;
    for (int i=0; i<3; i++) if (fds[i]>=0 && close(fds[i])) ok=0;
    if (!ok) for (int i=0; i<3; i++) if (created[i]) unlink(paths[i]);
    LOG("exact trace %s: %s update=%d physical=%d rc=%d write=%.0f ms",
        exact_trace.token,ok ? "saved" : "dump failed (no overwrite)",
        exact_trace.update,physical_attempted,rc,(now()-write_start)*1000);
    free(exact_trace.input); free(exact_trace.post);
    exact_trace.input=exact_trace.post=NULL;
    exact_trace.state=TRACE_DONE; /* One attempt per process, including IO failure. */
}

/* eink-round4: "input 720x1440 prepared in 52-61 ms" (6 Oct logs) is on the critical path of every update. For the
 * position-stable dithers (ordered, none) the same bytes are produced without the int16 copy: one table lookup per pixel
 * (ordered16 for every 8x8 phase and grey) and one 32-bit store into the transposed library layout, in row blocks that
 * keep the strided source reads in cache. Floyd-Steinberg keeps img_from_grey(). Byte-identical (host-tested). */
static uint8_t ord_lut[64][256]; static int ord_lut_ready;
static void img_from_u8(const uint8_t *px, int w, int h) {
    double t0 = now();
    if (!ord_lut_ready) { for (int p = 0; p < 64; p++) for (int g = 0; g < 256; g++) ord_lut[p][g] = (uint8_t)ordered16(g, (unsigned)(p & 7), (unsigned)(p >> 3)); ord_lut_ready = 1; }
    uint32_t *dst = img.data; enum { RB = 32 };
    for (int y0 = 0; y0 < h; y0 += RB) {
        int y1 = y0 + RB < h ? y0 + RB : h;
        for (int x = 0; x < w; x++) {
            int xx = rot180 ? w - 1 - x : x;
            for (int y = y0; y < y1; y++) {
                uint8_t v = px[(size_t)y * w + x];
                if (dither == 2) v = ord_lut[((y & 7) << 3) | (x & 7)][v];
                int yy = rot180 ? h - 1 - y : y;
                size_t i = w == IW ? (size_t)yy * IW + IW - 1 - xx : (size_t)xx * IW + yy;	/* = img_set() */
                dst[i] = 0xff000000u | (uint32_t)v << 16 | (uint32_t)v << 8 | v;	/* little endian: bytes v v v ff */
            }
        }
    }
    input_dither_ms = 0; input_pack_ms = (now() - t0) * 1000;
}
static int load_grey(const uint8_t *px, int w, int h) {	/* v4 "frame" command */
    double t0 = now();
    if (!size_ok(w, h)) { LOG("FAIL frame %dx%d: need 1440x720 or 720x1440", w, h); return -1; }
    if (dither != 1) {
        img_from_u8(px, w, h);
        LOG("input %dx%d prepared in %.0f ms (table dither + direct pack)", w, h, (now() - t0) * 1000);
        return 0;
    }
    int16_t *g = malloc((size_t)w * h * sizeof *g); if (!g) return -1;
    for (size_t i = 0; i < (size_t)w * h; i++) g[i] = px[i];
    img_from_grey(g, w, h); free(g);
    LOG("input %dx%d prepared in %.0f ms: dither %.0f ms, pack %.0f ms", w, h, (now() - t0) * 1000, input_dither_ms, input_pack_ms);
    return 0;
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
#ifdef A6L_EPDD_HOSTTEST	/* tests/test_epdd_drive.c: fake rail switch and page flips (no DRM, no sysfs) */
static int a6l_test_power(int on);
static int a6l_test_flip(uint32_t id);
static int a6l_test_modeset(void);
#endif
static int power(int on) {
#ifdef A6L_EPDD_HOSTTEST
    return a6l_test_power(on);
#endif
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
/* eink-round4: kernel/power/wakelock.c pm_wake_lock()/pm_wake_unlock() return -EPERM without CAP_BLOCK_SUSPEND. The
 * service ran as "user system" without that capability, so every lock write failed (open() succeeds: the file is
 * radio:wakelock 0660) and the failure was logged once at the first update after boot only. 6 Oct 17:01:56: Android
 * suspended 0.48 s into update 86 (s2idle entry while the rails were on), the panel suspend cut the rails mid-waveform,
 * the kernel resume re-committed the enabled lessee CRTC and the next LCD switch faulted in the MDP SMMU again (round 3's
 * trigger, which 0013's a6l_epdd_crtc lock was meant to remove). The rc now grants BLOCK_SUSPEND; every failed lock write
 * is logged (rate-limited) and reported by "status" (kernel_wakelock=ok|<errno>). Paths are variables for the host test. */
static const char *wl_lock_path = "/sys/power/wake_lock", *wl_unlock_path = "/sys/power/wake_unlock";
static int wl_err;		/* errno of the last failed lock write, 0 = the last lock write succeeded */
static int wl_write(int lock, const char *what) {
    int f = open(lock ? wl_lock_path : wl_unlock_path, O_WRONLY | O_CLOEXEC), e = 0;
    if (f < 0) e = errno ? errno : EIO;
    else { size_t n = strlen(what); if (write(f, what, n) != (ssize_t)n) e = errno ? errno : EIO; close(f); }
    if (!lock) return e;	/* unlock of a lock that is not held = EINVAL: not an error */
    if (e) {
        static double last; static int last_e;
        if (e != last_e || now() - last > 60) {
            LOG("FAIL wakelock '%s' not taken: %s%s - the system can suspend in the middle of an e-ink update", what, strerror(e),
                e == EPERM ? " (needs CAP_BLOCK_SUSPEND: rc 'capabilities BLOCK_SUSPEND')" : "");
            last = now(); last_e = e;
        }
    } else if (wl_err) LOG("wakelock '%s' taken again (kernel wakelock ok)", what);
    wl_err = e;
    return e;
}
static void wakelock(int on) {
    if (!use_wakelock || dry) return;
    wl_write(on, "a6l_epdd");
}
/* eink-round4: CLOCK_BOOTTIME - CLOCK_MONOTONIC grows by the time spent in system suspend (s2idle included). A drive
 * that straddles a suspend has lost its rails (panel suspend) and its frame timing: the picture is unknown. */
#ifdef A6L_EPDD_HOSTTEST
static double (*a6l_test_suspended_hook)(void);	/* set by tests/test_epdd_drive.c; other host tests: never suspended */
static double suspended_s(void) { return a6l_test_suspended_hook ? a6l_test_suspended_hook() : 0; }
#else
static double suspended_s(void) {
    struct timespec b, m;
    if (clock_gettime(CLOCK_BOOTTIME, &b) || clock_gettime(CLOCK_MONOTONIC, &m)) return 0;
    return (b.tv_sec - m.tv_sec) + (b.tv_nsec - m.tv_nsec) / 1e9;
}
#endif
/* eink-round3: the e-ink CRTC (a lessee CRTC) must never be enabled across a system suspend. The kernel's
 * drm_mode_config_helper_suspend/resume saves and re-commits it behind the composer's and our back (V73 bring-up runs
 * inside the resume path), and the only filmed LCD scan-out corruption (6 Oct 16:07:34, ~90 s of MDP SMMU faults) started
 * at the first LCD switch after exactly that sequence. Held from the modeset to crtc_off (power off / idle-off / exit).
 * Timed (idle-off + 15 s, refreshed per update) so a crashed daemon cannot block suspend for ever; untimed with
 * --idle-off 0 because the CRTC then stays on until "power off". --crtc-wakelock 0 restores the old behaviour. */
static int crtc_wakelock_on = 1, crtc_wl_held, idle_off_s;
static void crtc_wakelock(int on) {
    if (!crtc_wakelock_on || dry) return;
    if (!on && !crtc_wl_held) return;
    crtc_wl_held = on;
    if (!use_wakelock) return;
    char b[64];
    if (on && idle_off_s > 0) snprintf(b, sizeof b, "a6l_epdd_crtc %lld", (long long)(idle_off_s + 15) * 1000000000LL);
    else snprintf(b, sizeof b, "a6l_epdd_crtc");
    wl_write(on, b);	/* eink-round4: failures logged, see wl_write() */
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
#ifdef A6L_EPDD_HOSTTEST
    return a6l_test_flip(id);
#endif
    drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
    /* r5 bug hunt eink-display E1: a flip whose event missed the 1 s wait is still queued in the kernel. Its late event
     * must not be taken for the completion of this flip (it would return one vblank early, and the next flip would fail
     * with EBUSY, for ever). Wait for it first; if it still does not come, ask for a DRM reset (drm_lost: run_update
     * closes the fd, which discards the stale event, and re-acquires the lease). */
    while (pending) { struct pollfd p = {.fd = dfd, .events = POLLIN};
        if (poll(&p, 1, 1000) <= 0) { LOG("FAIL page flip of an earlier update still not completed: resetting DRM"); drm_lost = 1; return -1; }
        drmHandleEvent(dfd, &ev); }
    if (drmModePageFlip(dfd, crtc, id, DRM_MODE_PAGE_FLIP_EVENT, 0)) { int e = errno; LOG("FAIL pageflip: %s", strerror(e)); if (lost_errno(e)) drm_lost = 1; return -1; }
    pending = 1;
    while (pending) { struct pollfd p = {.fd = dfd, .events = POLLIN}; if (poll(&p, 1, 1000) <= 0) { LOG("FAIL vblank timeout (flip still pending)"); return -1; } drmHandleEvent(dfd, &ev); }
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
static int rails_release(const char *why);
static void drm_close(void) {	/* v4: lease lost / composer restarted: drop everything, the next update reopens */
    kmark("DRM reset (lease lost / composer restarted?)");
    rails_release("DRM reset");
    /* eink-round3: closing the lessee fd removes our fbs but does not disable the CRTC; switch it off while we still can
     * (fails harmlessly when the lease is already revoked), then drop the no-suspend lock with it. */
    if (dfd >= 0 && crtc && started && !dry) drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL);
    crtc_wakelock(0);
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
#ifdef A6L_EPDD_HOSTTEST
    return a6l_test_modeset();
#endif
    char st[200];
    for (int attempt = 1; attempt <= 3; attempt++) {
        kmark("e-ink CRTC %u modeset on (attempt %d)", crtc, attempt);
        if (drmModeSetCrtc(dfd, crtc, idle.id, 0, 0, &conn_id, 1, &mode_info)) { int e = errno; KLOG("FAIL setcrtc: %s", strerror(e)); if (lost_errno(e)) drm_lost = 1; return -1; }
        for (int i = 0; i < 10; i++) flip(idle.id);
        bringup_status(st, sizeof st); KLOG("bring-up %d: %s", attempt, st[0] ? st : "(no status attribute)");
        if (!st[0] || strstr(st, "bridge_ok=1")) return 0;
        drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); sleep(1);
    }
    return -1;
}
static int rails_release(const char *why);
static void crtc_off(const char *why) {
    rails_release(why);	/* eink-round4: never leave the rails up behind a switched-off CRTC */
    if (dry || dfd < 0 || !crtc || !started) return;
    kmark("e-ink CRTC %u off begin (%s)", crtc, why);
    drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); started = 0; KLOG("e-ink CRTC off (%s)", why);
    crtc_wakelock(0);
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
/* r5 review fix F35: panel_unknown = the picture on the panel is not the one the library state assumes (a failed or
 * partial update, a rail failure). The library already advanced its differential state, so the next picture is only
 * drawn after a full recovery clear (white GC16 + INIT waveform, clear_kind("full")); rails_off_fail counts failed
 * rail switch-offs (reported by "status"). Only the error handling changed: rail values/sequencing are untouched. */
static int panel_unknown, rails_off_fail;
/* eink-round2 (EXPERIMENTAL, --overlap-gen 1, default 0): the library generates the waveform frames in a worker thread
 * while the main thread does the modeset, the rail switch-on and the lead idle scans (~170 ms that were serial after a
 * 300-420 ms generation). The waveform scanout starts only once ALL frames exist, exactly as before: if generation is
 * still running after the lead scans, more idle frames are scanned (idle = no drive). Rail switch-on is delayed by the
 * previous update's generation time so the rails are not on longer than today. Library calls stay on one thread. */
static int overlap_gen, last_frames;
static double last_gen_ms = -1, last_rails_on_ms = 60;
struct gen_job { pthread_mutex_t mu; pthread_cond_t cv; int done, n, nf, t, force, m; double t0, ms; };
static int generate(int t, int force, int m, int *nf_out) {	/* decision + all frames of one update (library state) */
    int nf = tc_decide(&img, handle, t, t, force, m), n = 0; uint8_t more = 1;
    while (more && n < MAXF) { struct buf *b = &ring[n % RING]; more = tc_update(b, handle);
        if (!frames[n] && !(frames[n] = malloc(FRAME))) { LOG("FAIL out of memory"); return -1; }
        memcpy(frames[n], b->data, FRAME); n++; }
    *nf_out = nf; return n;
}
static void *gen_main(void *arg) {
    struct gen_job *j = arg; int nf = 0;
    int n = generate(j->t, j->force, j->m, &nf);
    pthread_mutex_lock(&j->mu); j->n = n; j->nf = nf; j->ms = (now() - j->t0) * 1000; j->done = 1;
    pthread_cond_broadcast(&j->cv); pthread_mutex_unlock(&j->mu);
    return NULL;
}
static int job_done(struct gen_job *j) { pthread_mutex_lock(&j->mu); int d = j->done; pthread_mutex_unlock(&j->mu); return d; }
static void job_wait_until(struct gen_job *j, double t_end) {	/* wait for the job or the deadline, whichever first */
    pthread_mutex_lock(&j->mu);
    while (!j->done && now() < t_end) {
        double r = t_end - now(); struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        long ns = ts.tv_nsec + (long)((r > 0.05 ? 0.05 : r) * 1e9); ts.tv_sec += ns / 1000000000L; ts.tv_nsec = ns % 1000000000L;
        pthread_cond_timedwait(&j->cv, &j->mu, &ts);
    }
    pthread_mutex_unlock(&j->mu);
}
/* eink-round4 (--chain 1, rc default 1): one update every ~0.8 s instead of ~1.5 s while the page keeps changing.
 * Stock keeps the rails up during a burst and starts the next waveform ~0.09 s after the previous one (round 1/2 films).
 * After the last waveform frame the scanout is switched to the idle (no-drive) frame and the reply is sent at once: the
 * picture on the panel is final, the tail scans only kept the rails up over idle frames. The rails then stay up for the
 * tail time (tail / 85 Hz, serve() switches them off) and, if the next frame command arrives within it, its waveform
 * starts without rails-off, rails-on and lead scans (the CRTC kept scanning the idle frame, so the rails had settled).
 * Rails-on time per cycle is no longer than lead + tail today; the "a6l_epdd" wakelock is held while they are up.
 * Every other command, power off, idle-off, a lost lease and exit switch the rails off first. */
static int chain, rails_held; static double rails_hold_until;
static int rails_release(const char *why) {
    if (!rails_held) return 0;
    rails_held = 0;
    int rc = 0; double t0 = now();
    if (power(0) && power(0)) { rails_off_fail++; panel_unknown = 1; rc = -1; LOG("FAIL rails not switched off after update %d (%s): panel state unknown", updates, why); }
    else LOG("rails off after update %d (%s) in %.0f ms", updates, why, (now() - t0) * 1000);
    wakelock(0);
    return rc;
}
static int drive_job(int n, struct gen_job *job);
static int drive(int n) { return drive_job(n, NULL); }
static int drive_job(int n, struct gen_job *job) {	/* scan out the n frames of the current update; rails only around the drive (v2) */
    double tcold = now();
    int warm = rails_held && started;	/* eink-round4: chained update, rails still up over idle frames */
    if (rails_held && !started) rails_release("CRTC was off");
    rails_held = 0;
    /* modeset with the real strobe pattern, as a6l_epd_play did; eink-round3: no suspend while the e-ink CRTC is on */
    if (!started) { crtc_wakelock(1); if (drm_start()) { crtc_wakelock(0); return -1; } started = 1; }
    else crtc_wakelock(1);	/* refresh the timed lock: idle-off counts from this update */
    double cold_ms = (now() - tcold) * 1000;
    if (job && last_gen_ms > 0 && !warm) {	/* rails on only so early that rails-on + lead end when generation is expected to end */
        double lead_s = lead / 85.0, budget = last_gen_ms / 1000 - last_rails_on_ms / 1000 - lead_s;
        if (budget > 0) job_wait_until(job, job->t0 + budget);
    }
    unsigned g0 = gaps; int rc = 0; double t1 = now(), susp0 = suspended_s();
    if (!warm && power(1)) {	/* F35: no waveform scanout without the rails; switch off whatever may have come up */
        LOG("FAIL rails not switched on: update %d not driven", updates);
        if (power(0)) { rails_off_fail++; LOG("FAIL rails not switched off after the failed switch-on"); }
        last_ms = (int)((now() - t1) * 1000);
        return -1;
    }
    double rails_on_ms = (now() - t1) * 1000, tp = now();
    if (!warm) last_rails_on_ms = rails_on_ms;
    for (int i = 0; i < lead && !rc && !warm; i++) rc = flip(idle.id);
    int extra_idle = 0;
    if (job) {	/* all frames must exist before the first waveform frame; idle scans meanwhile drive nothing */
        while (!rc && !job_done(job)) { rc = flip(idle.id); extra_idle++; }
        n = job->n;
        if (!rc && n <= 0) { LOG("FAIL update %d: frame generation failed", updates); rc = -1; }
    }
    double lead_ms = (now() - tp) * 1000; tp = now();
    unsigned gd = gaps;
    for (int k = 0; k < n && !rc; k++) { struct fb *f = (k & 1) ? &fb2 : &fa; memcpy(f->map, frames[k], FRAME); rc = flip(f->id); }
    double wave_ms = (now() - tp) * 1000; tp = now();
    gd = gaps - gd;
    double lost = suspended_s() - susp0;	/* eink-round4: a suspend cut the rails (panel suspend) mid-waveform */
    int hold = chain && !rc && lost <= 0.05 && tail > 1;
    if (hold) {	/* eink-round4 chain: idle frame now, reply now, rails off by serve() after the tail time unless chained */
        rc = flip(idle.id);
        if (!rc) { rails_held = 1; rails_hold_until = now() + (tail - 1) / 85.0; }
        else hold = 0;
    }
    for (int i = 0; i < tail && !rc && !hold; i++) rc = flip(idle.id);
    double tail_ms = (now() - tp) * 1000; tp = now();
    if (!rails_held && power(0) && power(0)) {	/* F35: every exit switches the rails off; a failure (after one retry) is an error */
        rails_off_fail++; LOG("FAIL rails not switched off after update %d", updates); rc = -1;
    }
    if (lost > 0.05) { LOG("FAIL system suspended for %.1f s during update %d: waveform interrupted, panel state unknown", lost, updates); rc = -1; }
    last_ms = (int)((now() - t1) * 1000);
    LOG("update %d stages mono_ms=%.3f cold=%.0f rails_on=%.0f lead=%.0f waveform=%.0f tail=%.0f rails_off=%.0f%s%s%s", updates, now() * 1000, cold_ms, rails_on_ms, lead_ms, wave_ms, tail_ms, (now() - tp) * 1000, job ? " (overlapped generation)" : "",
        warm ? " (chained: rails were up)" : "", rails_held ? " (rails held for the next update)" : "");
    if (job) LOG("update %d overlap: %d extra idle scans while generating", updates, extra_idle);
    LOG("update %d shown in %d ms: %s, missed vblanks during drive=%u (total %u)", updates, last_ms, rc ? "FAILED" : "ok", gd, gaps - g0);
    return rc;
}
static int run_update(int force, int m, const char *what) {
    double tt = now(); int t = temperature(); double t0 = now();
    LOG("temperature stage mono_ms=%.3f read=%.0f ms value=%dC", t0 * 1000, (t0 - tt) * 1000, t);
    if (overlap_gen && !dry && !lib_only && idle_pattern) {
        static struct gen_job job;
        memset(&job, 0, sizeof job); pthread_mutex_init(&job.mu, NULL); pthread_cond_init(&job.cv, NULL);
        job.t = t; job.force = force; job.m = m; job.t0 = now(); job.n = -1;
        pthread_t th;
        if (!pthread_create(&th, NULL, gen_main, &job)) {
            updates++;
            int rc = -1;
            wakelock(1);
            for (int attempt = 0; attempt < 2 && rc; attempt++) {
                if (dfd < 0 && drm_open()) break;
                if (idle.map && !started) memcpy(idle.map, idle_pattern, FRAME);
                if (exact_trace.state == TRACE_CAPTURED && exact_trace.update == updates) exact_trace.drive_attempted=1;
                rc = drive_job(-1, &job);
                if (rc && drm_lost) { LOG("WARN DRM access lost (lease revoked / composer restarted?): re-acquiring and driving again"); drm_close(); }
                else break;
            }
            pthread_join(th, NULL);	/* also after an early drive failure: the library state must be quiescent */
            if (!rails_held) wakelock(0);	/* eink-round4: held while the rails stay up (chain) */
            if (job.n < 0) rc = -1;
            last_frames = job.n; last_gen_ms = job.ms;
            if (exact_trace.state == TRACE_CAPTURED && exact_trace.update == updates) { exact_trace.frames=job.n; exact_trace.decision=job.nf; exact_trace.temperature=t; }
            LOG("update %d (%s): mode=%d force=%d temp=%dC decision=%d frames=%d generated in %.0f ms (overlapped)", updates, what, m, force, t, job.nf, job.n, job.ms);
            pthread_mutex_destroy(&job.mu); pthread_cond_destroy(&job.cv);
            last_update_t = now(); if (rc) { fails_total++; panel_unknown = 1; }
            return rc;
        }
        LOG("WARN generation thread not started: serial update");
        pthread_mutex_destroy(&job.mu); pthread_cond_destroy(&job.cv);
    }
    int nf = 0, n = generate(t, force, m, &nf);
    if (n < 0) return -1;
    last_frames = n; last_gen_ms = (now() - t0) * 1000;
    updates++; if (exact_trace.state == TRACE_CAPTURED && exact_trace.update == updates) { exact_trace.frames=n; exact_trace.decision=nf; exact_trace.temperature=t; } LOG("update %d (%s): mode=%d force=%d temp=%dC decision=%d frames=%d generated in %.0f ms", updates, what, m, force, t, nf, n, (now() - t0) * 1000);
    if (dry) { write_a6lepd(n); return 0; }
    if (lib_only) return 0;
    if (!idle_pattern) { if (!(idle_pattern = malloc(FRAME))) return -1; for (unsigned i = 0; i < FRAME / 4; i++) idle_pattern[i] = frames[0][i] & 0xffffff00u; }
    int rc = -1;
    wakelock(1);
    for (int attempt = 0; attempt < 2 && rc; attempt++) {
        if (dfd < 0 && drm_open()) break;
        if (idle.map && !started) memcpy(idle.map, idle_pattern, FRAME);
        if (exact_trace.state == TRACE_CAPTURED && exact_trace.update == updates) exact_trace.drive_attempted=1;
        rc = drive(n);
        if (rc && drm_lost) { LOG("WARN DRM access lost (lease revoked / composer restarted?): re-acquiring and driving again"); drm_close(); }
        else break;
    }
    if (!rails_held) wakelock(0);	/* eink-round4: held while the rails stay up (chain) */
    last_update_t = now(); if (rc) { fails_total++; panel_unknown = 1; }
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
static int clear_kind2(const char *k) {
    if (!k || !*k || !strcmp(k, "full")) { int rc = clear_white_gc("clear-white"); return rc ? rc : clear_init("clear-init"); }
    if (!strcmp(k, "stock")) { int rc = clear_init("clear-init"); return rc ? rc : clear_white_gc("clear-white"); }
    if (!strcmp(k, "init")) return clear_init("clear-init");
    if (!strcmp(k, "gc")) return clear_white_gc("clear-gc");
    LOG("unknown clear kind '%s'", k); return -1;
}
static int clear_kind(const char *k) {	/* F35: a successful INIT-based clear re-establishes a known (white) panel */
    int rc = clear_kind2(k);
    if (!rc && (!k || strcmp(k, "gc"))) { if (panel_unknown) LOG("panel state known again (clear %s)", k && *k ? k : "full"); panel_unknown = 0; }
    return rc;
}
/* F35: before drawing a picture on a panel in unknown state, run the full recovery clear; -1 = still unknown */
static int recover(void) {
    if (!panel_unknown || dry) return 0;
    LOG("panel state unknown after a failed update: full recovery clear before the next picture");
    if (clear_kind("full")) { LOG("FAIL recovery clear failed: panel state still unknown"); return -1; }
    return 0;
}
static int clear_full(void) { return clear_kind("full"); }
static int refresh(void) {	/* stock epd_force_clear: forced GC16 redraw of the current picture */
    if (recover()) return -1;
    if (!have_last) return clear_white_gc("refresh-white");
    memcpy(img.data, last_img, RGBA); return run_update(1, last_mode, "refresh");
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
    memcpy(last_img, img.data, RGBA); have_last = 1; last_mode = m; return run_update(0, m, what);
}
static int show(const char *path, int m) {
    if (clear_every > 0 && ++since_clear >= clear_every) { since_clear = 0; clear_full(); }
    if (recover()) return -1;
    if (load_pnm(path)) return -1; return show_loaded(m, path);
}
/* eink-lockscreen: lock pictures (a6l_einklock) and the restore of the picture they covered. lock_on_panel = a lock picture
 * was the last one driven; mirror_img/mirror_mode = the picture (library layout) the panel showed before the first one. */
static uint8_t *mirror_img; static int mirror_mode = 3, have_mirror, lock_on_panel, lock_frames;
static int lock_frame(const uint8_t *payload, int w, int h, int m, int force) {
    if (!size_ok(w, h)) { LOG("FAIL lockframe %dx%d: need 1440x720 or 720x1440", w, h); return -1; }
    if (!lock_on_panel) {
        if (have_last && (mirror_img || (mirror_img = malloc(RGBA)))) { memcpy(mirror_img, last_img, RGBA); mirror_mode = last_mode; have_mirror = 1; }
        else have_mirror = 0;
        LOG("lock screen: first lock picture (%s)", have_mirror ? "the current picture is kept for the restore" : "no earlier picture to keep");
    }
    kmark("lockframe begin (crtc %s, rails %s)", started ? "on" : "off", rails_held ? "held" : "off");
    int rc = recover();
    if (!rc) rc = load_grey(payload, w, h);
    if (!rc) {
        memcpy(last_img, img.data, RGBA); have_last = 1; last_mode = m; lock_on_panel = 1; lock_frames++;
        rc = run_update(force, m, force ? "lock-clean" : "lock");
    }
    crtc_off("lock picture shown: the system may suspend until the next tick");	/* also after a failure */
    kmark("lockframe end: %s", rc ? "FAILED" : "ok");
    return rc;
}
static int lock_restore(int off, char *reply, size_t rn) {
    int rc = 0;
    kmark("lock restore%s (lock picture %s)", off ? " off" : "", lock_on_panel ? "shown" : "already replaced");
    if (!lock_on_panel) snprintf(reply, rn, "OK lock restore: nothing to do");
    else if (!have_mirror) { lock_on_panel = 0; snprintf(reply, rn, "OK lock restore: no earlier picture, lock picture kept"); }
    else {
        lock_on_panel = 0; rc = recover();
        if (!rc) { memcpy(img.data, mirror_img, RGBA); memcpy(last_img, mirror_img, RGBA); have_last = 1; last_mode = mirror_mode; rc = run_update(0, mirror_mode, "lock-restore"); }
        if (rc) snprintf(reply, rn, "ERR lock restore failed (see log)");
        else snprintf(reply, rn, "OK lock restore: earlier picture redrawn in %d ms (update %d)", last_ms, updates);
    }
    if (off) crtc_off("lock restore: not awake on the e-ink");
    return rc;
}

/* ---------------- commands (v4: one reply per command) ---------------- */
static volatile sig_atomic_t stop;
static void on_sig(int s) { (void)s; stop = 1; }
static double t_boot;
/* returns 0/-1; reply gets "OK ..." or "ERR ..." (without newline). For "frame", *need = bytes of pixels to read first. */
static int exec_cmd(const char *line, const uint8_t *payload, char *reply, size_t rn);
static int exec_cmd(const char *line, const uint8_t *payload, char *reply, size_t rn) {
    char path[512], mname[32] = "", flag[16] = ""; int m, rc = 0, w = 0, h = 0;
    if (!strncmp(line, "trace arm ", 10)) { rc=trace_arm(line+10); snprintf(reply,rn,"%s trace arm",rc ? "ERR" : "OK"); return rc; }
    if (!strcmp(line, "trace cancel")) { rc=trace_cancel(); snprintf(reply,rn,"%s trace cancel",rc ? "ERR" : "OK"); return rc; }
    if (!strcmp(line, "trace status")) { snprintf(reply,rn,"OK trace state=%d token=%s",exact_trace.state,exact_trace.token); return 0; }
    if (!strncmp(line, "dither ", 7)) {
        int selected = dither_parse(line + 7);
        if (selected < 0) { snprintf(reply,rn,"ERR dither requires fs|ordered|none"); return -1; }
        /* Single-threaded command dispatch: no current image/history rewrite.
         * Only future load_grey/load_pnm conversion uses the selected algorithm. */
        dither = selected;
        snprintf(reply,rn,"OK dither=%s next-frame-only",dither_name(dither));
        return 0;
    }
    if (!strcmp(line, "ping")) { snprintf(reply, rn, "OK pong"); return 0; }
    if (!strcmp(line, "quit")) { stop = 1; snprintf(reply, rn, "OK quitting"); return 0; }
    if (!strcmp(line, "status")) {
        char st[200]; bringup_status(st, sizeof st);
        snprintf(reply, rn, "OK updates=%d fails=%d rails_off_fail=%d panel=%s last_ms=%d rails=%s crtc=%s crtc_wakelock=%s kernel_wakelock=%s drm=%s mode=%d idle_s=%.0f uptime_s=%.0f lock=%s lock_frames=%d bringup=%s", updates, fails_total, rails_off_fail, panel_unknown ? "unknown" : "known", last_ms, rails_held ? "held" : "off",
                 started ? "on" : "off", crtc_wl_held ? "held" : "free", !use_wakelock || dry ? "off" : wl_err ? strerror(wl_err) : "ok", dfd >= 0 ? (no_master ? "lessee" : "open") : "closed", mode, last_update_t ? now() - last_update_t : -1, now() - t_boot,
                 lock_on_panel ? "on" : "off", lock_frames, st[0] ? st : "-");
        return 0;
    }
    if (!strcmp(line, "power off")) { if (started) kmark("power off requested (mirror: LCD off / mirror off)"); crtc_off("requested"); snprintf(reply, rn, "OK crtc off"); return 0; }
    if (!strcmp(line, "lock restore") || !strcmp(line, "lock restore off")) return lock_restore(line[12] != 0, reply, rn);
    if (sscanf(line, "lockframe %d %d %31s %15s", &w, &h, mname, flag) >= 2) {
        if (!payload) { snprintf(reply, rn, "ERR lockframe needs a pixel payload (socket only)"); return -1; }
        if (!strcmp(mname, "force")) { snprintf(mname, sizeof mname, "reading"); snprintf(flag, sizeof flag, "force"); }
        if (flag[0] && strcmp(flag, "force")) { snprintf(reply, rn, "ERR unknown lockframe flag"); return -1; }
        rc = lock_frame(payload, w, h, mname[0] ? mode_by_name(mname, 3) : 3, flag[0] != 0);
        if (rc) snprintf(reply, rn, "ERR lock picture failed (see log)");
        else snprintf(reply, rn, "OK lock picture shown in %d ms (update %d), crtc off", last_ms, updates);
        return rc;
    }
    if (!strncmp(line, "mode ", 5)) { mode = mode_by_name(line + 5, mode); snprintf(reply, rn, "OK mode=%d", mode); return 0; }
    /* eink-lockscreen: any other picture replaces a lock picture ("refresh" redraws the current one and keeps the state) */
    if (!strcmp(line, "clear")) { lock_on_panel = 0; rc = clear_full(); }
    else if (!strncmp(line, "clear ", 6)) { lock_on_panel = 0; rc = clear_kind(line + 6); }
    else if (!strcmp(line, "refresh")) rc = refresh();
    else if (!strncmp(line, "sleep ", 6)) { rails_release("sleep command"); sleep((unsigned)atoi(line + 6)); rc = 0; }
    else if (sscanf(line, "frame %d %d %31s %15s", &w, &h, mname, flag) >= 2) {
        if (!payload) { snprintf(reply, rn, "ERR frame needs a pixel payload (socket only)"); return -1; }
        if (flag[0] && strcmp(flag, "force")) { snprintf(reply, rn, "ERR unknown frame flag"); return -1; }
        int clean = !strcmp(mname, "clean") || !strcmp(flag, "force");
        m = !strcmp(mname, "clean") ? 2 : mode_by_name(mname, mode);
        if (clear_every > 0 && ++since_clear >= clear_every) { since_clear = 0; clear_full(); }
        rc = recover();
        trace_poll();
        if (!rc && size_ok(w,h)) trace_capture(payload,w,h,m,clean,line);
        if (!rc) rc = load_grey(payload, w, h);
        if (!rc) { trace_post(); lock_on_panel = 0; }
        if (!rc && clean) {
            /* Stock ghost clearing forces GC16 on the actual page. Reserve
             * INIT/white sequences for startup, manual clear and recovery. */
            memcpy(last_img, img.data, RGBA); have_last = 1; last_mode = m;
            rc = run_update(1, m, "frame-clean");
        } else if (!rc) rc = show_loaded(m, "frame");
    }
    else if (sscanf(line, "frametest %511s %31s %15s", path, mname, flag) >= 1) {	/* test hook: a P5 file through the "frame" path */
        FILE *f = fopen(path, "rb"); char mg[3] = {0}; int ok = f && fread(mg, 1, 2, f) == 2 && mg[0] == 'P' && mg[1] == '5';
        int fw = ok ? tok(f) : -1, fh = ok ? tok(f) : -1, mx = ok ? tok(f) : -1; uint8_t *px = NULL;
        ok = ok && mx == 255 && size_ok(fw, fh) && (px = malloc((size_t)fw * fh)) && fread(px, 1, (size_t)fw * fh, f) == (size_t)fw * fh;
        if (f) fclose(f);
        if (!ok) { free(px); snprintf(reply, rn, "ERR frametest %s: need P5 720x1440/1440x720", path); return -1; }
        char hdr[96]; snprintf(hdr, sizeof hdr, "frame %d %d %s%s%s", fw, fh, mname[0] ? mname : "", flag[0] ? " " : "", flag);
        rc = exec_cmd(hdr, px, reply, rn); free(px); return rc;
    }
    else if (sscanf(line, "show %511s %31s", path, mname) >= 1) { m = mode_by_name(mname, mode); lock_on_panel = 0; rc = show(path, m); }
    else { LOG("unknown command '%s'", line); snprintf(reply, rn, "ERR unknown command"); return -1; }
    if (rc) snprintf(reply, rn, "ERR update failed (see log)");
    else snprintf(reply, rn, "OK shown in %d ms (update %d)", last_ms, updates);
    trace_finish(rc,reply,exact_trace.drive_attempted);
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
        if (sscanf(c->buf, "frame %d %d", &w, &h) == 2 || sscanf(c->buf, "lockframe %d %d", &w, &h) == 2) {
            if (!size_ok(w, h)) { client_reply(c, "ERR frame size must be 720x1440 or 1440x720"); client_drop(c); return; }
            snprintf(c->hdr, sizeof c->hdr, "%s", c->buf); c->need = (size_t)w * h;
        } else if (c->buf[0] && c->buf[0] != '#') {
            char reply[512];
            if (!strncmp(c->buf,"dither ",7) && memchr(c->buf,0,used-1))
                snprintf(reply,sizeof reply,"ERR embedded NUL in dither command");
            else exec_cmd(c->buf, NULL, reply, sizeof reply);
            client_reply(c, reply);
        }
        memmove(c->buf, c->buf + used, c->len - used); c->len -= used;
    }
}
static void serve(int lfd) {
    for (int i = 0; i < MAXCL; i++) cl[i].fd = -1;
    while (!stop) {
        struct pollfd p[MAXCL + 1]; int n = 0; p[n++] = (struct pollfd){lfd, POLLIN, 0};
        for (int i = 0; i < MAXCL; i++) if (cl[i].fd >= 0) p[n++] = (struct pollfd){cl[i].fd, POLLIN, 0};
        trace_poll();
        int timeout = 1000;
        if (rails_held) { double ms = (rails_hold_until - now()) * 1000; timeout = ms < 0 ? 0 : ms < 1000 ? (int)ms + 1 : 1000; }
        int r = poll(p, (nfds_t)n, timeout);
        if (r < 0 && errno != EINTR) { LOG("FAIL poll: %s", strerror(errno)); break; }
        if (rails_held && now() >= rails_hold_until) rails_release("tail: no chained update");	/* eink-round4 */
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
        else if (!strcmp(a, "--rot180")) rot180 = 1; else if (!strcmp(a, "--no-dither")) dither = 0;
        else if (!strcmp(a, "--dither")) {
            int selected = v ? dither_parse(v) : -1;
            if (selected < 0) { fprintf(stderr,"--dither requires fs|ordered|none\n"); return 2; }
            dither = selected; i++;
        } else if (!strcmp(a, "--show") && v && nshow < 32) shows[nshow++] = argv[++i];
        else if (!strcmp(a, "--hold") && v) hold = atoi(argv[++i]); else if (!strcmp(a, "--fifo") && v) fifo = argv[++i]; else if (!strcmp(a, "--script") && v) script = argv[++i];
        else if (!strcmp(a, "--dry") && v) { dry = 1; dry_prefix = argv[++i]; } else if (!strcmp(a, "--power") && v) power_path = argv[++i];
        else if (!strcmp(a, "--xon-line") && v) xon_line = atoi(argv[++i]); else if (!strcmp(a, "--lead") && v) lead = atoi(argv[++i]);
        else if (!strcmp(a, "--tail") && v) tail = atoi(argv[++i]);
        else if (!strcmp(a, "--overlap-gen") && v) overlap_gen = atoi(argv[++i]) == 1;
        else if (!strcmp(a, "--chain") && v) chain = atoi(argv[++i]) == 1;
        else if (!strcmp(a, "--lease") && v) { if (nleases < 6) leases[nleases++] = v; i++; } else if (!strcmp(a, "--mode-file") && v) mode_file = argv[++i];
        else if (!strcmp(a, "--save-mode") && v) save_mode = argv[++i];
        else if (!strcmp(a, "--no-master")) no_master = 1; else if (!strcmp(a, "--wait-drm") && v) wait_drm = atoi(argv[++i]);
        else if (!strcmp(a, "--socket") && v) sock_name = argv[++i]; else if (!strcmp(a, "--listen") && v) listen_at = argv[++i];
        else if (!strcmp(a, "--idle-off") && v) idle_off_s = atoi(argv[++i]); else if (!strcmp(a, "--no-startup-clear")) startup_clear = 0;
        else if (!strcmp(a, "--wakelock")) wl = 1; else if (!strcmp(a, "--no-wakelock")) wl = 0;
        else if (!strcmp(a, "--crtc-wakelock") && v) crtc_wakelock_on = atoi(argv[++i]) != 0;
        else { fprintf(stderr, "usage: see source header (%s)\n", a); return 2; }
    }
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig); signal(SIGPIPE, SIG_IGN); setvbuf(stdout, NULL, _IOLBF, 0);
    /* eink-round2: lead/tail are now property-tunable from the rc (persist.vendor.eink.lead/tail, defaults unchanged
     * 10/20) for an ATTENDED A/B test; bound them so a typo cannot remove the idle scans or stall the drive. */
    if (lead < 2 || lead > 40) { LOG("WARN --lead %d out of range 2..40: using 10", lead); lead = 10; }
    if (tail < 4 || tail > 60) { LOG("WARN --tail %d out of range 4..60: using 20", tail); tail = 20; }
    use_wakelock = wl >= 0 ? wl : (sock_name || listen_at);
    /* eink-round3: a previous instance that died with the e-ink CRTC on may have left its no-suspend lock behind */
    if (use_wakelock && !dry) { int keep = crtc_wakelock_on; crtc_wakelock_on = 1; crtc_wl_held = 1; crtc_wakelock(0); crtc_wakelock_on = keep; }
    LOG("e-ink CRTC no-suspend lock: %s (idle-off %d s)", crtc_wakelock_on && !dry ? "on" : "off", idle_off_s);
    /* eink-round4: prove at start-up that the kernel accepts our wakelocks (EPERM without CAP_BLOCK_SUSPEND) */
    if (use_wakelock && !dry) { if (!wl_write(1, "a6l_epdd")) LOG("kernel wakelock: ok"); wl_write(0, "a6l_epdd"); }
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
    if (!dry) { power(0); if (crtc && dfd >= 0) drmModeSetCrtc(dfd, crtc, 0, 0, 0, NULL, 0, NULL); crtc_wakelock(0); }
    if (xon_fd >= 0) { struct gpio_v2_line_values v = {0, 1}; ioctl(xon_fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &v); close(xon_fd); }
    LOG("exit");
    return 0;
}
