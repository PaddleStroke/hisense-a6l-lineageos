// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): tiny logging shim (liblog on Android, stderr on host / static CLI).
#pragma once

#include <cstdio>

namespace a6l::qmi {
// 0 = errors, 1 = +warnings, 2 = +info (default), 3 = +debug, 4 = +verbose (every QMI message)
extern int gLogLevel;
void logWrite(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
}  // namespace a6l::qmi

#define A6L_QLOG(lvl, ...)                                                                 \
    do {                                                                                   \
        if (::a6l::qmi::gLogLevel >= (lvl)) ::a6l::qmi::logWrite((lvl), __VA_ARGS__);      \
    } while (0)
#define ALOGE_Q(...) A6L_QLOG(0, __VA_ARGS__)
#define ALOGW_Q(...) A6L_QLOG(1, __VA_ARGS__)
#define ALOGI_Q(...) A6L_QLOG(2, __VA_ARGS__)
#define ALOGD_Q(...) A6L_QLOG(3, __VA_ARGS__)
#define ALOGV_Q(...) A6L_QLOG(4, __VA_ARGS__)
