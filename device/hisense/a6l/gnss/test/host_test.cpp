// SPDX-License-Identifier: Apache-2.0
// A6L GNSS host tests (agent gnss, 24 Sep 2026): codec, LOC parsers against hand-built wire bytes, NMEA, Android
// mapping, and the full client/engine state machine against the in-process fake modem.
// Build: g++ -std=c++17 -O1 -g -pthread -I../lib host_test.cpp ../lib/*.cpp -o host_test && ./host_test
#include <atomic>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include "android_map.h"
#include "fake_modem.h"
#include "loc_client.h"
#include "loc_v02.h"
#include "nmea.h"
#include "qmi.h"

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

static std::vector<uint8_t> H(const char* s) {
    std::vector<uint8_t> v;
    qmi::unhex(s, &v);
    return v;
}

static void testCodec() {
    qmi::Message m;
    m.type = qmi::kRequest;
    m.txn = 0x1234;
    m.msgId = loc::kStart;
    m.add(0x01, qmi::Writer().u8(1));
    m.add(0x13, qmi::Writer().u32(1000));
    auto b = qmi::encode(m);
    // 00 3412 2200 0b00 | 01 0100 01 | 13 0400 e8030000
    CHECK(qmi::hex(b.data(), b.size()) == "00341222000b0001010001130400e8030000");
    qmi::Message d;
    std::string err;
    CHECK(qmi::decode(b.data(), b.size(), &d, &err));
    uint32_t v = 0;
    CHECK(d.txn == 0x1234 && d.msgId == 0x22 && d.getU32(0x13, &v) && v == 1000);
    // malformed: TLV length beyond message
    auto bad = H("0401002400050001ff0000");
    CHECK(!qmi::decode(bad.data(), bad.size(), &d, &err));
    auto shortHdr = H("040100");
    CHECK(!qmi::decode(shortHdr.data(), shortHdr.size(), &d, &err));
    // response result TLV
    auto resp = H("0201002100070002040001001000");
    CHECK(qmi::decode(resp.data(), resp.size(), &d, &err));
    uint16_t r, e;
    CHECK(d.getResult(&r, &e) && r == 1 && e == 0x10);
}

// Hand-built QMI_LOC_EVENT_POSITION_REPORT_IND (independent of makePositionInd).
static void testPositionWire() {
    qmi::Writer body;
    auto tlv = [&](uint8_t t, const qmi::Writer& w) { body.u8(t).u16(uint16_t(w.b.size())).bytes(w.b.data(), w.b.size()); };
    tlv(0x01, qmi::Writer().u32(0));                              // SUCCESS
    tlv(0x02, qmi::Writer().u8(1));
    tlv(0x10, qmi::Writer().f64(48.8583701));
    tlv(0x11, qmi::Writer().f64(2.2944813));
    tlv(0x12, qmi::Writer().f32(4.5f));
    tlv(0x18, qmi::Writer().f32(1.25f));
    tlv(0x1A, qmi::Writer().f32(80.0f));
    tlv(0x1B, qmi::Writer().f32(35.0f));
    tlv(0x1C, qmi::Writer().f32(9.0f));
    tlv(0x20, qmi::Writer().f32(-90.0f));
    tlv(0x24, qmi::Writer().f32(1.8f).f32(0.9f).f32(1.5f));
    tlv(0x25, qmi::Writer().u64(1790236800123ull));              // 2026-09-24T08:00:00.123Z
    tlv(0x27, qmi::Writer().u16(2437).u32(374418123));
    tlv(0x2C, qmi::Writer().u8(3).u16(5).u16(70).u16(305));       // G05, R06, E05
    tlv(0x77, qmi::Writer().u32(0xdeadbeef));                     // unknown TLV must be ignored
    qmi::Writer pkt;
    pkt.u8(4).u16(7).u16(0x24).u16(uint16_t(body.b.size())).bytes(body.b.data(), body.b.size());
    qmi::Message m;
    std::string err;
    CHECK(qmi::decode(pkt.b.data(), pkt.b.size(), &m, &err));
    loc::Fix f;
    CHECK(loc::parsePosition(m, &f));
    CHECK(f.status == loc::kStatusSuccess && f.sessionId == 1 && f.hasLatLon);
    CHECK(std::fabs(f.latitude - 48.8583701) < 1e-9 && std::fabs(f.longitude - 2.2944813) < 1e-9);
    CHECK(f.hasHorUnc && f.horUncCircular == 4.5f && f.hasAltEllipsoid && f.altEllipsoid == 80.f);
    CHECK(f.hasDop && std::fabs(f.hdop - 0.9f) < 1e-6 && f.hasUtc && f.utcMs == 1790236800123ull);
    CHECK(f.hasGpsTime && f.gpsWeek == 2437 && f.gpsTowMs == 374418123);
    CHECK(f.svUsed.size() == 3 && f.svUsed[2] == 305);
    auto a = toAndroidLocation(f, 0);
    CHECK((a.flags & (kHasLatLong | kHasAltitude | kHasSpeed | kHasBearing | kHasHorizontalAccuracy |
                      kHasVerticalAccuracy)) ==
          (kHasLatLong | kHasAltitude | kHasSpeed | kHasBearing | kHasHorizontalAccuracy | kHasVerticalAccuracy));
    CHECK(a.bearingDegrees == 270.0 && a.altitudeMeters == 80.0 && a.timestampMillis == 1790236800123ll);
    CHECK(!(a.flags & kHasSpeedAccuracy));
    // symmetric encoder produces a parseable equivalent
    auto m2 = loc::makePositionInd(f);
    auto b2 = qmi::encode(m2);
    qmi::Message d2;
    loc::Fix f2;
    CHECK(qmi::decode(b2.data(), b2.size(), &d2, &err) && loc::parsePosition(d2, &f2));
    CHECK(f2.latitude == f.latitude && f2.svUsed == f.svUsed && f2.utcMs == f.utcMs);
    // IN_PROGRESS report without position
    qmi::Message ip;
    ip.type = 4;
    ip.msgId = 0x24;
    ip.add(0x01, qmi::Writer().u32(1));
    ip.add(0x02, qmi::Writer().u8(1));
    CHECK(loc::parsePosition(ip, &f) && f.status == loc::kStatusInProgress && !f.hasLatLon);
    // missing mandatory status TLV
    qmi::Message nost;
    nost.msgId = 0x24;
    CHECK(!loc::parsePosition(nost, &f));
}

