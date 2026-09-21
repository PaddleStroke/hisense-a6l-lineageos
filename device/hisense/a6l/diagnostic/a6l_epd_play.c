// SPDX-License-Identifier: Apache-2.0
/* A6L e-paper bring-up player: scans out a pre-computed drive-frame sequence (.a6lepd, produced offline in QEMU by
 * the stock software TCON with this panel's own waveform) on the DPI connector, one frame per vblank.
 *   a6l_epd_play [--card /dev/dri/cardN] [--lead 20] [--tail 10] [--dry] file.a6lepd [more.a6lepd ...]
 * Sequence: modeset with an IDLE frame (real strobes, data byte zeroed = "no drive") -> lead idle frames while the
 * rails settle -> every drive frame exactly once -> tail idle frames -> CRTC off (panel driver drops the rails).
 * The +-15 V rails are only on if panel-a6l-epd was loaded with hv=1; with hv=0 this is a pure transport test.
 * Needs DRM master (recovery: nobody else has it). Reports vblank sequence gaps (= a frame shown twice). */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#define W 384
#define H 725
struct fb { uint32_t handle, id; uint32_t *map; size_t size; };
static int fd = -1;
static int mkfb(struct fb *f) {
    struct drm_mode_create_dumb c = {.width = W, .height = H, .bpp = 32};
    if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &c)) return -1;
    uint32_t hs[4] = {c.handle}, ps[4] = {c.pitch}, os[4] = {0};
    if (c.pitch != W * 4) { fprintf(stderr, "A6L_EPD_FAIL pitch %u\n", c.pitch); return -1; }
    if (drmModeAddFB2(fd, W, H, DRM_FORMAT_XRGB8888, hs, ps, os, &f->id, 0)) return -1;
    struct drm_mode_map_dumb m = {.handle = c.handle};
    if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &m)) return -1;
    f->map = mmap(0, c.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, m.offset);
    f->handle = c.handle; f->size = c.size; return f->map == MAP_FAILED ? -1 : 0;
}
static unsigned last_seq, gaps, flips; static int pending;
static void on_flip(int f, unsigned seq, unsigned s, unsigned us, void *d) {
    (void)f; (void)s; (void)us; (void)d; if (flips && seq != last_seq + 1) gaps += seq - last_seq - 1; last_seq = seq; flips++; pending = 0;
}
static int flip(uint32_t crtc, uint32_t fbid) {
    drmEventContext ev = {.version = 2, .page_flip_handler = on_flip};
    if (drmModePageFlip(fd, crtc, fbid, DRM_MODE_PAGE_FLIP_EVENT, 0)) { perror("A6L_EPD_FAIL pageflip"); return -1; }
    pending = 1;
    while (pending) { struct pollfd p = {.fd = fd, .events = POLLIN}; if (poll(&p, 1, 1000) <= 0) { fprintf(stderr, "A6L_EPD_FAIL vblank timeout\n"); return -1; } drmHandleEvent(fd, &ev); }
    return 0;
}
int main(int argc, char **argv) {
    const char *card = NULL; int lead = 20, tail = 10, dry = 0, first = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--card") && i + 1 < argc) card = argv[++i];
        else if (!strcmp(argv[i], "--lead") && i + 1 < argc) lead = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--tail") && i + 1 < argc) tail = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dry")) dry = 1;
        else { first = i; break; }
    }
    if (!first) { fprintf(stderr, "usage: %s [--card dev] [--lead n] [--tail n] [--dry] file.a6lepd...\n", argv[0]); return 2; }
    /* ---- load + validate every sequence before touching DRM ---- */
    int nseq = argc - first; uint32_t total = 0; uint8_t **blob = calloc(nseq, sizeof *blob); size_t *len = calloc(nseq, sizeof *len);
    for (int s = 0; s < nseq; s++) {
        FILE *f = fopen(argv[first + s], "rb"); if (!f) { perror(argv[first + s]); return 2; }
        fseek(f, 0, SEEK_END); len[s] = ftell(f); rewind(f); blob[s] = malloc(len[s]);
        if (fread(blob[s], 1, len[s], f) != len[s]) return 2; fclose(f);
        uint32_t hdr[3]; if (len[s] < 20 || memcmp(blob[s], "A6LEPD1\n", 8)) { fprintf(stderr, "A6L_EPD_FAIL bad magic\n"); return 2; }
        memcpy(hdr, blob[s] + 8, 12); if (hdr[0] != W || hdr[1] != H || hdr[2] == 0 || hdr[2] > 400) { fprintf(stderr, "A6L_EPD_FAIL bad header\n"); return 2; }
        size_t pos = 20;
        for (uint32_t n = 0; n < hdr[2]; n++) { uint32_t runs; if (pos + 4 > len[s]) return 2; memcpy(&runs, blob[s] + pos, 4); pos += 4; uint64_t px = 0;
            for (uint32_t r = 0; r < runs; r++) { uint32_t cv[2]; if (pos + 8 > len[s]) return 2; memcpy(cv, blob[s] + pos, 8); pos += 8; px += cv[0];
                uint8_t d = cv[1] & 0xff; for (int k = 0; k < 4; k++) if (((d >> (2 * k)) & 3) == 3) { fprintf(stderr, "A6L_EPD_FAIL illegal drive code 11\n"); return 2; } }
            if (px != (uint64_t)W * H) { fprintf(stderr, "A6L_EPD_FAIL frame %u pixel count\n", n); return 2; } }
        if (pos != len[s]) { fprintf(stderr, "A6L_EPD_FAIL trailing bytes\n"); return 2; }
        printf("A6L_EPD_SEQ %s frames=%u\n", argv[first + s], hdr[2]); total += hdr[2];
    }
    if (dry) { printf("A6L_EPD_DRY_OK sequences=%d frames=%u\n", nseq, total); return 0; }
    /* ---- find the DPI connector ---- */
    drmModeRes *res = NULL; drmModeConnector *con = NULL;
    for (int c = 0; c < 4 && !con; c++) {
        char path[32]; snprintf(path, sizeof path, "/dev/dri/card%d", c); if (card && strcmp(card, path)) continue;
        fd = open(path, O_RDWR | O_CLOEXEC); if (fd < 0) continue;
        res = drmModeGetResources(fd);
        for (int i = 0; res && i < res->count_connectors; i++) { drmModeConnector *k = drmModeGetConnector(fd, res->connectors[i]);
            if (k && k->connector_type == DRM_MODE_CONNECTOR_DPI && k->connection == DRM_MODE_CONNECTED && k->count_modes) { con = k; break; } drmModeFreeConnector(k); }
        if (!con) { if (res) drmModeFreeResources(res); res = NULL; close(fd); fd = -1; }
    }
    if (!con) { fprintf(stderr, "A6L_EPD_FAIL no connected DPI connector\n"); return 3; }
    drmModeModeInfo *mode = &con->modes[0];
    if (mode->hdisplay != W || mode->vdisplay != H) { fprintf(stderr, "A6L_EPD_FAIL mode %ux%u\n", mode->hdisplay, mode->vdisplay); return 3; }
    uint32_t crtc = 0; int ci = -1;
    for (int e = 0; e < con->count_encoders && !crtc; e++) { drmModeEncoder *enc = drmModeGetEncoder(fd, con->encoders[e]); if (!enc) continue;
        for (int i = 0; i < res->count_crtcs; i++) if (enc->possible_crtcs & (1u << i)) { int used = 0;   /* skip a CRTC that drives another connector (the LCD) */
            for (int j = 0; j < res->count_encoders; j++) { drmModeEncoder *o = drmModeGetEncoder(fd, res->encoders[j]); if (o && o->encoder_id != enc->encoder_id && o->crtc_id == res->crtcs[i]) used = 1; drmModeFreeEncoder(o); }
            if (!used) { crtc = res->crtcs[i]; ci = i; break; } }
        drmModeFreeEncoder(enc); }
    if (!crtc) { fprintf(stderr, "A6L_EPD_FAIL no free crtc\n"); return 3; }
    printf("A6L_EPD_DRM connector=%u crtc=%u(index %d) mode=%s@%u\n", con->connector_id, crtc, ci, mode->name, mode->vrefresh);
    if (drmSetMaster(fd)) perror("A6L_EPD_NOTE setmaster");
    struct fb idle, a, b; if (mkfb(&idle) || mkfb(&a) || mkfb(&b)) { perror("A6L_EPD_FAIL dumb fb"); return 4; }
    /* idle = strobes of the very first drive frame with the data byte cleared */
    { size_t pos = 24; uint32_t runs; memcpy(&runs, blob[0] + 20, 4); uint32_t *o = idle.map;
      for (uint32_t r = 0; r < runs; r++) { uint32_t cv[2]; memcpy(cv, blob[0] + pos, 8); pos += 8; for (uint32_t k = 0; k < cv[0]; k++) *o++ = cv[1] & 0xffffff00; } }
    int rc = 5;
    if (drmModeSetCrtc(fd, crtc, idle.id, 0, 0, &con->connector_id, 1, mode)) { perror("A6L_EPD_FAIL setcrtc"); goto off; }
    for (int i = 0; i < lead; i++) if (flip(crtc, idle.id)) goto off;
    for (int s = 0; s < nseq; s++) { uint32_t n; memcpy(&n, blob[s] + 16, 4); size_t pos = 20;
        for (uint32_t f = 0; f < n; f++) { struct fb *t = (f & 1) ? &b : &a; uint32_t runs; memcpy(&runs, blob[s] + pos, 4); pos += 4; uint32_t *o = t->map;
            for (uint32_t r = 0; r < runs; r++) { uint32_t cv[2]; memcpy(cv, blob[s] + pos, 8); pos += 8; for (uint32_t k = 0; k < cv[0]; k++) *o++ = cv[1]; }
            if (flip(crtc, t->id)) goto off; }
        for (int i = 0; i < tail; i++) if (flip(crtc, idle.id)) goto off; }
    rc = 0;
off:
    drmModeSetCrtc(fd, crtc, 0, 0, 0, NULL, 0, NULL);
    printf("A6L_EPD_PLAY_%s flips=%u repeated_frames=%u\n", rc ? "FAIL" : "DONE", flips, gaps);
    return rc;
}
