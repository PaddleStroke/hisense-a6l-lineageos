// SPDX-License-Identifier: Apache-2.0
/* The actual mirror's request snapshot, successful-frame ACK and scanout gate. */
#define main mirror_service_main
#include "../src/a6l_eink_mirror.c"
#undef main
#include <assert.h>
int main(void) {
    char v[96];
    prop_set("vendor.dualux.prepare", "10 eink");
    prop_set("sys.a6l.dualux.ready", "9 eink");
    appearance_snapshot(v, sizeof v); assert(!v[0]);
    prop_set("sys.a6l.dualux.ready", "10 eink");
    appearance_snapshot(v, sizeof v); assert(!strcmp(v, "10 eink"));
    snprintf(qappearance, sizeof qappearance, "%s", v);
    prop_set("vendor.dualux.prepare", "11 eink");
    appearance_snapshot(v, sizeof v); assert(!v[0]);
    /* A newer request must not be attributed to an older captured frame. */
    snprintf(pending_appearance, sizeof pending_appearance, "%s", qappearance);
    inflight = INF_FRAME; cmd_result(1, "test");
    prop_get("vendor.eink.ready", v, sizeof v); assert(!strcmp(v, "10 eink"));
    snprintf(pending_appearance, sizeof pending_appearance, "%s", "11 eink");
    inflight = INF_FRAME; cmd_result(0, "test");
    prop_get("vendor.eink.ready", v, sizeof v); assert(!strcmp(v, "10 eink"));
    prop_set("sys.a6l.dualux.ready", "11 eink");
    appearance_snapshot(v, sizeof v); assert(!strcmp(v, "11 eink"));
    prop_set("vendor.dualux.prepare", "12 lcd");
    prop_set("sys.a6l.dualux.ready", "12 lcd");
    appearance_snapshot(v, sizeof v); assert(!v[0]);
    struct appearance_scanout s = {0};
    assert(!appearance_scanout_update(&s, "11 eink", 1, 3, 100, 0));
    assert(!appearance_scanout_update(&s, "11 eink", 1, 3, 100, .1));
    assert(appearance_scanout_update(&s, "11 eink", 1, 3, 101, .12));
    assert(appearance_scanout_update(&s, "11 eink", 1, 3, 101, .13));
    assert(!appearance_scanout_update(&s, "12 eink", 1, 3, 101, .14));
    assert(!appearance_scanout_update(&s, "12 eink", 1, 3, 101, .63));
    assert(appearance_scanout_update(&s, "12 eink", 1, 3, 101, .65));
    assert(!appearance_scanout_update(&s, "13 eink", 1, 3, 101, .66));
    assert(appearance_scanout_update(&s, "13 eink", 1, 4, 101, .67));
    assert(appearance_scanout_update(&s, "14 eink", 0, 0, 0, .68));
    prop_set("sys.a6l.eink.no_animations", "1");
    prop_set("sys.a6l.eink.gesture_slop", "8");
    tm = (struct tmap){.touch_w=720,.touch_h=1440,.panel_w=720,.panel_h=1440,
        .front_w=1080,.front_h=2340,.img_w=720,.img_h=1440}; forward = 1;
    sl[0] = (struct slot){.in_active=1,.x=100,.y=100,.dirty=1}; touch_flush();
    assert(sl[0].out_active && !touch_any_drag()); // stationary contact/long press remains visible
    sl[0].x = 102; sl[0].dirty = 1; touch_flush(); assert(!touch_any_drag());
    sl[0].x = 120; sl[0].dirty = 1; touch_flush(); assert(touch_any_drag() && suppress_drag_frames());
    unsigned generation = gesture_generation;
    sl[0].x = 100; sl[0].dirty = 1; touch_flush(); assert(touch_any_drag()); // sticky until release
    sl[0].in_active = 0; sl[0].dirty = 1; touch_flush(); assert(!touch_any_drag() && gesture_generation != generation);
    sl[0] = (struct slot){.in_active=1,.x=100,.y=100,.dirty=1}; touch_flush();
    sl[0].x = 140; sl[0].dirty = 1; touch_flush(); touch_release_all(); assert(!touch_any_drag());
    prop_set("sys.a6l.eink.no_animations", "0"); assert(!suppress_drag_frames());
    puts("MIRROR_APPEARANCE_TEST PASS 24 checks"); return 0;
}
