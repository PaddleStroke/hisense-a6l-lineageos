// r5 review F49 host test, HAL side: the UNCHANGED LedVibratorDevice methods of the packaged QTI VibratorOL
// (vendor/qcom/opensource/vibrator/aidl/VibratorOL/Vibrator.cpp, extracted by run-tests.sh into led_methods.inc)
// driving the UNCHANGED a6l_gpio_vib.c LED attributes (test_vib_drv.c) through a fake /sys/class/leds/vibrator.
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>
#include "vib_test_api.h"

#define ALOGE(...) ((void)0)
#define ALOGD(...) ((void)0)
#define ALOGI(...) ((void)0)
#ifndef TEMP_FAILURE_RETRY
#define TEMP_FAILURE_RETRY(x) (x)
#endif
static const char LED_DEVICE[] = "/sys/class/leds/vibrator";
class LedVibratorDevice {
public:
    LedVibratorDevice();
    int on(int32_t timeoutMs);
    int off();
    bool mDetected;
    bool mIsLdo;
private:
    int write_value(const char *file, const char *value);
};

static int fails;
#define CHECK(c) do { if (c) printf("ok   %s\n", #c); else { printf("FAIL %s (line %d)\n", #c, __LINE__); fails++; } } while (0)

static const char *names[] = {"activate", "duration", "state"};
static std::string opened[16];
static int writes;
static int fake_open(const char *path, int) {
    std::string p(path), pre = std::string(LED_DEVICE) + "/";
    if (p.rfind(pre, 0) == 0) {
        std::string a = p.substr(pre.size());
        for (int i = 0; i < 3; i++)
            if (a == names[i] && kd_has_attr(names[i])) { opened[i] = a; return 100 + i; }
    }
    errno = ENOENT; return -1;
}
static ssize_t fake_write(int fd, const void *b, size_t n) {
    if (fd < 100 || fd > 102) { errno = EBADF; return -1; }
    writes++;
    ssize_t r = kd_store(names[fd - 100], (const char *)b, n);
    if (r < 0) { errno = (int)-r; return -1; }
    return r;
}
static int fake_close(int) { return 0; }
static char *fake_realpath(const char *, char *) { return nullptr; }
#define open fake_open
#define write fake_write
#define close fake_close
#define realpath fake_realpath
#include "led_methods.inc"
#undef open
#undef write
#undef close
#undef realpath

static std::string show(const char *a) { char b[64] = {0}; kd_show(a, b); return b; }

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // ordering: the service constructs its devices once; before the module is loaded nothing is detected
    { LedVibratorDevice early; CHECK(!early.mDetected); }
    // failed LED registration fails the probe (no half-registered vibrator)
    kd_set_led_register_err(-EEXIST); CHECK(kd_probe() == -EEXIST); kd_set_led_register_err(0);
    CHECK(kd_probe() == 0);
    CHECK(std::string(kd_led_name()) == "vibrator");
    LedVibratorDevice led;
    CHECK(led.mDetected);           // VibratorOL uses this backend in on()/off(); caps = ON_CALLBACK only
    CHECK(!led.mIsLdo);             // no 50..15000 ms HAL clamp: the driver caps
    CHECK(kd_gpio() == 0);
    // on(400): state=1, duration=400, activate=1 -> GPIO high for 400 ms, then low
    CHECK(led.on(400) == 0);
    CHECK(kd_gpio() == 1);
    CHECK(show("state") == "1\n" && show("duration") == "400\n" && show("activate") == "1\n");
    kd_advance(399); CHECK(kd_gpio() == 1);
    kd_advance(1); CHECK(kd_gpio() == 0); CHECK(show("activate") == "0\n");
    CHECK(kd_on_count() == 1);
    // off() cancels at once, no late toggle
    CHECK(led.on(1000) == 0); kd_advance(100); CHECK(kd_gpio() == 1);
    CHECK(led.off() == 0); CHECK(kd_gpio() == 0); CHECK(!kd_timed_pending());
    kd_advance(2000); CHECK(kd_gpio() == 0); CHECK(kd_on_count() == 2);
    // re-activation replaces the previous one (restarts the timer)
    CHECK(led.on(500) == 0); kd_advance(300); CHECK(led.on(500) == 0);
    kd_advance(499); CHECK(kd_gpio() == 1); kd_advance(1); CHECK(kd_gpio() == 0);
    // cap: 20 s request -> 15 s (stock timed-gpio max)
    CHECK(led.on(20000) == 0); kd_advance(14999); CHECK(kd_gpio() == 1); kd_advance(1); CHECK(kd_gpio() == 0);
    // zero duration / state 0 -> stays off; bad values rejected (HAL sees the write error)
    CHECK(led.on(0) == 0); CHECK(kd_gpio() == 0); CHECK(!kd_timed_pending());
    CHECK(kd_store("state", "0", 2) == 2); CHECK(kd_store("duration", "300", 4) == 4);
    CHECK(kd_store("activate", "1", 2) == 2); CHECK(kd_gpio() == 0);
    CHECK(kd_store("activate", "2", 2) == -EINVAL); CHECK(kd_store("duration", "x", 2) == -EINVAL);
    CHECK(kd_store("activate", "-1", 3) == -EINVAL);
    // suspend during an activation: off; resume before expiry: on again; expiry: off
    CHECK(led.on(1000) == 0); CHECK(kd_suspend() == 0); CHECK(kd_gpio() == 0);
    kd_advance(200); CHECK(kd_resume() == 0); CHECK(kd_gpio() == 1);
    kd_advance(800); CHECK(kd_gpio() == 0);
    // FF input path (a6l_vib test tool) still works and is independent of the LED activation
    kd_ff(1); CHECK(kd_gpio() == 1); CHECK(led.on(100) == 0); kd_advance(100); CHECK(kd_gpio() == 1);
    kd_ff(0); CHECK(kd_gpio() == 0);
    // brightness: non-zero refused (no untimed on), 0 stops an activation
    CHECK(kd_brightness(1) == -EOPNOTSUPP); CHECK(kd_gpio() == 0);
    CHECK(led.on(1000) == 0); CHECK(kd_brightness(0) == 0); CHECK(kd_gpio() == 0);
    // remove during an activation: GPIO low, LED gone, HAL writes fail (error propagated), no late toggle
    CHECK(led.on(1000) == 0); CHECK(kd_gpio() == 1);
    kd_remove(); CHECK(kd_gpio() == 0); CHECK(!kd_led_registered());
    CHECK(led.on(400) != 0);
    { LedVibratorDevice gone; CHECK(!gone.mDetected); }
    printf("A6L_VIB_F49_TEST %s %d\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
