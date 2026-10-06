// SPDX-License-Identifier: Apache-2.0
// Host tests for a6l_timekeep (r5 bug hunt round2 R1). Build + run: see run-timekeep-tests.sh.
#include <cstdio>

#include "timekeep.h"

using namespace a6l::timekeep;
static int gFail = 0, gPass = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (c) {                                                         \
            gPass++;                                                     \
        } else {                                                         \
            gFail++;                                                     \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

int main() {
    const int64_t build = 1790000000;    // 2026-09-21
    const int64_t rtc = 36000000;        // raw PMIC counter (~1971)
    const int64_t off = 1790500000 - rtc;
    // file format
    CHECK(parseOffset(formatOffset(off)) == off);
    CHECK(parseOffset(formatOffset(-5)) == -5);
    CHECK(!parseOffset(""));
    CHECK(!parseOffset("a6l_rtc_offset v1 \n"));
    CHECK(!parseOffset("a6l_rtc_offset v1 12x\n"));
    CHECK(!parseOffset("a6l_rtc_offset v1 -\n"));
    CHECK(!parseOffset("a6l_rtc_offset v2 12\n"));
    CHECK(!parseOffset("a6l_rtc_offset v1 99999999999999999999\n"));
    CHECK(!parseOffset("a6l_rtc_offset v1 1754000000"));   // truncated write (no newline)
    CHECK(!parseOffset("a6l_rtc_offset v1 1754000000\n\n"));
    CHECK(parseOffset("a6l_rtc_offset v1 1754000000\n") == 1754000000);
    // boot restore: clock = raw RTC (HCTOSYS, not set yet) -> rtc + offset
    CHECK(restoreTarget(rtc, rtc, off, build) == rtc + off);
    CHECK(restoreTarget(rtc, rtc + 1, off, build) == rtc + off);
    // clock before the build date (e.g. no HCTOSYS) also counts as unset
    CHECK(restoreTarget(rtc, 0, off, build) == rtc + off);
    // already set this boot (network/NITZ/user or daemon restart): untouched
    CHECK(!restoreTarget(rtc, 1790600000, off, build));
    // no/unusable offset, target before the build date or absurd
    CHECK(!restoreTarget(rtc, rtc, std::nullopt, build));
    CHECK(!restoreTarget(rtc, rtc, int64_t(1000), build));
    CHECK(!restoreTarget(rtc, rtc, kMaxUtcS, build));
    CHECK(!restoreTarget(-1, 0, off, build));
    // store after a clock change
    CHECK(offsetToStore(rtc, 1790500000, std::nullopt, build) == 1790500000 - rtc);
    CHECK(!offsetToStore(rtc, 1790500001, off, build));              // unchanged within tolerance
    CHECK(offsetToStore(rtc, 1790500100, off, build) == off + 100);  // user/network change
    CHECK(offsetToStore(rtc, 1790400000, off, build) == off - 100000);   // set backwards, still after the build
    CHECK(!offsetToStore(rtc, build, std::nullopt, build));          // AlarmManager's build-date clamp
    CHECK(!offsetToStore(rtc, build + 30, std::nullopt, build));
    CHECK(!offsetToStore(rtc, 1000, std::nullopt, build));           // 1970-ish clock
    CHECK(!offsetToStore(rtc, kMaxUtcS + 1, std::nullopt, build));
    printf("A6L_TIMEKEEP_TESTS %s pass=%d fail=%d\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    return gFail ? 1 : 0;
}