static void testSvWire() {
    qmi::Writer list;
    list.u8(3);
    // GPS 12: all valid, tracked, eph, el 45 az 120 snr 38.5
    list.u32(0xff).u32(1).u16(12).u8(1).u32(3).u8(1).f32(45.f).f32(120.f).f32(38.5f);
    // GLONASS 70 (-> R06), search, no snr valid bit
    list.u32(0x7f).u32(5).u16(70).u8(1).u32(2).u8(0).f32(10.f).f32(300.f).f32(0.f);
    // Galileo 305 (-> E05) tracked
    list.u32(0xff).u32(2).u16(305).u8(1).u32(3).u8(3).f32(60.f).f32(20.f).f32(41.f);
    qmi::Message m;
    m.type = 4;
    m.msgId = 0x25;
    m.add(0x01, qmi::Writer().u8(0));
    m.add(0x10, list);
    CHECK(list.b.size() == 1 + 3 * loc::kSvInfoWireSize);
    std::vector<loc::Sv> svs;
    bool assumed = true;
    CHECK(loc::parseSvInfo(m, &svs, &assumed) && !assumed && svs.size() == 3);
    CHECK(svs[0].svId == 12 && svs[0].snr == 38.5f && svs[0].azimuth == 120.f && svs[0].status == 3);
    CHECK(svs[2].system == 2 && svs[2].infoMask == 3 && svs[2].snr == 41.f);
    auto a = toAndroidSvs(svs, {12, 305});
    CHECK(a.size() == 3);
    CHECK(a[0].constellation == 1 && a[0].svid == 12 && (a[0].svFlag & kSvUsedInFix) && (a[0].svFlag & kSvHasEphemeris));
    CHECK(a[1].constellation == 3 && a[1].svid == 6 && !(a[1].svFlag & kSvUsedInFix) && a[1].cN0DbHz == 0.f);
    CHECK(a[2].constellation == 6 && a[2].svid == 5 && (a[2].svFlag & kSvUsedInFix) &&
          (a[2].svFlag & kSvHasAlmanac));
    CHECK(std::fabs(a[1].carrierFrequencyHz - 1602e6) < 1);
    // truncated list must be rejected
    qmi::Message t = m;
    t.tlvs[1].value.resize(40);
    CHECK(!loc::parseSvInfo(t, &svs, &assumed));
    // mapping ranges
    CHECK(loc::toAndroid(loc::kSysSbas, 33).svid == 120 && loc::toAndroid(loc::kSysBds, 201).svid == 1);
    CHECK(loc::toAndroidFromUsedId(65).constellation == 3 && loc::toAndroidFromUsedId(65).svid == 1);
    CHECK(loc::toAndroidFromUsedId(195).constellation == 4);
}

static void testNmea() {
    CHECK(nmea::valid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47"));
    CHECK(!nmea::valid("$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*48"));
    CHECK(nmea::finish("GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,") ==
          "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47");
    loc::Fix f;
    f.status = 0;
    f.hasLatLon = true;
    f.latitude = 48.1173;
    f.longitude = -11.516666666;
    f.hasUtc = true;
    f.utcMs = 1790236800120ull;   // 08:00:00.12
    f.hasAltMsl = true;
    f.altMsl = 545.4f;
    f.hasAltEllipsoid = true;
    f.altEllipsoid = 592.3f;
    f.hasDop = true;
    f.hdop = 0.9f;
    f.svUsed = {1, 2, 3, 4, 5, 6, 7, 8};
    auto g = nmea::gga(f);
    CHECK(nmea::valid(g));
    CHECK(g.rfind("$GPGGA,080000.12,4807.0380,N,01131.0000,W,1,08,0.9,545.4,M,46.9,M,,*", 0) == 0);
    auto r = nmea::rmc(f);
    CHECK(nmea::valid(r) && r.find(",240926,") != std::string::npos && r.find(",A,4807.0380,N,") != std::string::npos);
    // NMEA indication splitting (two sentences in one indication, CRLF, trailing NUL)
    auto m = loc::makeNmeaInd("$GPGSA,A,3,,,,,,,,,,,,,,,*6E\r\n$GPVTG,,T,,M,,N,,K,N*2C\r\n");
    std::vector<std::string> s;
    CHECK(loc::parseNmea(m, &s) && s.size() == 2 && s[1] == "$GPVTG,,T,,M,,N,,K,N*2C");
    // expanded NMEA TLV 0x10 preferred
    std::string ex = "$GNGNS,1*00";
    m.add(0x10, std::vector<uint8_t>(ex.begin(), ex.end()));
    CHECK(loc::parseNmea(m, &s) && s.size() == 1 && s[0] == ex);
}

static void testRequests() {
    auto st = loc::makeStart(1, 2000, true);
    uint8_t sid;
    uint32_t v;
    CHECK(st.msgId == 0x22 && st.getU8(0x01, &sid) && sid == 1 && st.getU32(0x10, &v) && v == 1 &&
          st.getU32(0x12, &v) && v == 1 && st.getU32(0x13, &v) && v == 2000);
    auto t = loc::makeInjectUtcTime(1790236800000ull, 5000);
    uint64_t ms;
    CHECK(t.msgId == 0x38 && t.getU64(0x01, &ms) && ms == 1790236800000ull && t.getU32(0x02, &v) && v == 5000);
    auto e = loc::makeRegEvents(0x1c7);
    uint64_t mask;
    CHECK(e.msgId == 0x21 && e.getU64(0x01, &mask) && mask == 0x1c7);
    CHECK(loc::makeSetOperationMode(4).msgId == 0x4a && loc::makeSetNmeaTypes(1).msgId == 0x3e &&
          loc::makeDeleteAllAssistData().msgId == 0x44 && loc::makeSetEngineLock(1).msgId == 0x3a);
    // QRTR control packets
    QrtrCtrl c;
    CHECK(parseCtrl(makeLookup(16), &c) && c.cmd == kQrtrNewLookup && c.service == 16 && makeLookup(16).size() == 20);
}

struct Rec : EngineListener {
    std::mutex mu;
    std::vector<loc::Fix> fixes;
    std::vector<std::pair<std::string, bool>> nmea;
    std::vector<std::vector<uint16_t>> used;
    std::atomic<int> up{0}, down{0}, timeReq{0}, inter{0};
    void onFix(const loc::Fix& f) override { std::lock_guard<std::mutex> l(mu); fixes.push_back(f); }
    void onIntermediate(const loc::Fix&) override { inter++; }
    void onNmea(const std::string& s, bool syn) override { std::lock_guard<std::mutex> l(mu); nmea.emplace_back(s, syn); }
    void onSvs(const std::vector<loc::Sv>&, const std::vector<uint16_t>& u) override { std::lock_guard<std::mutex> l(mu); used.push_back(u); }
    void onServiceState(bool u) override { u ? up++ : down++; }
    void onTimeRequest() override { timeReq++; }
    size_t nFix() { std::lock_guard<std::mutex> l(mu); return fixes.size(); }
    size_t nNmea() { std::lock_guard<std::mutex> l(mu); return nmea.size(); }
};

static loc::Fix sampleFix(uint64_t utc) {
    loc::Fix f;
    f.status = 0;
    f.sessionId = 1;
    f.hasLatLon = true;
    f.latitude = 45.0;
    f.longitude = 5.0;
    f.hasHorUnc = true;
    f.horUncCircular = 8;
    f.hasUtc = true;
    f.utcMs = utc;
    f.svUsed = {3, 7};
    return f;
}

static void testEngine() {
    auto fake = std::make_shared<FakeModem>();
    fake->failOpen = true;   // first: no AF_QIPCRTR (e.g. qrtr module not loaded yet)
    Rec rec;
    EngineConfig cfg;
    cfg.reconnectMs = 100;
    std::vector<std::string> logs;
    std::mutex lm;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg,
                   [&](int, const std::string& s) { std::lock_guard<std::mutex> l(lm); logs.push_back(s); });
    eng.begin();
    eng.setActive(true);
    CHECK(fake->waitFor([&] { std::lock_guard<std::mutex> l(fake->mu); return fake->opens >= 2; }, 2000));
    CHECK(fake->count(loc::kStart) == 0);
    {
        std::lock_guard<std::mutex> l(fake->mu);
        fake->failOpen = false;
    }
    CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 1; }, 3000));
    CHECK(eng.sessionRunning());
    {
        std::lock_guard<std::mutex> l(fake->mu);
        std::vector<uint16_t> order;
        for (auto& r : fake->requests) order.push_back(r.msgId);
        CHECK((order == std::vector<uint16_t>{0x20, 0x21, 0x4a, 0x3e, 0x22}));
        uint32_t mode;
        CHECK(fake->requests[2].getU32(0x01, &mode) && mode == loc::kModeStandalone);
        uint64_t mask;
        CHECK(fake->requests[1].getU64(0x01, &mask) && (mask & 0x7) == 0x7);
    }
    // fix without modem NMEA -> synthetic GGA/RMC
    fake->inject(loc::makePositionInd(sampleFix(1790236800000ull)));
    CHECK(fake->waitFor([&] { return rec.nFix() == 1 && rec.nNmea() == 2; }, 2000));
    CHECK(rec.nmea.size() == 2 && rec.nmea[0].second && nmea::valid(rec.nmea[0].first));
    // SV report after the fix carries the used list
    loc::Sv s;
    s.valid = 0xff;
    s.system = 1;
    s.svId = 7;
    s.snr = 30;
    fake->inject(loc::makeSvInfoInd({s}));
    CHECK(fake->waitFor([&] { std::lock_guard<std::mutex> l(rec.mu); return rec.used.size() == 1; }, 2000));
    CHECK(rec.used[0] == (std::vector<uint16_t>{3, 7}));
    // modem NMEA suppresses synthesis
    fake->inject(loc::makeNmeaInd("$GPGGA,080001.00,4500.0000,N,00500.0000,E,1,02,1.0,0.0,M,0.0,M,,*6A\r\n"));
    CHECK(fake->waitFor([&] { return rec.nNmea() == 3; }, 2000));
    fake->inject(loc::makePositionInd(sampleFix(1790236801000ull)));
    CHECK(fake->waitFor([&] { return rec.nFix() == 2; }, 2000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(rec.nNmea() == 3);
    // failure status is not a fix; IN_PROGRESS is intermediate
    auto bad = sampleFix(1);
    bad.status = loc::kStatusTimeout;
    fake->inject(loc::makePositionInd(bad));
    bad.status = loc::kStatusInProgress;
    fake->inject(loc::makePositionInd(bad));
    CHECK(fake->waitFor([&] { return rec.inter == 1; }, 2000));
    CHECK(rec.nFix() == 2);
    // time injection request + injection
    qmi::Message tr;
    tr.type = 4;
    tr.msgId = loc::kIndInjectTimeReq;
    fake->inject(tr);
    CHECK(fake->waitFor([&] { return rec.timeReq == 1; }, 2000));
    eng.injectTime(1790236802000ull, 3000);
    CHECK(fake->waitFor([&] { return fake->count(loc::kInjectUtcTime) == 1; }, 2000));
    // modem restart: DEL_SERVER then NEW_SERVER -> reconfigure + START again
    fake->serverEvent(false);
    CHECK(fake->waitFor([&] { return rec.down == 1; }, 2000));
    fake->serverEvent(true);
    CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 2 && fake->count(loc::kRegEvents) == 2; }, 3000));
    // interval change re-issues START with the new interval
    eng.setInterval(5000);
    CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 3; }, 2000));
    {
        std::lock_guard<std::mutex> l(fake->mu);
        uint32_t iv = 0;
        fake->requests.back().getU32(0x13, &iv);
        CHECK(iv == 5000);
    }
    // stop
    eng.setActive(false);
    CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 1; }, 2000));
    CHECK(!eng.sessionRunning());
    eng.end();
    CHECK(eng.fixCount() == 2);
}

