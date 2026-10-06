// SPDX-License-Identifier: Apache-2.0
/* Frozen generic plane oracle: tests alpha integer fast path and fallback. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../src/eink_logic.h"
static uint8_t reference_luma(int r, int g, int b) { return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8); }	/* = the mirror's luma() */
static int reference_compose(uint8_t *gray, int gw, int gh, const struct plane_geo *q, const struct plane_fb *fb) {
    if (plane_supported(q) || q->cw <= 0 || q->ch <= 0) return -1;
    const uint32_t rot = q->rotation & 0xf; const double pa = q->alpha16 / 65535.0;
    for (int y = q->cy < 0 ? 0 : q->cy; y < q->cy + q->ch && y < gh; y++) {
        for (int x = q->cx < 0 ? 0 : q->cx; x < q->cx + q->cw && x < gw; x++) {
            double u = (x - q->cx + 0.5) / q->cw, v = (y - q->cy + 0.5) / q->ch, xn, yn;	/* normalised destination */
            /* inverse of the counter-clockwise rotation, then of the reflection: normalised source coordinates */
            if (rot == PLANE_ROT_90) { xn = 1 - v; yn = u; }
            else if (rot == PLANE_ROT_180) { xn = 1 - u; yn = 1 - v; }
            else if (rot == PLANE_ROT_270) { xn = v; yn = 1 - u; }
            else { xn = u; yn = v; }
            if (q->rotation & PLANE_REFLECT_X) xn = 1 - xn;
            if (q->rotation & PLANE_REFLECT_Y) yn = 1 - yn;
            int sxx = (int)(q->sx + xn * q->sw), syy = (int)(q->sy + yn * q->sh);
            if (sxx < 0 || sxx >= fb->w || syy < 0 || syy >= fb->h) continue;
            const uint8_t *s = fb->base + (size_t)syy * fb->pitch + (size_t)sxx * fb->bpp; uint8_t lv; int a = 255;
            if (fb->bpp == 4) { lv = fb->bgr ? reference_luma(s[2], s[1], s[0]) : reference_luma(s[0], s[1], s[2]); if (fb->has_alpha) a = s[3]; }
            else { uint16_t w16 = (uint16_t)(s[0] | s[1] << 8); lv = reference_luma((w16 >> 11) << 3, ((w16 >> 5) & 63) << 2, (w16 & 31) << 3); }
            uint8_t *d = &gray[(size_t)y * gw + x];
            double A = q->blend == PLANE_BLEND_NONE ? 1.0 : a / 255.0, o;
            if (q->blend == PLANE_BLEND_COVERAGE) o = pa * A * lv + (1 - pa * A) * *d;
            else o = pa * lv + (1 - pa * A) * *d;	/* None (A = 1) and pre-multiplied */
            int iv = (int)(o + 0.5); *d = (uint8_t)(iv > 255 ? 255 : iv < 0 ? 0 : iv);
        }
    }
    return 0;
}
static unsigned seed=1;
static unsigned rnd(void) { seed=seed*1664525u+1013904223u; return seed; }
static double tm(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec/1e9; }
int main(void) {
 unsigned cases=0;
 unsigned char px[64*64*4], a[64*64], b[64*64];
 for(int k=0;k<1536;k++) {
  for(unsigned j=0;j<sizeof px;j++) px[j]=rnd()>>24;
  for(unsigned j=0;j<sizeof a;j++) a[j]=rnd()>>24;
  memcpy(b,a,sizeof a);
  struct plane_geo q={-3,2,31,29,4,6,31,29,PLANE_ROT_0,65535,k%3};
  struct plane_fb f={px,64*4,64,64,4,(k>>2)&1,(k>>3)&1};
  if(k%7==1)q.alpha16=rnd()&65535;
  if(k%7==2)q.rotation=PLANE_ROT_90;
  if(k%7==3)q.sx=4.25;
  if(k%7==4)q.sw=29;
  if(k%7==5)q.rotation|=PLANE_REFLECT_X;
  if(k%7==6){f.bpp=2;f.pitch=64*2;}
  if(plane_compose(a,64,64,&q,&f)!=reference_compose(b,64,64,&q,&f)||memcmp(a,b,sizeof a)) {fprintf(stderr,"FAIL plane case %d\n",k);return 1;}
  cases++;
 }
 /* Exhaust every 8-bit luma/background/alpha combination in the coverage and
  * premultiplied equations, comparing original floating-point rounding. */
 for(int blend=1;blend<=2;blend++)for(int alpha=0;alpha<256;alpha++)for(int lv=0;lv<256;lv++)for(int d=0;d<256;d++) {
  double A=alpha/255.0, o=blend==PLANE_BLEND_COVERAGE ? A*lv+(1-A)*d : lv+(1-A)*d;
  int old=(int)(o+0.5);if(old>255)old=255;
  int v=blend==PLANE_BLEND_COVERAGE ? (alpha*lv+(255-alpha)*d+127)/255 : lv+((255-alpha)*d+127)/255;
  if(v>255)v=255;
  if(old!=v){fprintf(stderr,"FAIL blend %d a%d l%d d%d\n",blend,alpha,lv,d);return 1;}
 }
 printf("PLANE_INTEGER PASS %u image cases and 33554432 blend values\n",cases);
 size_t pixels=1080*2340;unsigned char *p=malloc(pixels*4), *ga=malloc(pixels),*gb=malloc(pixels);
 if(!p||!ga||!gb)return 2;
 for(size_t j=0;j<pixels*4;j++)p[j]=rnd()>>24;
 for(int opaque=0;opaque<2;opaque++)for(int blend=1;blend<=2;blend++) {
  if(opaque)for(size_t j=0;j<pixels;j++)p[j*4+3]=255;
  memset(ga,117,pixels);memcpy(gb,ga,pixels);
  struct plane_geo q={0,0,1080,2340,0,0,1080,2340,PLANE_ROT_0,65535,blend};
  struct plane_fb f={p,1080*4,1080,2340,4,1,1};
  double t=tm();reference_compose(gb,1080,2340,&q,&f);double old=tm()-t;
  t=tm();plane_compose(ga,1080,2340,&q,&f);double fast=tm()-t;
  if(memcmp(ga,gb,pixels))return 1;
  printf("FULL_ALPHA_FRAME PASS opaque=%d blend=%d old_ms=%.3f new_ms=%.3f speedup=%.2f\n",opaque,blend,old*1000,fast*1000,old/fast);
 }
 free(p);free(ga);free(gb);return 0;
}
