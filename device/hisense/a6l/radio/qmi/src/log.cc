// SPDX-License-Identifier: Apache-2.0
#include <a6lqmi/log.h>

#include <cstdarg>
#include <ctime>

#if defined(__ANDROID__) && !defined(A6L_QMI_NO_LIBLOG)
#include <android/log.h>
#endif

namespace a6l::qmi {

int gLogLevel = 2;

void logWrite(int prio, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
#if defined(__ANDROID__) && !defined(A6L_QMI_NO_LIBLOG)
    static const int map[] = {ANDROID_LOG_ERROR, ANDROID_LOG_WARN, ANDROID_LOG_INFO,
                              ANDROID_LOG_DEBUG, ANDROID_LOG_VERBOSE};
    __android_log_vprint(map[prio < 0 ? 0 : prio > 4 ? 4 : prio], "a6l-qmi", fmt, ap);
#else
    static const char* tag[] = {"E", "W", "I", "D", "V"};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    fprintf(stderr, "[%5ld.%03ld] %s ", static_cast<long>(ts.tv_sec), ts.tv_nsec / 1000000,
            tag[prio < 0 ? 0 : prio > 4 ? 4 : prio]);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
#endif
    va_end(ap);
}

}  // namespace a6l::qmi