static void testStartError() {
    auto fake = std::make_shared<FakeModem>();
    fake->errors[loc::kStart] = 0x19;   // e.g. QMI_ERR_NOT_SUPPORTED
    Rec rec;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, EngineConfig(),
                   nullptr);
    eng.begin();
    eng.setActive(true);
    CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 1; }, 3000));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!eng.sessionRunning() && eng.lastRequestError() == 0x19);
    eng.end();
}

static void testNoServiceTimeout() {
    auto fake = std::make_shared<FakeModem>();
    fake->serviceAtOpen = false;   // modem not booted: name service lists nothing
    auto t = std::unique_ptr<Transport>(new FakeModemTransport(fake));
    LocClient c(std::move(t), nullptr);
    std::string err;
    CHECK(c.open(&err));
    CHECK(!c.waitService(200));
    CHECK(c.lookupDone());
    CHECK(c.request(loc::makeStop(1)) == -2);
    fake->serverEvent(true);   // modem comes up later
    CHECK(c.waitService(2000));
    CHECK(c.request(loc::makeStop(1)) == 0);
    c.close();
}

struct XRec : EngineListener {
    std::mutex mu;
    std::vector<std::string> info;
    std::atomic<int> done{0};
    std::string detail;
    void onXtraResult(bool ok, const std::string& d) override {
        std::lock_guard<std::mutex> l(mu);
        detail = d;
        done = ok ? 1 : 2;
    }
    void onXtraInfo(const std::string& d) override {
        std::lock_guard<std::mutex> l(mu);
        info.push_back(d);
    }
};

