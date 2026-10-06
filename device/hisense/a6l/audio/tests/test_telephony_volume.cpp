// SPDX-License-Identifier: Apache-2.0
// r5 review round4 F37 host test: the audio HAL Telephony voice-volume helper (extracted from
// audio/patches/0003-a6l-voice-volume.patch) against the real a6l-q6voiced command handling (q6voiced_shim.c).
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cmath>
#include <string>
#include <thread>

#include "helper.inc"

extern "C" {
int shim_listen(const char* path);
void shim_serve_one(int lfd);
extern long g_last_step;
extern int g_fail_err;
}

static int fails;
#define EXPECT(c, what) do { if (c) printf("ok   %s\n", what); else { printf("FAIL %s\n", what); fails++; } } while (0)

static int req(int lfd, const char* path, float v, std::string* r) {
    std::thread t([lfd] { shim_serve_one(lfd); });
    int e = a6lQ6voicedVolume(v, r, path);
    t.join();
    return e;
}

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "/tmp/a6l-tel-vol.sock";
    int lfd = shim_listen(path);
    if (lfd < 0) { printf("FAIL listen\n"); return 1; }
    std::string r;
    EXPECT(req(lfd, path, 1.0f, &r) == 0 && g_last_step == 5, "volume 1.0 -> applied, step 5");
    EXPECT(req(lfd, path, 0.5f, &r) == 0 && g_last_step == 3 && r.rfind("OK volume=3", 0) == 0, "volume 0.5 -> step 3");
    EXPECT(req(lfd, path, 0.0f, &r) == 0 && g_last_step == 0, "volume 0 -> step 0 (lowest level)");
    long last = -1; bool mono = true;
    for (int i = 0; i <= 20; i++) {
        if (req(lfd, path, i / 20.0f, &r) != 0 || g_last_step < last) mono = false;
        last = g_last_step;
    }
    EXPECT(mono && last == 5, "Android volume sweep 0..1 -> monotonic steps 0..5");
    g_fail_err = EIO; r.clear();
    EXPECT(req(lfd, path, 0.8f, &r) == EIO && r.rfind("ERR 5", 0) == 0, "DSP error -> EIO reported (not cached)");
    g_fail_err = ENOENT;
    EXPECT(req(lfd, path, 0.8f, &r) == ENOENT, "kernel without the control -> ENOENT (EX_UNSUPPORTED_OPERATION)");
    g_fail_err = 0;
    EXPECT(a6lQ6voicedVolume(1.5f, &r, path) == EINVAL, "out of range -> EINVAL, nothing sent");
    EXPECT(a6lQ6voicedVolume(NAN, &r, path) == EINVAL, "NaN -> EINVAL, nothing sent");
    EXPECT(a6lQ6voicedVolume(0.5f, &r, "/tmp/a6l-no-such-daemon.sock") != 0, "daemon absent -> error (not success)");
    close(lfd);
    unlink(path);
    printf("A6L_TELEPHONY_VOLUME_TEST %s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
