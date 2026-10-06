#define _GNU_SOURCE
#include <poll.h>
#include <setjmp.h>
#include <assert.h>
static jmp_buf stopped;
static int capture, captured_timeout;
static int review_poll(struct pollfd *p, nfds_t n, int timeout) {
    if (capture) { captured_timeout = timeout; longjmp(stopped, 1); }
    return poll(p, n, timeout);
}
#define poll review_poll
#include "sensors_a6l.c"
#undef poll
static void attr(struct a6l_ctx *c, const char *name, const char *v) {
    assert(write_sys(c, name, v) == 0);
}
int main(void) {
    struct hw_device_t *d = NULL;
    assert(a6l_open(&HAL_MODULE_INFO_SYM.common, SENSORS_HARDWARE_POLL, &d) == 0);
    struct a6l_ctx *c = (struct a6l_ctx *)d;
    assert(c->sys[0]);
    const char *en = "events/in_proximity_thresh_rising_en";
    assert(read_long(c, en, -1) == 0);
    char p[320], saved[330];
    snprintf(p, sizeof(p), "%s/%s", c->sys, en);
    snprintf(saved, sizeof(saved), "%s.saved", p);
    assert(rename(p, saved) == 0); assert(mkdir(p, 0700) == 0);
    int rc = a6l_activate(&c->dev.v0, H_PROX, 1);
    assert(rc == 0 && c->evfd >= 0 && c->prox_on);
    assert(rmdir(p) == 0); assert(rename(saved, p) == 0);
    sensors_event_t events[8];
    assert(a6l_poll(&c->dev.v0, events, 8) == 1);
    assert(events[0].sensor == H_PROX);
    assert(a6l_activate(&c->dev.v0, H_LIGHT, 1) == 0);
    for (int i = 0; i < 12; ++i) {
        char raw[32]; snprintf(raw, sizeof(raw), "%d", 100 + i);
        attr(c, "in_illuminance_raw", raw);
        assert(a6l_poll(&c->dev.v0, events, 8) > 0);
        usleep(210000);
    }
    assert(read_long(c, en, -1) == 0);
    assert(read_long(c, "events/in_proximity_thresh_falling_en", -1) == 0);
    puts("PROX_ENABLE_FAILED activate_rc=0 valid_event_fd=1 interrupt_enable=0 after_12_poll_calls_and_2.5_seconds");
    // Fix the injection manually, then inspect the real HAL's idle event wait.
    assert(a6l_activate(&c->dev.v0, H_PROX, 1) == 0);
    assert(a6l_activate(&c->dev.v0, H_LIGHT, 0) == 0);
    assert(a6l_poll(&c->dev.v0, events, 8) == 1);
    char drain[100]; while (read(c->wake[0], drain, sizeof(drain)) > 0) {}
    capture = 1;
    if (!setjmp(stopped)) a6l_poll(&c->dev.v0, events, 8);
    assert(captured_timeout == -1);
    puts("PROX_IDLE_WAIT timeout=-1 (unbounded; see exact kernel poll contract test)");
    a6l_close(d);
}
