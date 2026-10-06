// SPDX-License-Identifier: Apache-2.0
/* External KMS integers from include/drm/drm_blend.h, independent of our enum.
 * Symbol-only tests cannot detect a wrong userspace/kernel numeric mapping.
 */
#include <stdio.h>
#include <string.h>
#include "../src/eink_logic.h"

enum { KMS_PREMULTIPLIED = 0, KMS_COVERAGE = 1, KMS_NONE = 2 };
static unsigned checks, failures;
static void check(int condition, const char *name) {
    checks++;
    if (!condition) { fprintf(stderr, "FAIL %s\n", name); failures++; }
}
int main(void) {
    check((int)PLANE_BLEND_PREMULTI == KMS_PREMULTIPLIED &&
          (int)PLANE_BLEND_COVERAGE == KMS_COVERAGE && (int)PLANE_BLEND_NONE == KMS_NONE,
          "external DRM numeric blend ABI");
    const unsigned alphas[] = {0, 64, 128, 255};
    const unsigned plane_alphas[] = {0, 32768, 65535};
    for (unsigned raw = 0; raw < 3; raw++)
        for (unsigned ai = 0; ai < 4; ai++)
            for (unsigned gi = 0; gi < 3; gi++)
                for (unsigned general = 0; general < 2; general++)
                    for (unsigned has_alpha = 0; has_alpha < 2; has_alpha++) {
                        unsigned a = has_alpha ? alphas[ai] : 255;
                        unsigned rgb = raw == KMS_PREMULTIPLIED ? a * 200 / 255 : 200;
                        unsigned char pixels[8 * 8 * 4], canvas[8 * 8];
                        for (unsigned i = 0; i < sizeof pixels; i += 4) {
                            pixels[i] = pixels[i+1] = pixels[i+2] = rgb;
                            pixels[i+3] = alphas[ai];
                        }
                        memset(canvas, 100, sizeof canvas);
                        struct plane_geo q = {0, 0, 8, 8, 0, 0, 8, 8,
                            general ? PLANE_ROT_180 : PLANE_ROT_0, plane_alphas[gi], raw};
                        struct plane_fb fb = {pixels, 32, 8, 8, 4, 0, has_alpha};
                        double pa = plane_alphas[gi] / 65535.0, A = a / 255.0;
                        double value = raw == KMS_NONE ? pa * rgb + (1-pa) * 100 :
                            raw == KMS_COVERAGE ? pa * A * rgb + (1-pa*A) * 100 :
                            pa * rgb + (1-pa*A) * 100;
                        unsigned expected = (unsigned)(value + 0.5);
                        if (expected > 255) expected = 255;
                        int rc = plane_compose(canvas, 8, 8, &q, &fb);
                        char label[128];
                        snprintf(label, sizeof label,
                            "raw=%u pixel_alpha=%u plane_alpha=%u general=%u has_alpha=%u expected=%u actual=%u",
                            raw, alphas[ai], plane_alphas[gi], general, has_alpha, expected, canvas[0]);
                        check(rc == 0 && canvas[0] == expected, label);
                    }
    struct plane_geo q = {0, 0, 8, 8, 0, 0, 8, 8, PLANE_ROT_0, 65535, KMS_PREMULTIPLIED};
    check(!plane_opaque_fullscreen(&q, 8, 8, 8, 8), "premultiplied metadata cannot prove opaque occlusion");
    q.blend = KMS_COVERAGE;
    check(!plane_opaque_fullscreen(&q, 8, 8, 8, 8), "coverage metadata cannot prove opaque occlusion");
    q.blend = KMS_NONE;
    check(plane_opaque_fullscreen(&q, 8, 8, 8, 8), "None with full plane alpha ignores pixel alpha and occludes");
    q.alpha16 = 32768;
    check(!plane_opaque_fullscreen(&q, 8, 8, 8, 8), "None with partial plane alpha does not occlude");
    q.alpha16 = 65535; q.cw = 7;
    check(!plane_opaque_fullscreen(&q, 8, 8, 8, 8), "partial destination does not occlude whole screen");
    printf("PLANE_BLEND_ABI %s: %u checks, %u failures\n", failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
