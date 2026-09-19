/* Emulator-only exercise of the stock software-TCON call sequence recovered from
 * hwcomposer.sdm660.so (docs/eink-swtcon-abi-20260919.md):
 *   Init_Eink_SWTcon(ring[5]{buf,size}, 5, cfg{1440,720,50}, flash, 0x70080, info) -> handle
 *   ModeDecision_MirrorMode(&img{rgba,size}, handle, top_temp, bottom_temp, force, mode) -> mode
 *   Update_Display_Image(&ring[i], handle) -> u8 more_frames
 * No hardware is touched: the library imports no open/ioctl/mmap. system()/popen() are
 * interposed and refused. Without the panel's real SPI waveform (not backed up yet) the
 * generated frames are NOT meaningful; this only establishes control flow, buffer bounds
 * and what Init accepts. usage: a6l_eink_swtcon_probe [waveform.bin]
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#define RING 5
#define FRAME 0x10fe00u   /* 768 x 1450 */
#define FLASH 0x70080u
#define RGBA  0x3f4800u   /* 1440 x 720 x 4 */
struct buf { void *data; uint32_t size; uint32_t pad; };
int system(const char *c) { printf("A6L_EINK_REFUSED system(\"%s\")\n", c ? c : ""); return -1; }
FILE *popen(const char *c, const char *m) { (void)m; printf("A6L_EINK_REFUSED popen(\"%s\")\n", c ? c : ""); return NULL; }
static const char *stage = "start";
static void fault(int sig, siginfo_t *si, void *ctx) {
    (void)ctx; char b[160];
    int n = snprintf(b, sizeof b, "A6L_EINK_SWTCON_FAULT sig=%d addr=%p stage=%s\n", sig, si->si_addr, stage);
    (void)!write(1, b, (size_t)n); _exit(40);
}
static void *guarded(size_t n) { /* PROT_NONE page directly after the buffer */
    size_t pg = (size_t)sysconf(_SC_PAGESIZE), len = (n + pg - 1) / pg * pg;
    uint8_t *p = mmap(NULL, len + 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return NULL;
    mprotect(p + pg, len, PROT_READ | PROT_WRITE);
    return p + pg + (len - n);   /* end-aligned: overruns hit the guard immediately */
}
static uint32_t fnv(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
int main(int argc, char **argv) {
    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_sigaction = fault; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGILL, &sa, NULL); sigaction(SIGABRT, &sa, NULL);
    setvbuf(stdout, NULL, _IONBF, 0);
    void *h = dlopen("libtcon_eink.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("A6L_EINK_DLOPEN_ERROR %s\n", dlerror()); return 1; }
    uint64_t (*req)(void *) = dlsym(h, "Request_ProcessBuf_Size");
    void *(*init)(struct buf *, int, uint32_t *, void *, uint32_t, void *) = dlsym(h, "Init_Eink_SWTcon");
    int (*decide)(struct buf *, void *, int, int, int, int) = dlsym(h, "ModeDecision_MirrorMode");
    uint8_t (*update)(struct buf *, void *) = dlsym(h, "Update_Display_Image");
    if (!req || !init || !decide || !update) { printf("A6L_EINK_SYMBOL_MISSING\n"); return 2; }
    uint32_t cfg[4] = {1440, 720, 50, 0xdeadbeef};
    uint64_t r = req(cfg);
    printf("A6L_EINK_PROCESSBUF w=%u h=%u\n", (unsigned)(r & 0xffffffff), (unsigned)(r >> 32));
    struct buf ring[RING];
    for (int i = 0; i < RING; i++) { ring[i].data = guarded(FRAME); ring[i].size = FRAME; ring[i].pad = 0; if (!ring[i].data) return 3; memset(ring[i].data, 0, FRAME); }
    uint8_t *flash = guarded(FLASH); if (!flash) return 3; memset(flash, 0, FLASH);
    int real = 0;
    if (argc > 1) { FILE *f = fopen(argv[1], "rb"); if (f) { real = fread(flash, 1, FLASH, f) == FLASH; fclose(f); } }
    printf("A6L_EINK_WAVEFORM source=%s fnv=%08x\n", real ? "file" : "zeros", fnv(flash, FLASH));
    uint8_t info[0x400]; memset(info, 0xa6, sizeof info);
    stage = "init";
    void *handle = init(ring, RING, cfg, flash, FLASH, info);
    size_t first = sizeof info, last = 0;
    for (size_t i = 0; i < sizeof info; i++) if (info[i] != 0xa6) { if (first == sizeof info) first = i; last = i; }
    printf("A6L_EINK_INIT handle=%s cfg_guard=%s info_touched=%zu..%zu\n", handle ? "nonnull" : "NULL", cfg[3] == 0xdeadbeef ? "ok" : "CLOBBERED", first, last);
    if (first != sizeof info) { printf("A6L_EINK_INFO"); for (size_t i = first; i <= last && i < first + 96; i++) printf(" %02x", info[i]); printf("\n"); }
    if (!handle) { printf("A6L_EINK_SWTCON_DONE init_rejected=1\n"); return 0; }
    struct buf img; img.data = guarded(RGBA); img.size = RGBA; img.pad = 0; if (!img.data) return 3;
    uint8_t *px = img.data;   /* left half white, right half black, a mid-grey band */
    for (unsigned y = 0; y < 1440; y++) for (unsigned x = 0; x < 720; x++) {
        uint8_t v = y > 680 && y < 760 ? 0x80 : (x < 360 ? 0xff : 0x00); uint8_t *q = px + 4 * (y * 720u + x); q[0] = q[1] = q[2] = v; q[3] = 0xff; }
    stage = "mode-decision";
    int mode = decide(&img, handle, 25, 25, 1, 0);
    printf("A6L_EINK_MODE_DECISION returned=%d\n", mode);
    stage = "update";
    int frames = 0; uint8_t more = 1;
    while (more && frames < 300) {
        struct buf *b = &ring[frames % RING];
        more = update(b, handle);
        if (frames < 6 || !more) printf("A6L_EINK_FRAME n=%d more=%u fnv=%08x\n", frames, more, fnv(b->data, FRAME));
        frames++;
    }
    printf("A6L_EINK_SWTCON_DONE init_rejected=0 frames=%d terminated=%d waveform=%s\n", frames, !more, real ? "file" : "zeros");
    return 0;
}
