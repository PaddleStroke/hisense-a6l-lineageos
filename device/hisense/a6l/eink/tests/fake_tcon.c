// SPDX-License-Identifier: Apache-2.0
/* Host stand-in for the stock libtcon_eink.so (tests only): same 3 entry points and handle fields a6l_epdd touches
 * (+0x270 INIT flag, +0x274 call counter). Frame counts follow the QEMU measurements: INIT 99, GC16 39, fast 23, A2 10. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
struct buf { void *data; uint32_t size; uint32_t pad; };
void *Init_Eink_SWTcon(struct buf *ring, int n, uint32_t *cfg, void *flash, uint32_t size, void *info) {
    (void)ring; (void)n; (void)cfg;
    if (!flash || size < 0x70080) return NULL;
    if (((uint8_t *)flash)[0] == 0xff && ((uint8_t *)flash)[1] == 0xff) return NULL;	/* "erased" waveform -> rejected */
    uint8_t *h = calloc(1, 0x400); *(uint32_t *)(h + 0x270) = 1;
    memcpy(info, "FAKE-ED058TC7  ", 15); memcpy((char *)info + 47, "FAKE-WAVEFORM", 13); return h;
}
int ModeDecision_MirrorMode(struct buf *img, void *handle, int t1, int t2, int force, int mode) {
    (void)img; (void)t1; (void)t2; uint8_t *h = handle; uint32_t *calls = (uint32_t *)(h + 0x274), *flag = (uint32_t *)(h + 0x270);
    int n = *flag ? 99 : force ? 38 : mode == 8 || mode > 5 ? 10 : mode == 1 ? 23 : 39;
    if (*calls == 1 && *flag) *flag = 0;
    (*calls)++; *(int *)(h + 0x300) = n; *(int *)(h + 0x304) = 0; return n;
}
uint8_t Update_Display_Image(struct buf *b, void *handle) {
    uint8_t *h = handle; int *left = (int *)(h + 0x300), *k = (int *)(h + 0x304);
    memset(b->data, (*k)++ & 0xff, b->size); return --(*left) > 0;
}
