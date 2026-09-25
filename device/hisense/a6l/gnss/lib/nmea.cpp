// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): NMEA synthesis.
#include "nmea.h"

#include <cmath>
#include <cstdio>
#include <ctime>

namespace a6l {
namespace nmea {

uint8_t checksum(const std::string& body) {
    uint8_t c = 0;
    for (char ch : body) c ^= uint8_t(ch);
    return c;
}

std::string finish(const std::string& body) {
    char tail[8];
    snprintf(tail, sizeof(tail), "*%02X", checksum(body));
    return "$" + body + tail;
}

bool valid(const std::string& s) {
    if (s.size() < 4 || s[0] != '$') return false;
    size_t star = s.rfind('*');
    if (star == std::string::npos || star + 3 != s.size()) return false;
    unsigned v = 0;
    if (sscanf(s.c_str() + star + 1, "%2X", &v) != 1) return false;
    return checksum(s.substr(1, star - 1)) == v;
}

static void utcParts(uint64_t ms, struct tm* tm, int* centis) {
    time_t t = time_t(ms / 1000);
    gmtime_r(&t, tm);
    *centis = int((ms % 1000) / 10);
}

static std::string coord(double v, bool lat) {
    char hemi = lat ? (v < 0 ? 'S' : 'N') : (v < 0 ? 'W' : 'E');
    double a = std::fabs(v);
    int deg = int(a);
    double min = (a - deg) * 60.0;
    if (min >= 59.99995) {   // avoid "60.0000" after rounding
        deg += 1;
        min = 0;
    }
    char buf[32];
    if (lat)
        snprintf(buf, sizeof(buf), "%02d%07.4f,%c", deg, min, hemi);
    else
        snprintf(buf, sizeof(buf), "%03d%07.4f,%c", deg, min, hemi);
    return buf;
}

std::string gga(const loc::Fix& f) {
    if (!f.hasLatLon || !f.hasUtc) return "";
    struct tm tm;
    int cs;
    utcParts(f.utcMs, &tm, &cs);
    char body[160];
    float alt = f.hasAltMsl ? f.altMsl : (f.hasAltEllipsoid ? f.altEllipsoid : 0.f);
    float sep = (f.hasAltMsl && f.hasAltEllipsoid) ? f.altEllipsoid - f.altMsl : 0.f;
    snprintf(body, sizeof(body), "GPGGA,%02d%02d%02d.%02d,%s,%s,1,%02zu,%.1f,%.1f,M,%.1f,M,,", tm.tm_hour, tm.tm_min,
             tm.tm_sec, cs, coord(f.latitude, true).c_str(), coord(f.longitude, false).c_str(), f.svUsed.size(),
             f.hasDop ? f.hdop : 0.f, alt, sep);
    return finish(body);
}

std::string rmc(const loc::Fix& f) {
    if (!f.hasLatLon || !f.hasUtc) return "";
    struct tm tm;
    int cs;
    utcParts(f.utcMs, &tm, &cs);
    char body[160];
    double knots = f.hasSpeed ? f.speedHorizontal * 1.943844 : 0.0;
    char course[16] = "";
    if (f.hasHeading) snprintf(course, sizeof(course), "%.1f", f.heading);
    snprintf(body, sizeof(body), "GPRMC,%02d%02d%02d.%02d,A,%s,%s,%.2f,%s,%02d%02d%02d,,,A", tm.tm_hour, tm.tm_min, tm.tm_sec,
             cs, coord(f.latitude, true).c_str(), coord(f.longitude, false).c_str(), knots, course, tm.tm_mday,
             tm.tm_mon + 1, tm.tm_year % 100);
    return finish(body);
}

}  // namespace nmea
}  // namespace a6l
