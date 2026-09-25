// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC -> android.hardware.gnss value mapping, kept free of AIDL types so it
// is unit-tested on the host. The HAL copies these plain structs into GnssLocation / IGnssCallback::GnssSvInfo.
#pragma once

#include <cstdint>
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

// fallbackUtcMs is used when the report carries no UTC timestamp (should not happen for SUCCESS reports).
AndroidLocation toAndroidLocation(const loc::Fix& f, int64_t fallbackUtcMs);
// Satellites without a valid id/system are dropped; USED_IN_FIX from the last position report's used list.
std::vector<AndroidSv> toAndroidSvs(const std::vector<loc::Sv>& svs, const std::vector<uint16_t>& usedIds);

}  // namespace a6l