static void testXtra() {
    // wire format of one part
    std::vector<uint8_t> file(2500);
    for (size_t i = 0; i < file.size(); i++) file[i] = uint8_t(i * 7 + 1);
    auto parts = loc::buildXtraParts(file);
    CHECK(parts.size() == 3);
    uint32_t tot = 0;
    uint16_t np = 0, pn = 0;
    CHECK(parts[2].getU32(0x01, &tot) && tot == 2500);
    CHECK(parts[2].getU16(0x02, &np) && np == 3);
    CHECK(parts[2].getU16(0x03, &pn) && pn == 3);
    auto* d = parts[2].find(0x04);
    CHECK(d && d->size() == 2 + 452 && (*d)[0] == (452 & 0xff) && (*d)[1] == (452 >> 8) && (*d)[2] == file[2048]);
    CHECK(parts[0].find(0x04)->size() == 2 + 1024);
    uint32_t fmt = 9;
    CHECK(parts[0].getU32(0x10, &fmt) && fmt == 0);
    CHECK(loc::buildXtraParts(file, 1024, false)[0].find(0x10) == nullptr);
    std::string why;
    CHECK(!loc::looksLikeXtra(std::vector<uint8_t>(110, '<'), &why));
    CHECK(loc::looksLikeXtra(file, &why));
    auto cp = loc::makeInjectCoarsePosition(48.85, 2.35, 3000, 1);
    uint32_t src = 0;
    CHECK(cp.getU32(0x1D, &src) && src == 1 && cp.find(0x10) && cp.find(0x14));

    // engine flow against the fake modem: one indication per part, then source + validity queries
    auto fake = std::make_shared<FakeModem>();
    std::atomic<int> badPart{0};
    fake->onRequest = [&](FakeModem& f, const qmi::Message& r) {
        qmi::Message ind;
        ind.type = qmi::kIndication;
        ind.msgId = r.msgId;
        if (r.msgId == loc::kInjectPredictedOrbits) {
            uint16_t n = 0;
            r.getU16(0x03, &n);
            ind.add(0x01, qmi::Writer().u32(badPart == n ? 2 : 0));
            ind.add(0x10, qmi::Writer().u16(n));
        } else if (r.msgId == loc::kGetPredictedOrbitsSource) {
            ind.add(0x01, qmi::Writer().u32(0));
            ind.add(0x10, qmi::Writer().u32(307200).u32(1024));
            std::string u = "https://path1.xtracloud.net/xtra3grc.bin";
            qmi::Writer w;
            w.u8(1).u8(uint8_t(u.size())).bytes(u.data(), u.size());
            ind.add(0x11, w);
        } else if (r.msgId == loc::kGetPredictedOrbitsValidity) {
            ind.add(0x01, qmi::Writer().u32(0));
            ind.add(0x10, qmi::Writer().u64(1442700000ull).u16(168));
        } else {
            return;
        }
        f.inject(ind);
    };
    XRec rec;
    EngineConfig cfg;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
    eng.begin();
    CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
    eng.injectXtra(file);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 5000));
    CHECK(rec.done == 1);
    CHECK(fake->count(loc::kInjectPredictedOrbits) == 3);
    CHECK(fake->waitFor([&] { std::lock_guard<std::mutex> l(rec.mu); return rec.info.size() == 2; }, 3000));
    {
        std::lock_guard<std::mutex> l(rec.mu);
        CHECK(rec.info.size() == 2 && rec.info[0].find("server=https://path1.xtracloud.net/xtra3grc.bin") != std::string::npos);
        CHECK(rec.info.size() == 2 && rec.info[0].find("max_part=1024") != std::string::npos);
        CHECK(rec.info.size() == 2 && rec.info[1].find("duration_h=168") != std::string::npos);
    }
    // a failing part stops the injection
    badPart = 2;
    rec.done = 0;
    eng.injectXtra(file);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 5000));
    CHECK(rec.done == 2);
    CHECK(fake->count(loc::kInjectPredictedOrbits) == 5);
    eng.end();
}

// misc2 (25 Sep 2026): byte-exact wire image vs the stock struct layout, and the error handling fixed after the
// 25 Sep attended run (request error 3 = QMI_ERR_INTERNAL, 94 = QMI_ERR_NOT_SUPPORTED).
static void testXtraWireAndErrors() {
    std::vector<uint8_t> file(1500);
    for (size_t i = 0; i < file.size(); i++) file[i] = uint8_t(i ^ 0x5a);
    auto parts = loc::buildXtraParts(file);
    CHECK(parts.size() == 2);
    parts[0].txn = 0x1234;
    auto w = qmi::encode(parts[0]);
    // QMI header: type 0 (request), txn, msg 0x0035, payload length
    size_t tlvLen = (3 + 4) + (3 + 2) + (3 + 2) + (3 + 2 + 1024) + (3 + 4);
    CHECK(w.size() == 7 + tlvLen);
    CHECK(w[0] == 0x00 && w[1] == 0x34 && w[2] == 0x12 && w[3] == 0x35 && w[4] == 0x00);
    CHECK(w[5] == (tlvLen & 0xff) && w[6] == (tlvLen >> 8));
    const uint8_t hdr[] = {0x01, 0x04, 0x00, 0xdc, 0x05, 0x00, 0x00,   // totalSize 1500
                           0x02, 0x02, 0x00, 0x02, 0x00,               // totalParts 2
                           0x03, 0x02, 0x00, 0x01, 0x00,               // partNum 1
                           0x04, 0x02, 0x04, 0x00, 0x04};              // partData: TLV len 1026, u16 count 1024
    CHECK(w.size() > 7 + sizeof(hdr) && memcmp(w.data() + 7, hdr, sizeof(hdr)) == 0);
    CHECK(w[7 + sizeof(hdr)] == file[0] && w[7 + sizeof(hdr) + 1023] == file[1023]);
    const uint8_t tail[] = {0x10, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};  // formatType XTRA (stock: valid=1, 0)
    CHECK(memcmp(w.data() + w.size() - sizeof(tail), tail, sizeof(tail)) == 0);
    CHECK(std::string(loc::qmiErrorName(3)) == "INTERNAL" && std::string(loc::qmiErrorName(94)) == "NOT_SUPPORTED");
    CHECK(!loc::qmiErrorIsLayout(3) && !loc::qmiErrorIsLayout(94) && loc::qmiErrorIsLayout(1));

    auto fake = std::make_shared<FakeModem>();
    std::atomic<int> failFirst{0};
    fake->onRequest = [&](FakeModem& f, const qmi::Message& r) {
        qmi::Message ind;
        ind.type = qmi::kIndication;
        ind.msgId = r.msgId;
        if (r.msgId == loc::kInjectPredictedOrbits) {
            {
                std::lock_guard<std::mutex> lk(f.mu);
                if (f.errors.count(r.msgId)) {
                    if (failFirst > 0 && --failFirst == 0) f.errors.erase(r.msgId);
                    return;   // error response: no indication
                }
            }
            uint16_t n = 0;
            r.getU16(0x03, &n);
            ind.add(0x01, qmi::Writer().u32(0));
            ind.add(0x10, qmi::Writer().u16(n));
        } else if (r.msgId == loc::kGetPredictedOrbitsSource) {
            ind.add(0x01, qmi::Writer().u32(0));
            ind.add(0x10, qmi::Writer().u32(307200).u32(512));   // modem asks for smaller parts
        } else if (r.msgId == loc::kGetPredictedOrbitsValidity) {
            ind.add(0x01, qmi::Writer().u32(0));
            ind.add(0x10, qmi::Writer().u64(1442700000ull).u16(168));
        } else {
            return;
        }
        f.inject(ind);
    };
    XRec rec;
    EngineConfig cfg;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
    eng.begin();
    CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
    // 1) INTERNAL once, then OK: retried WITH formatType, in modem-sized parts (512 -> 3 parts)
    {
        std::lock_guard<std::mutex> lk(fake->mu);
        fake->errors[loc::kInjectPredictedOrbits] = 3;
    }
    failFirst = 1;
    eng.injectXtra(file);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 8000));
    CHECK(rec.done == 1);
    CHECK(fake->count(loc::kInjectPredictedOrbits) == 1 + 3);
    {
        std::lock_guard<std::mutex> lk(fake->mu);
        bool allFmt = true;
        for (auto& r : fake->requests)
            if (r.msgId == loc::kInjectPredictedOrbits && !r.find(0x10)) allFmt = false;
        CHECK(allFmt);
    }
    // 2) INTERNAL always: 3 attempts, never without formatType, FAILED with the error name
    rec.done = 0;
    {
        std::lock_guard<std::mutex> lk(fake->mu);
        fake->errors[loc::kInjectPredictedOrbits] = 3;
    }
    failFirst = 0;
    size_t before = fake->count(loc::kInjectPredictedOrbits);
    eng.injectXtra(file);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 12000));
    CHECK(rec.done == 2);
    CHECK(fake->count(loc::kInjectPredictedOrbits) == before + 3);
    {
        std::lock_guard<std::mutex> lk(fake->mu);
        bool allFmt = true;
        for (auto& r : fake->requests)
            if (r.msgId == loc::kInjectPredictedOrbits && !r.find(0x10)) allFmt = false;
        CHECK(allFmt);
    }
    // 3) MALFORMED (layout) once: retried WITHOUT formatType
    rec.done = 0;
    {
        std::lock_guard<std::mutex> lk(fake->mu);
        fake->errors[loc::kInjectPredictedOrbits] = 1;
    }
    failFirst = 1;
    before = fake->count(loc::kInjectPredictedOrbits);
    eng.injectXtra(file);
    CHECK(fake->waitFor([&] { return rec.done != 0; }, 8000));
    CHECK(rec.done == 1);
    {
        std::lock_guard<std::mutex> lk(fake->mu);
        const qmi::Message* last = nullptr;
        for (auto& r : fake->requests)
            if (r.msgId == loc::kInjectPredictedOrbits) last = &r;
        CHECK(last && !last->find(0x10));
    }
    CHECK(fake->count(loc::kInjectPredictedOrbits) == before + 1 + 3);
    eng.end();
}

