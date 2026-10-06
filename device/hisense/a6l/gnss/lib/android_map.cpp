// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC -> Android mapping.
#include "android_map.h"

#include <cmath>

namespace a6l {

namespace {
constexpr double kAndroidConfidence = 0.68;
bool validConfidence(bool has, uint8_t pct) { return has && pct >= 1 && pct <= 99; }
// z such that erf(z / sqrt 2) = p (two-sided normal quantile), by bisection: monotonic, no library erfinv needed.
double twoSidedZ(double p) {
    double lo = 0, hi = 10;
    for (int i = 0; i < 80; i++) {
        double mid = (lo + hi) / 2;
        (std::erf(mid / std::sqrt(2.0)) < p ? lo : hi) = mid;
    }
    return (lo + hi) / 2;
}
}  // namespace

std::optional<double> horizontalAccuracy68(float unc, bool hasConfidence, uint8_t pct) {
    if (!std::isfinite(unc) || unc <= 0) return std::nullopt;
    if (!validConfidence(hasConfidence, pct)) return double(unc);
    const double p = pct / 100.0;
    return unc * std::sqrt(std::log(1 - kAndroidConfidence) / std::log(1 - p));
}

std::optional<double> verticalAccuracy68(float unc, bool hasConfidence, uint8_t pct) {
    if (!std::isfinite(unc) || unc <= 0) return std::nullopt;
    if (!validConfidence(hasConfidence, pct)) return double(unc);
    return unc * twoSidedZ(kAndroidConfidence) / twoSidedZ(pct / 100.0);
}

AndroidLocation toAndroidLocation(const loc::Fix& f, int64_t fallbackUtcMs) {
    AndroidLocation l;
    if (f.hasLatLon) {
        l.flags |= kHasLatLong;
        l.latitudeDegrees = f.latitude;
        l.longitudeDegrees = f.longitude;
    }
    // GnssLocation.altitudeMeters is height above the WGS84 ellipsoid (QMI TLV 0x1A altitudeWrtEllipsoid).
    // r5 review F22 (28 Sep 2026): an MSL-only report (TLV 0x1B) is a different datum and gnss-V7 has no MSL field,
    // so HAS_ALTITUDE (and therefore vertical accuracy) is omitted rather than reporting MSL as ellipsoid height.
    if (f.hasAltEllipsoid && !f.altitudeAssumed && std::isfinite(f.altEllipsoid)) {
        l.flags |= kHasAltitude;
        l.altitudeMeters = f.altEllipsoid;
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
    // r5 round12 F66: normalized to 68% confidence (horizontal radial and vertical 1-D models differ)
    if (auto h = f.hasHorUnc ? horizontalAccuracy68(f.horUncCircular, f.hasHorConfidence, f.horConfidence)
                             : std::nullopt) {
        l.flags |= kHasHorizontalAccuracy;
        l.horizontalAccuracyMeters = *h;
    }
    if (auto v = f.hasVertUnc && (l.flags & kHasAltitude)
                     ? verticalAccuracy68(f.vertUnc, f.hasVertConfidence, f.vertConfidence)
                     : std::nullopt) {
        l.flags |= kHasVerticalAccuracy;
        l.verticalAccuracyMeters = *v;
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
