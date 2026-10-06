// SPDX-License-Identifier: Apache-2.0
// A6L RTC offset keeper (r5 bug hunt round2 gnss-time R1, 29 Sep 2026). See timekeep.h for the why.
// usage: a6l_timekeep [--restore-only] [--dry-run] [--min-utc S] [--file PATH] [--rtc-dev /dev/rtc0 | --rtc-file PATH]
//   --restore-only  boot step (init exec_start, before zygote): set the clock from RTC + stored offset, then exit
//   default         restore, then watch clock changes (timerfd CANCEL_ON_SET) and store the new offset
//   --rtc-file      host tests: RTC seconds read from a text file instead of /dev/rtc0 RTC_RD_TIME
#include <errno.h>
#include <fcntl.h>
#include <linux/rtc.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <string>

#include "timekeep.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

using namespace a6l::timekeep;

namespace {

void logf(int prio, const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
#ifdef __ANDROID__
    __android_log_write(prio, "a6l_timekeep", buf);
#else
    (void)prio;
#endif
    fprintf(stderr, "a6l_timekeep: %s\n", buf);
}
#ifdef __ANDROID__
constexpr int kI = ANDROID_LOG_INFO, kW = ANDROID_LOG_WARN, kE = ANDROID_LOG_ERROR;
#else
constexpr int kI = 4, kW = 5, kE = 6;
#endif

struct Opts {
    bool restoreOnly = false, dryRun = false;
    int64_t minUtc = 0;
    std::string file = "/data/vendor/a6l_time/rtc_offset";
    std::string rtcDev = "/dev/rtc0";
    std::string rtcFile;
};

// RTC seconds since the epoch (UTC, as the driver reports it). /dev/rtc0 is single-open (rtc-dev RTC_DEV_BUSY):
// open/read/close at once and retry briefly when system_server holds it for its (failing) RTC_SET_TIME.
bool readRtc(const Opts& o, int64_t* out) {
    if (!o.rtcFile.empty()) {
        FILE* f = fopen(o.rtcFile.c_str(), "re");
        if (!f) return false;
        long long v = -1;
        bool ok = fscanf(f, "%lld", &v) == 1 && v >= 0;
        fclose(f);
        *out = v;
        return ok;
    }
    for (int i = 0; i < 20; i++) {
        int fd = open(o.rtcDev.c_str(), O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EBUSY) {
                usleep(50 * 1000);
                continue;
            }
            return false;
        }
        struct rtc_time rt;
        memset(&rt, 0, sizeof rt);
        int r = ioctl(fd, RTC_RD_TIME, &rt);
        close(fd);
        if (r < 0) return false;
        struct tm tm;
        memset(&tm, 0, sizeof tm);
        tm.tm_sec = rt.tm_sec;
        tm.tm_min = rt.tm_min;
        tm.tm_hour = rt.tm_hour;
        tm.tm_mday = rt.tm_mday;
        tm.tm_mon = rt.tm_mon;
        tm.tm_year = rt.tm_year;
        time_t t = timegm(&tm);
        if (t == time_t(-1)) return false;
        *out = int64_t(t);
        return true;
    }
    errno = EBUSY;
    return false;
}

int64_t realtimeS() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return int64_t(ts.tv_sec);
}

std::optional<int64_t> loadOffset(const std::string& path) {
    FILE* f = fopen(path.c_str(), "re");
    if (!f) return std::nullopt;
    char buf[96];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    auto v = parseOffset(std::string(buf, n));
    if (!v) logf(kW, "%s: unreadable offset, ignored", path.c_str());
    return v;
}

// tmp + fsync + rename + directory fsync: a crash never leaves a torn file (parseOffset rejects partial ones anyway)
bool storeOffset(const std::string& path, int64_t off) {
    const std::string tmp = path + ".tmp";
    int fd = open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string s = formatOffset(off);
    bool ok = write(fd, s.data(), s.size()) == ssize_t(s.size()) && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    std::string dir = path.substr(0, path.find_last_of('/') == std::string::npos ? 0 : path.find_last_of('/'));
    if (dir.empty()) dir = ".";
    int dfd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dfd >= 0) {
        fsync(dfd);
        close(dfd);
    }
    return true;
}

void restore(const Opts& o, std::optional<int64_t> off) {
    int64_t rtc = 0;
    if (!readRtc(o, &rtc)) {
        logf(kE, "RTC unreadable (%s): nothing restored", strerror(errno));
        return;
    }
    const int64_t now = realtimeS();
    auto t = restoreTarget(rtc, now, off, o.minUtc);
    if (!t) {
        logf(kI, "restore: nothing to do (rtc=%lld clock=%lld offset=%s min_utc=%lld)", (long long)rtc,
             (long long)now, off ? std::to_string(*off).c_str() : "none", (long long)o.minUtc);
        return;
    }
    if (o.dryRun) {
        printf("A6L_TIMEKEEP RESTORE %lld\n", (long long)*t);
        return;
    }
    struct timeval tv{time_t(*t), 0};
    if (settimeofday(&tv, nullptr) != 0)
        logf(kE, "settimeofday(%lld): %s", (long long)*t, strerror(errno));
    else
        logf(kI, "clock restored: rtc %lld + offset %lld = %lld (was %lld)", (long long)rtc, (long long)*off,
             (long long)*t, (long long)now);
}

// timerfd on CLOCK_REALTIME with CANCEL_ON_SET: read() fails with ECANCELED whenever the clock is set
int armWatch() {
    int fd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC);
    if (fd < 0) return -1;
    struct itimerspec its;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = time_t(kMaxUtcS) * 2;   // never expires
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, nullptr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

}  // namespace

int main(int argc, char** argv) {
    Opts o;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--restore-only") o.restoreOnly = true;
        else if (a == "--dry-run") o.dryRun = true;
        else if (a == "--min-utc") o.minUtc = strtoll(next().c_str(), nullptr, 10);   // unexpanded/empty -> 0
        else if (a == "--file") o.file = next();
        else if (a == "--rtc-dev") o.rtcDev = next();
        else if (a == "--rtc-file") o.rtcFile = next();
        else {
            fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    auto off = loadOffset(o.file);
    restore(o, off);
    if (o.restoreOnly) return 0;
    signal(SIGPIPE, SIG_IGN);
    for (;;) {
        int fd = armWatch();
        if (fd < 0) {
            logf(kE, "timerfd: %s", strerror(errno));
            sleep(60);
            continue;
        }
        // Arm first, then sample: a change between the two is either seen now or cancels the next read.
        int64_t rtc = 0;
        if (readRtc(o, &rtc)) {
            if (auto n = offsetToStore(rtc, realtimeS(), off, o.minUtc)) {
                if (storeOffset(o.file, *n)) {
                    logf(kI, "offset stored: %lld (was %s)", (long long)*n, off ? std::to_string(*off).c_str() : "none");
                    off = *n;
                } else {
                    logf(kE, "store %s: %s", o.file.c_str(), strerror(errno));
                }
            }
        } else {
            logf(kW, "RTC unreadable (%s)", strerror(errno));
        }
        uint64_t exp;
        ssize_t r = read(fd, &exp, sizeof exp);   // blocks until the wall clock is set (ECANCELED)
        int e = errno;
        close(fd);
        if (r < 0 && e != ECANCELED && e != EINTR) {
            logf(kE, "timerfd read: %s", strerror(e));
            sleep(10);
        }
    }
}