// r5 review F10: no blocklist capability without a modem-side exclusion; USED_IN_FIX comes from the report only
static void testCapabilities() {
    CHECK(!(kHalCapabilities & kCapSatelliteBlocklist));
    CHECK(!kModemSatelliteExclusion);
    CHECK((kHalCapabilities & kCapScheduling) != 0);
    loc::Sv a, b;
    a.valid = loc::kSvValidSystem | loc::kSvValidId | loc::kSvValidSnr;
    a.system = loc::kSysGps;
    a.svId = 7;
    a.snr = 30;
    b = a;
    b.svId = 9;
    auto out = toAndroidSvs({a, b}, {7});
    CHECK(out.size() == 2);
    if (out.size() == 2) {
        CHECK((out[0].svFlag & kSvUsedInFix) != 0);
        CHECK((out[1].svFlag & kSvUsedInFix) == 0);
    }
}

// r5 review F22: GnssLocation.altitudeMeters is WGS84 ellipsoid height; MSL (TLV 0x1B) is never reported as such.
static void testAltitudeDatum() {
    auto base = sampleFix(1790236800000ull);
    base.hasVertUnc = true;
    base.vertUnc = 6;
    auto ell = base;   // ellipsoid only
    ell.hasAltEllipsoid = true;
    ell.altEllipsoid = 170;
    auto a = toAndroidLocation(ell, 0);
    CHECK((a.flags & kHasAltitude) && a.altitudeMeters == 170 && (a.flags & kHasVerticalAccuracy));
    auto msl = base;   // MSL only: omitted, lat/long still usable, no vertical accuracy
    msl.hasAltMsl = true;
    msl.altMsl = 123;
    a = toAndroidLocation(msl, 0);
    CHECK(!(a.flags & kHasAltitude) && !(a.flags & kHasVerticalAccuracy) && a.altitudeMeters == 0);
    CHECK((a.flags & kHasLatLong) && a.latitudeDegrees == 45.0 && (a.flags & kHasHorizontalAccuracy));
    auto both = ell;   // both: ellipsoid wins
    both.hasAltMsl = true;
    both.altMsl = 123;
    a = toAndroidLocation(both, 0);
    CHECK((a.flags & kHasAltitude) && a.altitudeMeters == 170);
    a = toAndroidLocation(base, 0);   // neither
    CHECK(!(a.flags & (kHasAltitude | kHasVerticalAccuracy)) && (a.flags & kHasLatLong));
    auto assumed = both;   // assumed altitude (TLV 0x2D) is not a measurement
    assumed.altitudeAssumed = true;
    a = toAndroidLocation(assumed, 0);
    CHECK(!(a.flags & (kHasAltitude | kHasVerticalAccuracy)));
    auto nan = ell;
    nan.altEllipsoid = NAN;
    CHECK(!(toAndroidLocation(nan, 0).flags & kHasAltitude));
    // wire: MSL-only report parses as MSL, maps without altitude; NMEA GGA still carries MSL
    auto w = loc::makePositionInd(msl);
    loc::Fix p;
    CHECK(loc::parsePosition(w, &p) && p.hasAltMsl && !p.hasAltEllipsoid && !(toAndroidLocation(p, 0).flags & kHasAltitude));
    CHECK(nmea::gga(msl).find(",123.0,M,") != std::string::npos);
    // r5 review F22 follow-up: synthetic GGA field 9 = MSL height only, field 11 = separation only when both are known
    auto ggaAlt = [](const loc::Fix& f) {   // returns "alt|sep" (fields 9 and 11)
        auto g = nmea::gga(f);
        std::vector<std::string> v;
        size_t a = 0, b;
        while ((b = g.find(',', a)) != std::string::npos) { v.push_back(g.substr(a, b - a)); a = b + 1; }
        return nmea::valid(g) && v.size() > 12 && v[10] == "M" && v[12] == "M" ? v[9] + "|" + v[11] : std::string("BAD");
    };
    CHECK(ggaAlt(ell) == "|");              // ellipsoid-only: no MSL invented from ellipsoid height, no zero separation
    CHECK(ggaAlt(base) == "|");             // neither
    CHECK(ggaAlt(msl) == "123.0|");         // MSL-only: height, separation unknown
    auto bothN = ell;
    bothN.altEllipsoid = 80;
    bothN.hasAltMsl = true;
    bothN.altMsl = 35;
    CHECK(ggaAlt(bothN) == "35.0|45.0");    // both: positive control
    auto zero = bothN;                      // genuinely known zeros are emitted
    zero.altEllipsoid = 0;
    zero.altMsl = 0;
    CHECK(ggaAlt(zero) == "0.0|0.0");
    CHECK(ggaAlt(assumed) == "|");          // assumed altitude (2D fix) is not a measurement
    auto nanMsl = bothN;
    nanMsl.altMsl = NAN;
    CHECK(ggaAlt(nanMsl) == "|");
    CHECK(ggaAlt(nan) == "|");
    CHECK(toAndroidLocation(ell, 0).altitudeMeters == 170 && nmea::valid(nmea::rmc(ell)));   // other outputs unchanged
}

