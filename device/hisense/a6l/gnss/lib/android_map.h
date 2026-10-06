// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC -> android.hardware.gnss value mapping, kept free of AIDL types so it
// is unit-tested on the host. The HAL copies these plain structs into GnssLocation / IGnssCallback::GnssSvInfo.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "loc_v02.h"

namespace a6l {

struct AndroidLocation {
    int flags = 0;   // GnssLocation::HAS_* bits
    double latitudeDegrees = 0, longitudeDegrees = 0, altitudeMeters = 0, speedMetersPerSec = 0, bearingDegrees = 0;
    double horizontalAccuracyMeters = 0, verticalAccuracyMeters = 0, speedAccuracyMetersPerSecond = 0,
           bearingAccuracyDegrees = 0;
    int64_t timestampMillis = 0;
};

struct AndroidSv {
    int svid = 0;
    int constellation = 0;   // GnssConstellationType
    float cN0DbHz = 0, basebandCN0DbHz = 0, elevationDegrees = 0, azimuthDegrees = 0;
    double carrierFrequencyHz = 0;
    int svFlag = 0;   // IGnssCallback::GnssSvFlags
};

enum : int {
    kHasLatLong = 0x0001, kHasAltitude = 0x0002, kHasSpeed = 0x0004, kHasBearing = 0x0008,
    kHasHorizontalAccuracy = 0x0010, kHasVerticalAccuracy = 0x0020, kHasSpeedAccuracy = 0x0040,
    kHasBearingAccuracy = 0x0080,
};
enum : int { kSvHasEphemeris = 1, kSvHasAlmanac = 2, kSvUsedInFix = 4, kSvHasCarrierFrequency = 8 };

// r5 review F10 (28 Sep 2026): IGnssCallback capabilities the HAL advertises (static_assert'ed against the AIDL values
// in hal/Gnss.cpp). Only what the QMI LOC client implements. SATELLITE_BLOCKLIST is NOT advertised: loc_v02 has no
// constellation-control / blacklist-SV request, so blocklisted satellites could not be excluded from the solution.
enum : int { kCapScheduling = 1 << 0, kCapSatelliteBlocklist = 1 << 9 };
constexpr int kHalCapabilities = kCapScheduling;
constexpr bool kModemSatelliteExclusion = false;

// r5 round12 F66 (28 Sep 2026): GnssLocation.horizontalAccuracyMeters / verticalAccuracyMeters are 68%-confidence
// values; QMI LOC reports uncertainty with its own confidence (horConfidence / vertConfidence, percent 1..99).
// Horizontal: circular 2-D Gaussian error (Rayleigh radius), P(r <= R) = 1 - exp(-R^2 / 2 sigma^2), so
//   R68 = R_p * sqrt(ln(1 - 0.68) / ln(1 - p)).
// Vertical: 1-D Gaussian error, two-sided, P(|e| <= R) = erf(R / (sigma sqrt 2)), so R68 = R_p * z(0.68) / z(p).
// Confidence absent, 0 or > 99 (not a valid percentage): the uncertainty is taken as already at 68%, the convention
// of Qualcomm's own LocApiV02 (it copies horUncCircular / vertUnc into the Android accuracy unchanged). Non-finite or
// non-positive uncertainty -> no accuracy (nullopt).
std::optional<double> horizontalAccuracy68(float uncMeters, bool hasConfidence, uint8_t confidencePct);
std::optional<double> verticalAccuracy68(float uncMeters, bool hasConfidence, uint8_t confidencePct);

// fallbackUtcMs is used when the report carries no UTC timestamp (should not happen for SUCCESS reports).
AndroidLocation toAndroidLocation(const loc::Fix& f, int64_t fallbackUtcMs);
// Satellites without a valid id/system are dropped; USED_IN_FIX from the last position report's used list.
std::vector<AndroidSv> toAndroidSvs(const std::vector<loc::Sv>& svs, const std::vector<uint16_t>& usedIds);

}  // namespace a6l
