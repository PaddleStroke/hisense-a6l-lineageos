// SPDX-License-Identifier: Apache-2.0
// A6L GNSS, r5 bug hunt round2 gnss-time (29 Sep 2026): engine regressions found offline, production engine against the
// in-process fake modem (no phone).
//  G1  a stop requested while an XTRA transfer is still QUEUED behind other work must be served inside that transfer
//      (the worker loop cleared the stop flag when it dequeued the transfer -> STOP only after the whole transfer)
//  G2  XTRA handed over while the LOC service is absent (late modem start / modem restart) is kept and injected when
//      the service comes up, also when the service is lost in the middle of a transfer (it used to be dropped)
//  G4  non-finite / out-of-range latitude-longitude in a position report is not a fix (parsing bounds)
// Build: g++ -std=c++17 -O1 -g -pthread -fsanitize=address,undefined -I../lib bughunt2_test.cpp ../lib/*.cpp -o bh2 && ./bh2
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#include <cmath>
#include <limits>

#include "android_map.h"
#include "fake_modem.h"
#include "loc_client.h"
#include "loc_v02.h"
#include "nmea.h"

using namespace a6l;

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

struct XRec : EngineListener {
    std::mutex mu;
    std::vector<std::string> info;
    std::atomic<int> done{0}, results{0};
    std::string detail;
    void onXtraResult(bool ok, const std::string& d) override {
        std::lock_guard<std::mutex> l(mu);
        detail = d;
        done = ok ? 1 : 2;
        results++;
    }
    void onXtraInfo(const std::string& d) override {
        std::lock_guard<std::mutex> l(mu);
        info.push_back(d);
    }
};

static std::vector<uint8_t> xtraFile() {
    std::vector<uint8_t> f(2500);
    for (size_t i = 0; i < f.size(); i++) f[i] = uint8_t(i * 7 + 1);
    return f;
}

// source/validity always answered; parts confirmed only while `confirm` is set
static void modemXtra(FakeModem& f, const qmi::Message& r, bool confirm) {
    qmi::Message ind;
    ind.type = qmi::kIndication;
    ind.msgId = r.msgId;
    if (r.msgId == loc::kGetPredictedOrbitsSource || r.msgId == loc::kGetPredictedOrbitsValidity) {
        ind.add(0x01, qmi::Writer().u32(0));
        f.inject(ind);
    } else if (r.msgId == loc::kInjectPredictedOrbits && confirm) {
        uint16_t n = 0;
        r.getU16(0x03, &n);
        ind.add(0x01, qmi::Writer().u32(0));
        ind.add(0x10, qmi::Writer().u16(n));
        f.inject(ind);
    }
}

static int64_t ms() { return GnssEngine::bootMs(); }

static void testG1StopQueuedBehindXtra() {
    auto fake = std::make_shared<FakeModem>();
    fake->onRequest = [](FakeModem& f, const qmi::Message& r) {   // runs on the engine worker
        if (r.msgId == loc::kInjectUtcTime) std::this_thread::sleep_for(std::chrono::milliseconds(400));
        modemXtra(f, r, false);   // parts never confirmed: each attempt lasts xtraPartTimeoutMs
    };
    XRec rec;
    EngineConfig cfg;
    cfg.xtraPartTimeoutMs = 3000;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
    eng.begin();
    CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
    eng.setActive(true);
    CHECK(fake->waitFor([&] { return eng.sessionRunning(); }, 2000));
    eng.injectTime(1800000000000ull, 10);   // the worker is busy ~400 ms in this request
    CHECK(fake->waitFor([&] { return fake->count(loc::kInjectUtcTime) == 1; }, 2000));
    eng.injectXtra(xtraFile());             // queued
    int64_t t0 = ms();
    eng.setActive(false);                   // queued behind the transfer
    bool stopped = fake->waitFor([&] { return fake->count(loc::kStop) == 1; }, 1500);
    printf("  G1: STOP %s %lld ms after stop() (XTRA part deadline %d ms)\n", stopped ? "sent" : "NOT sent",
           (long long)(ms() - t0), cfg.xtraPartTimeoutMs);
    CHECK(stopped);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 6000));   // the transfer itself still ends normally
    CHECK(fake->count(loc::kStop) == 1 && !eng.sessionRunning());
    eng.end();
}