// r5 review F21: transient configuration/START failures with LOC present are retried (capped backoff) without a
// location toggle; stop during backoff cancels the retry; unsupported optional commands do not block.
static void testTransientConfigStart() {
    EngineConfig cfg;
    cfg.retryMinMs = 200;
    cfg.retryMaxMs = 400;
    for (int mode = 0; mode < 3; mode++) {
        // 0: REG_EVENTS INTERNAL once, 1: START INTERNAL once, 2: SET_OPERATION_MODE INTERNAL once
        auto fake = std::make_shared<FakeModem>();
        uint16_t id = mode == 0 ? loc::kRegEvents : mode == 1 ? loc::kStart : loc::kSetOperationMode;
        fake->errors[id] = 3;
        Rec rec;
        GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
        eng.begin();
        CHECK(fake->waitFor([&] { return eng.serviceUp() && fake->count(mode == 1 ? uint16_t(loc::kSetNmeaTypes) : id) >= 1; }, 2000));
        eng.setActive(true);
        if (mode == 1) CHECK(fake->waitFor([&] { return fake->count(loc::kStart) >= 1; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        CHECK(!eng.sessionRunning());
        if (mode != 1) CHECK(fake->count(loc::kStart) == 0);   // no START on an unregistered/unconfigured client
        {
            std::lock_guard<std::mutex> l(fake->mu);
            fake->errors.clear();
        }
        CHECK(fake->waitFor([&] { return eng.sessionRunning(); }, 1500));
        CHECK(fake->count(loc::kStart) == (mode == 1 ? 2u : 1u));
        if (mode != 1) CHECK(fake->count(id) >= 2);
        // registration applied -> position delivery without toggling location
        fake->inject(loc::makePositionInd(sampleFix(1790236800000ull)));
        CHECK(fake->waitFor([&] { return rec.nFix() == 1; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK(fake->count(loc::kStart) == (mode == 1 ? 2u : 1u));   // no duplicate sessions
        eng.end();
    }
    // stop during backoff cancels the retry
    {
        auto fake = std::make_shared<FakeModem>();
        fake->errors[loc::kStart] = 3;
        EngineConfig c3 = cfg;
        c3.retryMinMs = 800;
        c3.retryMaxMs = 800;
        GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, nullptr, c3,
                       nullptr);
        eng.begin();
        CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
        eng.setActive(true);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStart) >= 1; }, 2000));
        eng.setActive(false);
        {
            std::lock_guard<std::mutex> l(fake->mu);
            fake->errors.clear();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        CHECK(fake->count(loc::kStart) == 1 && !eng.sessionRunning() && fake->count(loc::kStop) == 0);
        eng.end();
    }
    // permanent error: capped number of retries; NOT_SUPPORTED operation mode is optional
    {
        auto fake = std::make_shared<FakeModem>();
        fake->errors[loc::kStart] = 0x19;
        fake->errors[loc::kSetOperationMode] = 0x5E;
        EngineConfig c2 = cfg;
        c2.retryMinMs = 50;
        c2.retryMaxMs = 100;
        c2.maxStartRetries = 3;
        GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, nullptr, c2,
                       nullptr);
        eng.begin();
        CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
        eng.setActive(true);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 4; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        CHECK(fake->count(loc::kStart) == 4 && !eng.sessionRunning());
        eng.setActive(true);   // explicit start re-arms the retries
        CHECK(fake->waitFor([&] { return fake->count(loc::kStart) >= 5; }, 2000));
        eng.end();
    }
}

// r5 review F33: XTRA success only with a valid, correlated confirmation for every part.
static void testXtraConfirmations() {
    std::vector<uint8_t> file(2500);
    for (size_t i = 0; i < file.size(); i++) file[i] = uint8_t(i * 7 + 1);
    // mode: 0 valid, 1 wrong part (99), 2 empty indication, 3 missing indication for part 2, 4 explicit error part 3,
    //       5 stale + duplicate + malformed before each valid one, 6 error status without part number
    for (int mode = 0; mode < 7; mode++) {
        auto fake = std::make_shared<FakeModem>();
        fake->onRequest = [mode](FakeModem& f, const qmi::Message& r) {
            qmi::Message ind;
            ind.type = qmi::kIndication;
            ind.msgId = r.msgId;
            if (r.msgId == loc::kGetPredictedOrbitsSource || r.msgId == loc::kGetPredictedOrbitsValidity) {
                ind.add(0x01, qmi::Writer().u32(0));
                f.inject(ind);
                return;
            }
            if (r.msgId != loc::kInjectPredictedOrbits) return;
            uint16_t n = 0;
            r.getU16(0x03, &n);
            auto mk = [&](uint32_t st, int pn) {
                qmi::Message m;
                m.type = qmi::kIndication;
                m.msgId = loc::kInjectPredictedOrbits;
                m.add(0x01, qmi::Writer().u32(st));
                if (pn >= 0) m.add(0x10, qmi::Writer().u16(uint16_t(pn)));
                return m;
            };
            if (mode == 1) f.inject(mk(0, 99));
            else if (mode == 2) f.inject(ind);
            else if (mode == 3 && n == 2) return;
            else if (mode == 4 && n == 3) f.inject(mk(2, 3));
            else if (mode == 5) {
                if (n > 1) f.inject(mk(0, n - 1));   // duplicate of the previous part
                f.inject(mk(0, n + 1));              // out of order
                f.inject(ind);                       // malformed
                f.inject(mk(0, n));
            } else if (mode == 6 && n == 2) f.inject(mk(4, -1));
            else f.inject(mk(0, n));
        };
        XRec rec;
        EngineConfig cfg;
        cfg.xtraPartTimeoutMs = 300;
        GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
        eng.begin();
        CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
        eng.injectXtra(file);
        CHECK(fake->waitFor([&] { return rec.done != 0; }, 3000));
        bool wantOk = mode == 0 || mode == 5;
        CHECK(rec.done == (wantOk ? 1 : 2));
        size_t wantParts = (mode == 1 || mode == 2) ? 1 : (mode == 3 || mode == 6) ? 2 : 3;   // stops at the bad part
        CHECK(fake->count(loc::kInjectPredictedOrbits) == wantParts);
        {
            std::lock_guard<std::mutex> l(rec.mu);
            if (wantOk) CHECK(rec.detail.find("all parts acknowledged") != std::string::npos);
            if (mode == 1 || mode == 2 || mode == 3) CHECK(rec.detail.find("no valid confirmation") != std::string::npos);
        }
        eng.end();
    }
}

// r5 review F34: a stop is not queued behind an assistance transfer; the transfer itself continues.
// r5 review round6 (29 Sep 2026): F52 rejected STOP, F53 single fix, F54 pending deletion
static uint32_t lastStartRecurrence(FakeModem& f) {
    std::lock_guard<std::mutex> l(f.mu);
    uint32_t v = 0;
    for (auto& r : f.requests)
        if (r.msgId == loc::kStart) r.getU32(0x10, &v);
    return v;
}
static int firstIndex(FakeModem& f, uint16_t id, size_t from = 0) {
    std::lock_guard<std::mutex> l(f.mu);
    for (size_t i = from; i < f.requests.size(); i++)
        if (f.requests[i].msgId == id) return int(i);
    return -1;
}
static qmi::Message sessionStateInd(uint32_t v) {
    qmi::Message m;
    m.type = qmi::kIndication;
    m.msgId = loc::kIndFixSessionState;
    m.add(0x01, qmi::Writer().u32(v));
    return m;
}
static void testRound6() {
    auto mk = [](std::shared_ptr<FakeModem> fake, int retryMin, Rec* rec) {
        EngineConfig cfg;
        cfg.retryMinMs = retryMin;
        cfg.retryMaxMs = retryMin * 2;
        return std::make_unique<GnssEngine>([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); },
                                            rec, cfg, nullptr);
    };
    auto setErr = [](FakeModem& f, uint16_t id, uint16_t e) {
        std::lock_guard<std::mutex> l(f.mu);
        if (e) f.errors[id] = e; else f.errors.erase(id);
    };
    {   // F52: rejected STOP keeps the session, recovers by bounded retry, no extra STOP afterwards
        auto fake = std::make_shared<FakeModem>();
        Rec rec;
        auto eng = mk(fake, 50, &rec);
        eng->begin();
        CHECK(fake->waitFor([&] { return eng->serviceUp() && fake->count(loc::kSetNmeaTypes) >= 1; }, 2000));
        eng->setActive(true);
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 2000));
        setErr(*fake, loc::kStop, 3);
        eng->setActive(false);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStop) >= 1; }, 1000));
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        CHECK(eng->sessionRunning());   // not applied: still running (or unknown)
        setErr(*fake, loc::kStop, 0);
        CHECK(fake->waitFor([&] { return !eng->sessionRunning(); }, 1500));
        size_t n = fake->count(loc::kStop);
        CHECK(n >= 2);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(fake->count(loc::kStop) == n && fake->count(loc::kStart) == 1);
        eng->end();
        CHECK(fake->count(loc::kStop) == n);   // nothing left to clean up
    }
    {   // F52: rejected STOP, then shutdown before the retry: cleanup STOP is sent
        auto fake = std::make_shared<FakeModem>();
        Rec rec;
        auto eng = mk(fake, 5000, &rec);
        eng->begin();
        CHECK(fake->waitFor([&] { return eng->serviceUp() && fake->count(loc::kSetNmeaTypes) >= 1; }, 2000));
        eng->setActive(true);
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 2000));
        setErr(*fake, loc::kStop, 3);
        eng->setActive(false);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 1; }, 1000));
        setErr(*fake, loc::kStop, 0);
        eng->end();
        CHECK(fake->count(loc::kStop) == 2);
    }
    {   // F52: stop (rejected) then start again while the retry is pending: stays running, no START/STOP churn
        auto fake = std::make_shared<FakeModem>();
        Rec rec;
        auto eng = mk(fake, 50, &rec);
        eng->begin();
        CHECK(fake->waitFor([&] { return eng->serviceUp() && fake->count(loc::kSetNmeaTypes) >= 1; }, 2000));
        eng->setActive(true);
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 2000));
        setErr(*fake, loc::kStop, 3);
        eng->setActive(false);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 1; }, 1000));
        eng->setActive(true);
        setErr(*fake, loc::kStop, 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(eng->sessionRunning() && fake->count(loc::kStop) == 1 && fake->count(loc::kStart) == 1);
        // F52 related: the modem reports our session finished while it is wanted -> restarted (capped retry)
        fake->inject(sessionStateInd(loc::kSessionFinished));
        CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 2; }, 1500));
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 500));
        // a finished indication after an accepted stop changes nothing
        eng->setActive(false);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 2 && !eng->sessionRunning(); }, 1000));
        fake->inject(sessionStateInd(loc::kSessionFinished));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        CHECK(fake->count(loc::kStart) == 2 && fake->count(loc::kStop) == 2);
        eng->end();
    }
    {   // F52: a malformed-request answer is not retried blindly
        auto fake = std::make_shared<FakeModem>();
        Rec rec;
        auto eng = mk(fake, 50, &rec);
        eng->begin();
        CHECK(fake->waitFor([&] { return eng->serviceUp() && fake->count(loc::kSetNmeaTypes) >= 1; }, 2000));
        eng->setActive(true);
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 2000));
        setErr(*fake, loc::kStop, 0x30);
        eng->setActive(false);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(fake->count(loc::kStop) == 1 && eng->sessionRunning());
        eng->end();
    }
    {   // F53: single vs periodic recurrence on the wire; single completes after one fix and is not restarted
        auto fake = std::make_shared<FakeModem>();
        Rec rec;
        auto eng = mk(fake, 50, &rec);
        eng->begin();
        CHECK(fake->waitFor([&] { return eng->serviceUp() && fake->count(loc::kSetNmeaTypes) >= 1; }, 2000));
        eng->setPositionMode(1000, false);
        eng->setActive(true);
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 2000));
        CHECK(lastStartRecurrence(*fake) == loc::kRecurrencePeriodic);
        fake->inject(loc::makePositionInd(sampleFix(1790236800000ull)));
        fake->inject(loc::makePositionInd(sampleFix(1790236801000ull)));
        CHECK(fake->waitFor([&] { return rec.nFix() == 2; }, 1000));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(eng->sessionRunning() && fake->count(loc::kStop) == 0);   // periodic keeps running
        eng->setActive(false);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 1; }, 1000));
        eng->setPositionMode(1000, true);
        eng->setActive(true);
        CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 2; }, 1000));
        CHECK(lastStartRecurrence(*fake) == loc::kRecurrenceSingle);
        fake->inject(loc::makePositionInd(sampleFix(1790236802000ull)));
        CHECK(fake->waitFor([&] { return !eng->sessionRunning() && fake->count(loc::kStop) == 2; }, 1000));
        CHECK(rec.nFix() == 3);
        fake->serverEvent(false);   // service loss + return must not restart the completed single fix
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        fake->serverEvent(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        CHECK(fake->count(loc::kStart) == 2 && !eng->sessionRunning());
        eng->setActive(false);   // Android's stop after the fix: nothing to stop
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(fake->count(loc::kStop) == 2);
        eng->setActive(true);    // a new start request = a new single fix
        CHECK(fake->waitFor([&] { return fake->count(loc::kStart) == 3; }, 1000));
        CHECK(lastStartRecurrence(*fake) == loc::kRecurrenceSingle);
        eng->setActive(false);   // cancelled before a fix
        CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 3 && !eng->sessionRunning(); }, 1000));
        eng->end();
    }
    {   // F54: deletion requested while LOC is absent stays pending and precedes the next START; rejection is final
        auto fake = std::make_shared<FakeModem>();
        fake->serviceAtOpen = false;
        Rec rec;
        auto eng = mk(fake, 50, &rec);
        eng->begin();
        CHECK(fake->waitFor([&] { std::lock_guard<std::mutex> l(fake->mu); return fake->opens >= 1; }, 2000));
        eng->deleteAll();
        eng->setActive(true);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(fake->count(loc::kDeleteAssistData) == 0 && fake->count(loc::kStart) == 0 && eng->deletePending());
        fake->serverEvent(true);
        CHECK(fake->waitFor([&] { return eng->sessionRunning(); }, 2000));
        int d = firstIndex(*fake, loc::kDeleteAssistData), st = firstIndex(*fake, loc::kStart);
        CHECK(d >= 0 && st > d && !eng->deletePending());
        CHECK(fake->count(loc::kDeleteAssistData) == 1);
        setErr(*fake, loc::kDeleteAssistData, 3);   // rejected: reported, not retried, not blocking
        eng->deleteAll();
        CHECK(fake->waitFor([&] { return fake->count(loc::kDeleteAssistData) == 2; }, 1000));
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        CHECK(!eng->deletePending() && fake->count(loc::kDeleteAssistData) == 2);
        eng->end();
    }
}

