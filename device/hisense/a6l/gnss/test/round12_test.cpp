// SPDX-License-Identifier: Apache-2.0
// A6L GNSS, r5 review round 12 F66 (28 Sep 2026, docs/hardware-review-round12-20260928.md): position uncertainty is
// normalized to Android's 68% accuracy convention using the modem's confidence TLVs (0x16 horConfidence, 0x1D
// vertConfidence). Wire codec -> production parser -> production Android mapper, no modem.
// Build: g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -I../lib round12_test.cpp ../lib/*.cpp -o r12 && ./r12
#include <cmath>
#include <cstdio>
#include <limits>

#include "android_map.h"
#include "loc_v02.h"
#include "qmi.h"

using namespace a6l;

static int gFail = 0, gPass = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        if (c) {                                                                  \
            gPass++;                                                              \
        } else {                                                                  \
            gFail++;                                                              \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
        }                                                                         \
    } while (0)
static bool near(double a, double b, double tol = 1e-3) { return std::fabs(a - b) <= tol; }

// Reviewer fixture: 10 m horizontal / 20 m vertical, ellipsoid altitude, optional confidences; encode + decode + map.
static AndroidLocation run(int horConf, int vertConf, float hUnc = 10.f, float vUnc = 20.f, bool withAlt = true,
                           bool assumed = false) {
    loc::Fix f;
    f.status = loc::kStatusSuccess;
    f.sessionId = 1;
    f.hasLatLon = true;
    f.latitude = 48.85;
    f.longitude = 2.29;
    f.hasHorUnc = true;
    f.horUncCircular = hUnc;
    f.hasAltEllipsoid = withAlt;
    f.altEllipsoid = 80.f;
    f.altitudeAssumed = assumed;
    f.hasVertUnc = true;
    f.vertUnc = vUnc;
    if (horConf >= 0) {
        f.hasHorConfidence = true;
        f.horConfidence = uint8_t(horConf);
    }
    if (vertConf >= 0) {
        f.hasVertConfidence = true;
        f.vertConfidence = uint8_t(vertConf);
    }
    auto bytes = qmi::encode(loc::makePositionInd(f));
    qmi::Message m;
    std::string err;
    loc::Fix d;
    CHECK(qmi::decode(bytes.data(), bytes.size(), &m, &err) && loc::parsePosition(m, &d));
    CHECK(d.hasHorConfidence == (horConf >= 0) && d.hasVertConfidence == (vertConf >= 0));
    if (horConf >= 0) CHECK(d.horConfidence == horConf);
    if (vertConf >= 0) CHECK(d.vertConfidence == vertConf);
    d.altitudeAssumed = assumed;  // TLV 0x2D is not produced by makePositionInd
    return toAndroidLocation(d, 0);
}

int main() {
    // Horizontal (Rayleigh): R68 = R * sqrt(ln 0.32 / ln(1 - p)); vertical (normal): R68 = R * z(0.68) / z(p)
    const double z68 = 0.994457883;  // two-sided normal quantile of 0.68
    struct Case {
        int pct;
        double h, v;
    } cases[] = {
        {39, 10 * std::sqrt(std::log(0.32) / std::log(0.61)), 20 * z68 / 0.510073},   // 1-sigma 2-D ellipse level
        {50, 10 * std::sqrt(std::log(0.32) / std::log(0.50)), 20 * z68 / 0.674490},
        {68, 10.0, 20.0},                                                               // positive control: unchanged
        {95, 10 * std::sqrt(std::log(0.32) / std::log(0.05)), 20 * z68 / 1.959964},
    };
    for (auto& c : cases) {
        auto a = run(c.pct, c.pct);
        CHECK((a.flags & kHasHorizontalAccuracy) && (a.flags & kHasVerticalAccuracy));
        CHECK(near(a.horizontalAccuracyMeters, c.h) && near(a.verticalAccuracyMeters, c.v));
        printf("F66 confidence=%d%% horizontal=%.3f m vertical=%.3f m\n", c.pct, a.horizontalAccuracyMeters,
               a.verticalAccuracyMeters);
    }
    // below 68% never becomes more optimistic, above 68% shrinks: monotonic in the confidence
    CHECK(run(39, 39).horizontalAccuracyMeters > 10 && run(95, 95).horizontalAccuracyMeters < 10);
    CHECK(near(run(39, -1).horizontalAccuracyMeters, 15.183, 2e-3));  // spot value: 10 m at 39% -> 15.18 m at 68%
    CHECK(near(run(-1, 95).verticalAccuracyMeters, 10.148, 2e-3));    // 20 m at 95% -> 10.15 m at 68%
    // independent horizontal / vertical confidence: one model each, no shared multiplier
    {
        auto a = run(39, 95);
        CHECK(near(a.horizontalAccuracyMeters, cases[0].h) && near(a.verticalAccuracyMeters, cases[3].v));
        auto b = run(95, 39);
        CHECK(near(b.horizontalAccuracyMeters, cases[3].h) && near(b.verticalAccuracyMeters, cases[0].v));
    }
    // omitted / invalid confidence: taken as 68% (Qualcomm LocApiV02 convention), not a guessed other level
    for (int bad : {-1, 0, 100, 255}) {
        auto a = run(bad, bad);
        CHECK(near(a.horizontalAccuracyMeters, 10) && near(a.verticalAccuracyMeters, 20));
        CHECK((a.flags & kHasHorizontalAccuracy) && (a.flags & kHasVerticalAccuracy));
    }
    // zero / negative / non-finite uncertainty: no accuracy claim at all
    const float inf = std::numeric_limits<float>::infinity(), nan = std::numeric_limits<float>::quiet_NaN();
    for (float u : {0.f, -3.f, inf, nan}) {
        auto a = run(50, 50, u, u);
        CHECK(!(a.flags & kHasHorizontalAccuracy) && !(a.flags & kHasVerticalAccuracy));
    }
    // altitude absent / assumed: no vertical accuracy, horizontal still normalized
    for (bool assumed : {false, true}) {
        auto a = run(50, 50, 10, 20, !assumed ? false : true, assumed);
        CHECK(!(a.flags & kHasAltitude) && !(a.flags & kHasVerticalAccuracy));
        CHECK((a.flags & kHasHorizontalAccuracy) && near(a.horizontalAccuracyMeters, cases[1].h));
    }
    // reviewer control: no uncertainty TLVs -> no accuracy flags
    {
        loc::Fix f;
        f.status = loc::kStatusSuccess;
        f.hasLatLon = true;
        f.hasHorConfidence = f.hasVertConfidence = true;
        f.horConfidence = f.vertConfidence = 50;
        auto a = toAndroidLocation(f, 0);
        CHECK(!(a.flags & kHasHorizontalAccuracy) && !(a.flags & kHasVerticalAccuracy));
    }
    // helpers directly
    CHECK(near(*horizontalAccuracy68(10, true, 68), 10) && near(*verticalAccuracy68(20, true, 68), 20));
    CHECK(!horizontalAccuracy68(nan, true, 50) && !verticalAccuracy68(-1, false, 0));
    printf("A6L_GNSS_ROUND12_TESTS %s pass=%d fail=%d\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    return gFail ? 1 : 0;
}