static void testG2DeferredUntilServiceUp() {
    auto fake = std::make_shared<FakeModem>();
    fake->serviceAtOpen = false;   // AF_QIPCRTR ok, LOC not registered yet (modem stack still starting)
    fake->onRequest = [](FakeModem& f, const qmi::Message& r) { modemXtra(f, r, true); };
    XRec rec;
    EngineConfig cfg;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
    eng.begin();
    CHECK(fake->waitFor([&] { std::lock_guard<std::mutex> l(fake->mu); return fake->opens >= 1; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!eng.serviceUp());
    eng.injectXtra(xtraFile());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    printf("  G2: before service up: result=%d parts_sent=%zu\n", rec.done.load(),
           fake->count(loc::kInjectPredictedOrbits));
    CHECK(rec.done == 0);   // not dropped with "LOC service not up"
    CHECK(fake->count(loc::kInjectPredictedOrbits) == 0);
    fake->serverEvent(true);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 5000));
    CHECK(rec.done == 1 && rec.results == 1);
    CHECK(fake->count(loc::kInjectPredictedOrbits) == 3);
    eng.end();
}

static void testG2ServiceLostMidTransfer() {
    auto fake = std::make_shared<FakeModem>();
    std::atomic<bool> confirm{false};
    fake->onRequest = [&confirm](FakeModem& f, const qmi::Message& r) { modemXtra(f, r, confirm); };
    XRec rec;
    EngineConfig cfg;
    cfg.xtraPartTimeoutMs = 800;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
    eng.begin();
    CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
    eng.injectXtra(xtraFile());
    CHECK(fake->waitFor([&] { return fake->count(loc::kInjectPredictedOrbits) == 1; }, 2000));
    fake->serverEvent(false);   // modem restart while part 1 waits for its confirmation
    CHECK(fake->waitFor([&] { return !eng.serviceUp(); }, 1000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));   // part deadline passed
    printf("  G2: after mid-transfer loss: result=%d\n", rec.done.load());
    CHECK(rec.done == 0);
    confirm = true;
    fake->serverEvent(true);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 5000));
    CHECK(rec.done == 1 && rec.results == 1);
    CHECK(fake->count(loc::kInjectPredictedOrbits) == 1 + 3);
    eng.end();
}

// G4: a position report whose latitude/longitude TLVs are non-finite or out of range must not become a fix
// (HAS_LAT_LONG with NaN / 91 deg to the framework, UB float->int conversion in the NMEA synthesis).
static void testG4InvalidCoordinates() {
    const double nan = std::numeric_limits<double>::quiet_NaN(), inf = std::numeric_limits<double>::infinity();
    const double bad[][2] = {{nan, 2.0}, {48.0, nan}, {inf, 2.0}, {48.0, -inf}, {90.5, 2.0}, {-91.0, 2.0},
                             {48.0, 180.5}, {48.0, -181.0}};
    for (auto& c : bad) {
        loc::Fix in;
        in.status = loc::kStatusSuccess;
        in.hasLatLon = true;
        in.latitude = c[0];
        in.longitude = c[1];
        in.hasUtc = true;
        in.utcMs = 1790000000000ull;
        loc::Fix out;
        CHECK(loc::parsePosition(loc::makePositionInd(in), &out));
        CHECK(!out.hasLatLon);
        CHECK(!(toAndroidLocation(out, 0).flags & kHasLatLong));
        CHECK(nmea::gga(out).empty() && nmea::rmc(out).empty());
    }
    const double good[][2] = {{90.0, 180.0}, {-90.0, -180.0}, {0.0, 0.0}, {48.85, 2.35}};
    for (auto& c : good) {
        loc::Fix in;
        in.status = loc::kStatusSuccess;
        in.hasLatLon = true;
        in.latitude = c[0];
        in.longitude = c[1];
        loc::Fix out;
        CHECK(loc::parsePosition(loc::makePositionInd(in), &out));
        CHECK(out.hasLatLon && out.latitude == c[0] && out.longitude == c[1]);
    }
}

int main() {
    testG4InvalidCoordinates();
    testG1StopQueuedBehindXtra();
    testG2DeferredUntilServiceUp();
    testG2ServiceLostMidTransfer();
    printf("A6L_GNSS_BUGHUNT2_TESTS %s pass=%d fail=%d\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    return gFail ? 1 : 0;
}