static void testStopDuringXtra() {
    std::vector<uint8_t> file(2500);
    for (size_t i = 0; i < file.size(); i++) file[i] = uint8_t(i * 7 + 1);
    for (int mode = 0; mode < 3; mode++) {
        // 0: stop while waiting for part 1's indication, 1: stop during the INTERNAL retry backoff,
        // 2: stop then start again during the wait (session must end up running, no extra START/STOP churn)
        auto fake = std::make_shared<FakeModem>();
        std::atomic<bool> hold{true};
        fake->onRequest = [&hold](FakeModem& f, const qmi::Message& r) {
            qmi::Message ind;
            ind.type = qmi::kIndication;
            ind.msgId = r.msgId;
            if (r.msgId == loc::kGetPredictedOrbitsSource || r.msgId == loc::kGetPredictedOrbitsValidity) {
                ind.add(0x01, qmi::Writer().u32(0));
                f.inject(ind);
            } else if (r.msgId == loc::kInjectPredictedOrbits && !hold) {
                uint16_t n = 0;
                r.getU16(0x03, &n);
                ind.add(0x01, qmi::Writer().u32(0));
                ind.add(0x10, qmi::Writer().u16(n));
                f.inject(ind);
            }
        };
        XRec rec;
        EngineConfig cfg;
        cfg.xtraPartTimeoutMs = 5000;
        GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
        eng.begin();
        CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
        eng.setActive(true);
        CHECK(fake->waitFor([&] { return eng.sessionRunning(); }, 2000));
        if (mode == 1) {
            std::lock_guard<std::mutex> l(fake->mu);
            fake->errors[loc::kInjectPredictedOrbits] = 3;
        }
        eng.injectXtra(file);
        CHECK(fake->waitFor([&] { return fake->count(loc::kInjectPredictedOrbits) >= 1; }, 2000));
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        eng.setActive(false);
        if (mode == 2) eng.setActive(true);
        if (mode < 2) {
            CHECK(fake->waitFor([&] { return fake->count(loc::kStop) == 1; }, 300));
            CHECK(!eng.sessionRunning());
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            CHECK(eng.sessionRunning() && fake->count(loc::kStop) == 0 && fake->count(loc::kStart) == 1);
        }
        // the transfer is not aborted by the stop: let it complete
        {
            std::lock_guard<std::mutex> l(fake->mu);
            fake->errors.clear();
        }
        hold = false;
        if (mode == 0) {   // part 1 got no indication: that transfer attempt fails at its deadline, not earlier
            CHECK(fake->waitFor([&] { return rec.done != 0; }, 7000));
            CHECK(rec.done == 2);
        } else if (mode == 1) {
            CHECK(fake->waitFor([&] { return rec.done != 0; }, 7000));
            CHECK(rec.done == 1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(fake->count(loc::kStop) == (mode < 2 ? 1u : 0u));   // queued stop task does not repeat STOP
        CHECK(fake->count(loc::kStart) == 1);                     // no stale restart
        CHECK(eng.sessionRunning() == (mode == 2));
        eng.end();
    }
}

// r5 review F64: UTC assistance queued behind blocking work is advanced by the queue delay (CLOCK_BOOTTIME reference)
static void testQueuedTimeInjection() {
    uint64_t o = 0;
    CHECK(GnssEngine::advanceUtc(1000, 50, 80, &o) && o == 1030);
    CHECK(!GnssEngine::advanceUtc(1000, 90, 80, &o));                  // future reference
    CHECK(!GnssEngine::advanceUtc(-1, 50, 80, &o));                    // invalid UTC
    CHECK(!GnssEngine::advanceUtc(INT64_MAX - 5, 0, 10, &o));          // overflow
    auto fake = std::make_shared<FakeModem>();
    std::atomic<bool> holdPos{true};
    fake->onRequest = [&holdPos](FakeModem&, const qmi::Message& r) {   // runs on the engine worker
        if (r.msgId == loc::kInjectPosition && holdPos) std::this_thread::sleep_for(std::chrono::milliseconds(350));
    };
    Rec rec;
    EngineConfig cfg;
    GnssEngine eng([fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); }, &rec, cfg, nullptr);
    eng.begin();
    CHECK(fake->waitFor([&] { return eng.serviceUp(); }, 2000));
    auto lastUtc = [&](uint64_t* utc, uint32_t* unc) {
        std::lock_guard<std::mutex> l(fake->mu);
        for (auto it = fake->requests.rbegin(); it != fake->requests.rend(); ++it)
            if (it->msgId == loc::kInjectUtcTime) return it->getU64(0x01, utc) && it->getU32(0x02, unc);
        return false;
    };
    // reviewer case: worker busy in an earlier position assistance for ~350 ms, then UTC 1800000000000 +/- 1 ms
    eng.injectLocation(45.0, 5.0, 1000.f);
    CHECK(fake->waitFor([&] { return fake->count(loc::kInjectPosition) == 1; }, 2000));
    int64_t t0 = GnssEngine::bootMs();
    eng.injectTime(1800000000000ull, 1);
    CHECK(fake->waitFor([&] { return fake->count(loc::kInjectUtcTime) == 1; }, 3000));
    int64_t waited = GnssEngine::bootMs() - t0;
    uint64_t utc = 0;
    uint32_t unc = 0;
    CHECK(lastUtc(&utc, &unc) && unc == 1);
    int64_t adv = int64_t(utc - 1800000000000ull);
    CHECK(adv >= 250 && adv <= waited + 2);   // advanced by the queue delay, never beyond the observed wait
    printf("  queued UTC: waited %lld ms, encoded UTC advanced %lld ms\n", (long long)waited, (long long)adv);
    holdPos = false;
    // HAL tuple (Android sampled 1000 ms ago on elapsedRealtime), immediate injection
    eng.injectTime(int64_t(1800000005000ll), 7, GnssEngine::bootMs() - 1000);
    CHECK(fake->waitFor([&] { return fake->count(loc::kInjectUtcTime) == 2; }, 2000));
    CHECK(lastUtc(&utc, &unc) && unc == 7 && utc >= 1800000006000ull && utc <= 1800000006100ull);
    // unusable samples are discarded, not injected
    eng.injectTime(int64_t(1800000005000ll), 7, GnssEngine::bootMs() + 60000);
    eng.injectTime(int64_t(-5), 7, GnssEngine::bootMs());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    CHECK(fake->count(loc::kInjectUtcTime) == 2);
    eng.end();
}

int main() {
    testCapabilities();
    testCodec();
    testPositionWire();
    testSvWire();
    testNmea();
    testRequests();
    testEngine();
    testStartError();
    testNoServiceTimeout();
    testXtra();
    testXtraWireAndErrors();
    testAltitudeDatum();
    testTransientConfigStart();
    testXtraConfirmations();
    testStopDuringXtra();
    testRound6();
    testQueuedTimeInjection();
    printf("A6L_GNSS_HOST_TESTS %s pass=%d fail=%d\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    return gFail ? 1 : 0;
}
