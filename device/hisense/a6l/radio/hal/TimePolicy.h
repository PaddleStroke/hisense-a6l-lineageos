// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL, r5 review round8 F61 (28 Sep 2026): NITZ age. IRadioNetworkIndication.nitzTimeReceived takes
// receivedTimeMs = when the HAL sends the indication (elapsedRealtime) and ageMs = how long the sample was cached in
// the RIL/modem, both on the suspend-inclusive boot clock; Android's NitzSignal uses receivedTimeMs - ageMs as the
// sample's reference time. ModemCore stamps the QMI indication with bootTimeMs() (CLOCK_BOOTTIME, the clock behind
// android::elapsedRealtime) on the QMI dispatch thread; the HAL forwards it later from the worker queue.
// Host-tested in tests/review7_tests.cc.
#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <optional>
#include <string>

namespace android::hardware::radio::a6l {

inline int64_t bootTimeMs() {
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Age to report for a sample stamped at receivedBootMs and sent at nowBootMs (same clock). No modem-provided age is
// known for QMI NAS Network Time (the modem sends it as it decodes the network message), so the RIL queue time is the
// age. An invalid stamp (missing, or later than now) is discarded (nullopt) rather than sent with age 0.
inline std::optional<int64_t> nitzAgeMs(int64_t receivedBootMs, int64_t nowBootMs) {
    if (receivedBootMs <= 0 || receivedBootMs > nowBootMs) return std::nullopt;
    return nowBootMs - receivedBootMs;
}

// r5 round11 F65 (28 Sep 2026): QMI NAS Network Time -> Android NITZ string "yy/mm/dd,hh:mm:ss(+/-)tz[,dt]" (tz =
// local offset incl. DST in quarter hours, dt = DST adjustment in hours; NitzData.parse). The modem sends the time
// zone (TLV 0x10, int8 quarter hours) and the DST adjustment (TLV 0x11, 0..2 hours) as separate optional TLVs.
// Missing metadata must stay unknown, never become a factual zero:
//  - DST absent or out of range -> the ",dt" component is omitted (NitzData: DST adjustment unknown/null)
//  - time zone absent or outside UTC-12..UTC+14 -> nullopt: no NITZ is sent (IRadioNetwork NITZ has no time-only form;
//    Android keeps NTP/GNSS time), rather than manufacturing an offset of +0 or borrowing an older one.
inline std::optional<std::string> formatNitz(unsigned year, unsigned month, unsigned day, unsigned hour, unsigned minute,
                                             unsigned second, std::optional<int> tzQuarterHours,
                                             std::optional<int> dstHours) {
    if (!tzQuarterHours || *tzQuarterHours < -48 || *tzQuarterHours > 56) return std::nullopt;
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 60) return std::nullopt;
    const int tz = *tzQuarterHours;
    char buf[64];
    int n = snprintf(buf, sizeof buf, "%02u/%02u/%02u,%02u:%02u:%02u%c%d", year % 100, month, day, hour, minute, second,
                     tz < 0 ? '-' : '+', tz < 0 ? -tz : tz);
    std::string out(buf, n > 0 ? static_cast<size_t>(n) : 0);
    if (dstHours && *dstHours >= 0 && *dstHours <= 2) out += "," + std::to_string(*dstHours);
    return out;
}

}  // namespace android::hardware::radio::a6l
