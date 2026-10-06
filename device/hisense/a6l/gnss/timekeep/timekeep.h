// SPDX-License-Identifier: Apache-2.0
// A6L RTC offset keeper (r5 bug hunt round2 gnss-time R1, 29 Sep 2026).
//
// Problem: the PM660 RTC (rtc-pm8xxx, pm660.dtsi rtc@6000) has neither `allow-set-time` nor an offset nvmem cell /
// UEFI variable, so every RTC write fails (pm8xxx_rtc_update_offset -> -ENODEV): Android's SystemClockTime writes
// RTC_SET_TIME to /dev/rtc0 and only logs the failure (ALOGV). The raw PMIC counter is read-only for the AP (stock uses
// Qualcomm's time_daemon, which keeps an offset). After every reboot CLOCK_REALTIME therefore restarts from the raw
// counter (CONFIG_RTC_HCTOSYS), i.e. a wrong wall clock (AlarmManager then clamps it to the build date) until network
// or NITZ time arrives - never in airplane mode / without SIM or Wi-Fi.
//
// Fix: keep offset = wall clock - RTC in /data/vendor/a6l_time/rtc_offset. At boot (post-fs-data, before zygote) the
// clock is set to RTC + offset if nobody set it yet this boot (clock still equals the raw RTC). Afterwards every clock
// change (network time, NITZ, GNSS-assisted, user) is caught with a CLOCK_REALTIME timerfd (TFD_TIMER_CANCEL_ON_SET)
// and the new offset stored (atomically). Values before the build date are never stored nor restored.
// Pure decision logic here (host-tested in timekeep_test.cpp); I/O in a6l_timekeep.cpp.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>

namespace a6l::timekeep {

constexpr int64_t kSetToleranceS = 2;          // |clock - raw RTC| <= this: the clock was not set this boot
constexpr int64_t kMaxOffsetS = 200LL * 365 * 86400;   // sanity bound (the raw counter starts near 1970)
constexpr int64_t kMaxUtcS = 4102444800LL;     // 2100-01-01: later wall clocks are not believed
// AlarmManagerService clamps a clock older than the build (max(ro.build.date.utc, /system mtime, Build.TIME)) to that
// value at boot: a clock within this margin of the build date is that clamp, not a real time, and is not stored.
constexpr int64_t kClampMarginS = 60;

// File format: "a6l_rtc_offset v1 <offset seconds>\n" (signed decimal, newline required). Anything else = no offset.
inline std::string formatOffset(int64_t off) { return "a6l_rtc_offset v1 " + std::to_string(off) + "\n"; }

inline std::optional<int64_t> parseOffset(const std::string& s) {
    static const std::string kHead = "a6l_rtc_offset v1 ";
    if (s.compare(0, kHead.size(), kHead) != 0) return std::nullopt;
    // the trailing newline is mandatory: a truncated file ("...v1 17") must not parse as a smaller offset
    if (s.empty() || s.back() != '\n') return std::nullopt;
    std::string num = s.substr(kHead.size(), s.size() - kHead.size() - 1);
    if (num.empty() || num.size() > 20) return std::nullopt;
    size_t i = (num[0] == '-') ? 1 : 0;
    if (i == num.size()) return std::nullopt;
    for (size_t k = i; k < num.size(); k++)
        if (num[k] < '0' || num[k] > '9') return std::nullopt;
    errno = 0;
    char* end = nullptr;
    long long v = strtoll(num.c_str(), &end, 10);
    if (errno != 0 || !end || *end != '\0') return std::nullopt;
    if (v > kMaxOffsetS || v < -kMaxOffsetS) return std::nullopt;
    return int64_t(v);
}

// Boot restore: wall clock to set, or nullopt (nothing to do / unusable). realtimeS = CLOCK_REALTIME now, rtcS = RTC now.
inline std::optional<int64_t> restoreTarget(int64_t rtcS, int64_t realtimeS, std::optional<int64_t> offset,
                                            int64_t minUtcS) {
    if (!offset || rtcS < 0) return std::nullopt;
    const int64_t d = realtimeS - rtcS;
    const bool unset = (d <= kSetToleranceS && d >= -kSetToleranceS) || realtimeS < minUtcS;
    if (!unset) return std::nullopt;   // already set this boot (network/NITZ/user, or a daemon restart)
    const int64_t t = rtcS + *offset;
    if (t < minUtcS || t > kMaxUtcS) return std::nullopt;                // stale/corrupt offset
    if (t - realtimeS <= kSetToleranceS && realtimeS - t <= kSetToleranceS) return std::nullopt;   // already right
    return t;
}

// After a clock change: offset to store, or nullopt (implausible clock, or unchanged within tolerance).
inline std::optional<int64_t> offsetToStore(int64_t rtcS, int64_t realtimeS, std::optional<int64_t> stored,
                                            int64_t minUtcS) {
    if (rtcS < 0 || realtimeS < minUtcS + kClampMarginS || realtimeS > kMaxUtcS) return std::nullopt;
    const int64_t off = realtimeS - rtcS;
    if (off > kMaxOffsetS || off < -kMaxOffsetS) return std::nullopt;
    if (stored && off - *stored <= kSetToleranceS && *stored - off <= kSetToleranceS) return std::nullopt;
    return off;
}

}  // namespace a6l::timekeep
