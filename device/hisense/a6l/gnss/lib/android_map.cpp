// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC -> Android mapping.
#include "android_map.h"

#include <cmath>

namespace a6l {

AndroidLocation toAndroidLocation(const loc::Fix& f, int64_t fallbackUtcMs) {
    AndroidLocation l;
    if (f.hasLatLon) {
        l.flags |= kHasLatLong;
        l.latitudeDegrees = f.latitude;
        l.longitudeDegrees = f.longitude;
    }
    // Android wants the WGS84 ellipsoid altitude; fall back to MSL only if that is all the modem gave.
    if (f.hasAltEllipsoid && !f.altitudeAssumed) {
        l.flags |= kHasAltitude;
        l.altitudeMeters = f.altEllipsoid;
    } else if (f.hasAltMsl && !f.altitudeAssumed) {
        l.flags |= kHasAltitude;
        l.altitudeMeters = f.altMsl;
    }
    if (f.hasSpeed && std::isfinite(f.speedHorizontal) && f.speedHorizontal >= 0) {
        l.flags |= kHasSpeed;
        l.speedMetersPerSec = f.speedHorizontal;
    }
    if (f.hasHeading && std::isfinite(f.heading)) {
        double b = std::fmod(double(f.heading), 360.0);
        if (b < 0) b += 360.0;
        l.flags |= kHasBearing;
        l.bearingDegrees = b;
    }
    if (f.hasHorUnc && f.horUncCircular > 0) {
        l.flags |= kHasHorizontalAccuracy;
        l.horizontalAccuracyMeters = f.horUncCircular;
    }
    if (f.hasVertUnc && f.vertUnc > 0 && (l.flags & kHasAltitude)) {
        l.flags |= kHasVerticalAccuracy;
        l.verticalAccuracyMeters = f.vertUnc;
    }
    if (f.hasSpeedUnc && f.speedUnc > 0 && (l.flags & kHasSpeed)) {
        l.flags |= kHasSpeedAccuracy;
        l.speedAccuracyMetersPerSecond = f.speedUnc;
    }
    if (f.hasHeadingUnc && f.headingUnc > 0 && (l.flags & kHasBearing)) {
        l.flags |= kHasBearingAccuracy;
        l.bearingAccuracyDegrees = f.headingUnc;
    }
    l.timestampMillis = f.hasUtc ? int64_t(f.utcMs) : fallbackUtcMs;
    return l;
}

std::vector<AndroidSv> toAndroidSvs(const std::vector<loc::Sv>& svs, const std::vector<uint16_t>& usedIds) {
    std::vector<AndroidSv> out;
    for (const auto& s : svs) {
        if (!(s.valid & loc::kSvValidSystem) || !(s.valid & loc::kSvValidId)) continue;
        auto a = loc::toAndroid(s.system, s.svId);
        if (a.constellation == 0 || a.svid <= 0) continue;
        AndroidSv v;
        v.svid = a.svid;
        v.constellation = a.constellation;
        v.cN0DbHz = (s.valid & loc::kSvValidSnr) && s.snr > 0 ? s.snr : 0.f;
        v.basebandCN0DbHz = v.cN0DbHz > 0 ? v.cN0DbHz - 1.0f : 0.f;   // not reported by QMI; conservative estimate
        v.elevationDegrees = (s.valid & loc::kSvValidElevation) ? s.elevation : 0.f;
        v.azimuthDegrees = (s.valid & loc::kSvValidAzimuth) ? s.azimuth : 0.f;
        v.carrierFrequencyHz = loc::carrierHz(a.constellation);
        v.svFlag = kSvHasCarrierFrequency;
        if (s.valid & loc::kSvValidInfoMask) {
            if (s.infoMask & loc::kSvHasEphemeris) v.svFlag |= kSvHasEphemeris;
            if (s.infoMask & loc::kSvHasAlmanac) v.svFlag |= kSvHasAlmanac;
        }
        for (auto id : usedIds) {
            auto u = loc::toAndroidFromUsedId(id);
            if (u.constellation == a.constellation && u.svid == a.svid) {
                v.svFlag |= kSvUsedInFix;
                break;
            }
        }
        out.push_back(v);
    }
    return out;
}

}  // namespace a6l
