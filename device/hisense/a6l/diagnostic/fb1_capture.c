// SPDX-License-Identifier: Apache-2.0
/* A6L, ROOTED STOCK only: capture what the stock hwcomposer really scans out on the e-ink framebuffer (fb1).
 * Read-only: FBIOGET_* + PROT_READ mmap. Prints the fb format, then polls the visible page (yoffset) and stores every
 * distinct frame, run-length encoded, in the .a6lepd format that a6l_epd_play replays on the mainline kernel.
 * usage: a6l_fb1_capture /dev/graphics/fb1 <seconds> <out.a6lepd>   (start it, then trigger an e-ink update in stock) */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
static uint32_t fnv(const uint8_t *p, size_t n) { uint32_t h = 2166136261u; while (n--) { h ^= *p++; h *= 16777619u; } return h; }
int main(int argc, char **argv) {
    if (argc != 4) return 2; int secs = atoi(argv[2]);
    int fd = open(argv[1], O_RDONLY); if (fd < 0) { perror("fb"); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &v) || ioctl(fd, FBIOGET_FSCREENINFO, &f)) { perror("fbioget"); return 1; }
    printf("A6L_FB1 id=%.16s xres=%u yres=%u virt=%ux%u off=%u,%u bpp=%u line_length=%u smem_len=%u\n", f.id, v.xres, v.yres, v.xres_virtual, v.yres_virtual, v.xoffset, v.yoffset, v.bits_per_pixel, f.line_length, f.smem_len);
    printf("A6L_FB1 red=%u/%u green=%u/%u blue=%u/%u transp=%u/%u pixclock=%u margins l%u r%u u%u d%u hs%u vs%u\n", v.red.offset, v.red.length, v.green.offset, v.green.length, v.blue.offset, v.blue.length, v.transp.offset, v.transp.length, v.pixclock, v.left_margin, v.right_margin, v.upper_margin, v.lower_margin, v.hsync_len, v.vsync_len);
    if (v.bits_per_pixel != 32 || v.xres != 384 || v.yres != 725) { printf("A6L_FB1_NOTE unexpected geometry, capturing anyway if 32bpp\n"); if (v.bits_per_pixel != 32) return 3; }
    uint8_t *m = mmap(0, f.smem_len, PROT_READ, MAP_SHARED, fd, 0); if (m == MAP_FAILED) { perror("mmap"); return 1; }
    FILE *o = fopen(argv[3], "wb"); if (!o) { perror("out"); return 1; }
    uint32_t hdr[3] = {v.xres, v.yres, 0}; fwrite("A6LEPD1\n", 1, 8, o); fwrite(hdr, 4, 3, o);
    size_t px = (size_t)v.xres * v.yres; uint32_t *row = malloc(px * 4), last = 0, frames = 0, lastyo = ~0u; time_t end = time(0) + secs;
    while (time(0) < end && frames < 400) {
        ioctl(fd, FBIOGET_VSCREENINFO, &v);
        const uint8_t *src = m + (size_t)v.yoffset * f.line_length;
        if ((size_t)v.yoffset * f.line_length + (size_t)v.yres * f.line_length > f.smem_len) src = m;
        for (unsigned y = 0; y < v.yres; y++) memcpy(row + (size_t)y * v.xres, src + (size_t)y * f.line_length, v.xres * 4);
        uint32_t h = fnv((uint8_t *)row, px * 4);
        if (h != last || v.yoffset != lastyo) {
            if (h != last) { uint32_t runs = 0; long pos = ftell(o); fwrite(&runs, 4, 1, o);
                for (size_t i = 0; i < px;) { size_t j = i + 1; while (j < px && row[j] == row[i]) j++; uint32_t cv[2] = {(uint32_t)(j - i), row[i]}; fwrite(cv, 4, 2, o); runs++; i = j; }
                long e = ftell(o); fseek(o, pos, SEEK_SET); fwrite(&runs, 4, 1, o); fseek(o, e, SEEK_SET); frames++;
                if (frames <= 3 || frames % 20 == 0) printf("A6L_FB1 frame=%u yoffset=%u fnv=%08x runs=%u first_px=%08x\n", frames, v.yoffset, h, runs, row[0]); }
            last = h; lastyo = v.yoffset; }
        usleep(2000);
    }
    hdr[2] = frames; fseek(o, 8, SEEK_SET); fwrite(hdr, 4, 3, o); fclose(o);
    printf("A6L_FB1_DONE frames=%u\n", frames); return 0;
}
