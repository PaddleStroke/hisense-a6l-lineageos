// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 review rounds 7/8 (28 Sep 2026, docs/hardware-review-round7-20260928.md F55-F58,
// docs/hardware-review-round8-20260928.md F59-F61): host tests. Fake modem only: no phone, no call is ever placed
// (the voice cases drive DTMF / reject / call-status indications against a fake VOICE service).
//  F55 setTtyMode acknowledges OFF only (source contract; no TTY path exists)
//  F56 finite DTMF: STOP result counts, bounded STOP retry for the same live call, START refused -> no STOP
//  F57 rejectCall: INCOMING -> End Call(id); WAITING -> CHLD=0 with the waiting call id; only held/active/empty or a
//      failed query -> nothing sent
//  F58 last call fail cause: QMI end reason (All Call Status TLV 0x14) mapped explicitly, kept after removal, id
//      reuse, VOICE loss -> RADIO_INTERNAL_ERROR, unknown -> ERROR_UNSPECIFIED (real ModemCore)
//  F59 CONNECTED + reconfiguration required -> refresh: Updated (DNS/MTU), Unchanged, Invalidate (address change or
//      unusable settings), Gone (old generation); reconfiguration during setup handled after publication
//  F60 DISCONNECTED while Get Current Settings is in flight -> setup fails, nothing cached, the queued loss is stale
//  F61 NITZ: receipt stamped on CLOCK_BOOTTIME, age = queue time, invalid stamps dropped
// Build/run: tests/run-host-tests.sh (review7 binary).
#define main modemcore_tests_main
#include "modemcore_tests.cc"
#undef main

#include "../hal/TimePolicy.h"

#include <a6lqmi/datacall.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

using android::hardware::radio::a6l::bootTimeMs;
using android::hardware::radio::a6l::nitzAgeMs;

static std::string slurp(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string body(const std::string& src, const std::string& start) {
    auto a = src.find(start);
    if (a == std::string::npos) return "";
    auto b = src.find("\nScopedAStatus ", a + 1);
    auto c = src.find("\nvoid ", a + 1);
    b = std::min(b == std::string::npos ? src.size() : b, c == std::string::npos ? src.size() : c);
    return src.substr(a, b - a);
}

// ------------------------------------------------------------------ fake VOICE (F56/F57)
struct VoiceFake {
    std::mutex m;
    std::vector<std::array<uint8_t, 2>> calls;  // (id, state)
    std::atomic<int> stopRejects{0};            // STOP answers with QMI error 3 while > 0
    std::atomic<bool> startReject{false}, queryFail{false}, manageIdRejected{false};
    std::atomic<bool> tone{false};
    std::function<void()> onStopRejected;  // e.g. the call ends
};
static VoiceFake gV;

static std::optional<Message> voiceHandler(uint32_t svc, const Message& req) {
    if (svc != kSvcVoice) return test::okResponse(req.msgId);
    switch (req.msgId) {
        case voice::kGetAllCallInfo: {
            if (gV.queryFail) return test::errResponse(req.msgId, kErrInternal);
            std::lock_guard<std::mutex> g(gV.m);
            std::vector<uint8_t> v{static_cast<uint8_t>(gV.calls.size())};
            for (auto& c : gV.calls) v.insert(v.end(), {c[0], c[1], 0, voice::kDirMt, voice::kModeLte, 0, 0});
            return test::okResponse(req.msgId).raw(0x10, v);
        }
        case voice::kStartContDtmf:
            if (gV.startReject) return test::errResponse(req.msgId, kErrInternal);
            gV.tone = true;
            return test::okResponse(req.msgId);
        case voice::kStopContDtmf:
            if (gV.stopRejects > 0) {
                gV.stopRejects--;
                if (gV.onStopRejected) gV.onStopRejected();
                return test::errResponse(req.msgId, kErrInternal);
            }
            gV.tone = false;
            return test::okResponse(req.msgId);
        case voice::kManageCalls:
            if (gV.manageIdRejected && req.get(0x10)) return test::errResponse(req.msgId, kErrInvalidArgument);
            return test::okResponse(req.msgId);
        default:
            return test::okResponse(req.msgId);
    }
}
static void setCalls(std::vector<std::array<uint8_t, 2>> c) {
    std::lock_guard<std::mutex> g(gV.m);
    gV.calls = std::move(c);
}
static std::vector<Message> reqs(FakeModem& fm, uint16_t id) {
    std::vector<Message> v;
    for (auto& [s, msg] : fm.requests())
        if (s == kSvcVoice && msg.msgId == id) v.push_back(msg);
    return v;
}

static void testVoice() {
    FakeModem fm;
    fm.addService(kSvcVoice);
    fm.setHandler(voiceHandler);
    Client c(fm.transport(), "review7");
    EXPECT(c.start({kSvcVoice}));
    EXPECT(c.waitForServices({kSvcVoice}, 3000).empty());

    // ---- F56
    EXPECT(voice::isDtmfDigit('0') && voice::isDtmfDigit('9') && voice::isDtmfDigit('*') && voice::isDtmfDigit('#'));
    EXPECT(!voice::isDtmfDigit('A') && !voice::isDtmfDigit(',') && !voice::isDtmfDigit('\0'));
    setCalls({{1, voice::kStateConversation}});
    auto o = voice::finiteDtmf(c, 1, '5', 10, 3, 10);  // control: START + STOP accepted
    EXPECT(o.ok() && o.stopAttempts == 1 && !gV.tone);
    gV.stopRejects = 1;  // reviewer case: STOP rejected (QMI error 3) -> retried, tone stopped, success
    o = voice::finiteDtmf(c, 1, '5', 10, 3, 10);
    EXPECT(o.ok() && o.stopAttempts == 2 && !gV.tone);
    gV.stopRejects = 10;  // STOP never accepted: failure reported (was: success), bounded attempts
    o = voice::finiteDtmf(c, 1, '7', 10, 3, 10);
    EXPECT(!o.ok() && o.start.ok() && !o.stop.ok() && o.stopAttempts == 3 && !o.callGone);
    EXPECT(gV.tone);  // the fake still plays: reported as a failure, not as a completed tone
    gV.stopRejects = 0;
    EXPECT(voice::stopDtmf(c, 1).ok() && !gV.tone);
    // the call ends while STOP is refused: no STOP retried on that id (a recycled id is never stopped)
    gV.stopRejects = 1;
    gV.onStopRejected = [] { setCalls({}); };
    int stopsBefore = static_cast<int>(reqs(fm, voice::kStopContDtmf).size());
    o = voice::finiteDtmf(c, 1, '1', 10, 3, 10);
    EXPECT(!o.ok() && o.callGone && o.stopAttempts == 1);
    EXPECT_EQ(static_cast<int>(reqs(fm, voice::kStopContDtmf).size()), stopsBefore + 1);
    gV.onStopRejected = nullptr;
    gV.tone = false;
    // START refused: nothing started, no STOP
    setCalls({{1, voice::kStateConversation}});
    gV.startReject = true;
    stopsBefore = static_cast<int>(reqs(fm, voice::kStopContDtmf).size());
    o = voice::finiteDtmf(c, 1, '2', 10, 3, 10);
    EXPECT(!o.ok() && !o.start.ok() && o.stopAttempts == 0);
    EXPECT_EQ(static_cast<int>(reqs(fm, voice::kStopContDtmf).size()), stopsBefore);
    gV.startReject = false;

    // ---- F57
    auto manage = [&] { return reqs(fm, voice::kManageCalls); };
    auto ends = [&] { return reqs(fm, voice::kEndCall); };
    size_t m0 = manage().size(), e0 = ends().size();
    setCalls({{7, voice::kStateHold}});  // reviewer case: only a held call when the reject runs
    auto r = voice::rejectRingingOrWaiting(c);
    EXPECT(r.kind == voice::RejectOutcome::NoTarget);
    EXPECT_EQ(manage().size(), m0);  // was: Manage Calls sups 1 without call id (released held call 7)
    EXPECT_EQ(ends().size(), e0);
    setCalls({{2, voice::kStateConversation}});  // only active
    EXPECT(voice::rejectRingingOrWaiting(c).kind == voice::RejectOutcome::NoTarget);
    setCalls({});  // empty
    EXPECT(voice::rejectRingingOrWaiting(c).kind == voice::RejectOutcome::NoTarget);
    EXPECT_EQ(manage().size(), m0);
    EXPECT_EQ(ends().size(), e0);
    // failed query: nothing sent
    setCalls({{3, voice::kStateWaiting}, {7, voice::kStateHold}});
    gV.queryFail = true;
    r = voice::rejectRingingOrWaiting(c);
    EXPECT(r.kind == voice::RejectOutcome::QueryFailed && !r.result.ok());
    EXPECT_EQ(manage().size(), m0);
    gV.queryFail = false;
    // waiting alongside held: CHLD=0 targeted with the waiting call id
    r = voice::rejectRingingOrWaiting(c);
    EXPECT(r.kind == voice::RejectOutcome::ReleasedWaiting && r.result.ok() && r.callId == 3);
    {
        auto v = manage();
        EXPECT_EQ(v.size(), m0 + 1);
        auto* sups = v.back().get(0x01);
        auto* id = v.back().get(0x10);
        EXPECT(sups && *sups == std::vector<uint8_t>{voice::kSupsReleaseHeldOrWaiting});
        EXPECT(id && *id == std::vector<uint8_t>{3});
    }
    // incoming: targeted End Call (unchanged)
    setCalls({{4, voice::kStateIncoming}, {7, voice::kStateHold}});
    r = voice::rejectRingingOrWaiting(c);
    EXPECT(r.kind == voice::RejectOutcome::EndedIncoming && r.callId == 4);
    {
        auto v = ends();
        EXPECT(v.size() == e0 + 1 && v.back().get(0x01) && *v.back().get(0x01) == std::vector<uint8_t>{4});
    }
    // modem refuses the call-id TLV: plain CHLD=0 only while the same call is still waiting
    gV.manageIdRejected = true;
    setCalls({{3, voice::kStateWaiting}, {7, voice::kStateHold}});
    size_t m1 = manage().size();
    r = voice::rejectRingingOrWaiting(c);
    EXPECT(r.result.ok());
    EXPECT_EQ(manage().size(), m1 + 2);
    EXPECT(manage().back().get(0x10) == nullptr);
    gV.manageIdRejected = false;
    c.stop();
}

// ------------------------------------------------------------------ F58 mapping (pure)
static void testCauseMapping() {
    using namespace voice;
    EXPECT_EQ(lastCallFailCauseFromQmi(145), 16);   // NORMAL_CALL_CLEARING -> NORMAL
    EXPECT_EQ(lastCallFailCauseFromQmi(29), 16);    // CLIENT_END -> NORMAL
    EXPECT_EQ(lastCallFailCauseFromQmi(146), 17);   // USER_BUSY -> BUSY
    EXPECT_EQ(lastCallFailCauseFromQmi(149), 21);   // CALL_REJECTED
    EXPECT_EQ(lastCallFailCauseFromQmi(141), 1);    // UNASSIGNED_NUMBER -> UNOBTAINABLE_NUMBER
    EXPECT_EQ(lastCallFailCauseFromQmi(157), 34);   // NO_CIRCUIT -> CONGESTION
    EXPECT_EQ(lastCallFailCauseFromQmi(188), 127);  // INTERWORKING_UNSPECIFIED
    EXPECT_EQ(lastCallFailCauseFromQmi(225), 255);  // RADIO_LINK_LOST
    EXPECT_EQ(lastCallFailCauseFromQmi(106), 248);  // NO_GW_SERVICE -> OUT_OF_SERVICE
    EXPECT_EQ(lastCallFailCauseFromQmi(0), 247);    // OFFLINE -> RADIO_OFF
    EXPECT_EQ(lastCallFailCauseFromQmi(115), 240);  // CALL_BARRED
    EXPECT_EQ(lastCallFailCauseFromQmi(135), 252);  // REJECTED_BY_NETWORK -> NETWORK_REJECT
    EXPECT_EQ(lastCallFailCauseFromQmi(230), 253);  // access stratum reject -> RADIO_ACCESS_FAILURE
    EXPECT_EQ(lastCallFailCauseFromQmi(9999), 0xffff);
    EXPECT_EQ(lastCallFailCauseFromQmi(16), 0xffff);  // never a raw cast (QMI 16 is not NORMAL)
    // no failure reason maps to NORMAL: only CLIENT_END / RELEASE_NORMAL / NORMAL_CALL_CLEARING do
    for (int q = 0; q < 400; q++)
        if (lastCallFailCauseFromQmi(static_cast<uint16_t>(q)) == 16) EXPECT(q == 25 || q == 29 || q == 145);
    Message m(MsgType::Indication, kAllCallStatusInd);
    m.raw(0x14, {2, 1, 146, 0, 2, 0x2D, 0x01});  // {1: 146}, {2: 0x012D = 301}
    auto e = parseCallEndReasons(m);
    using P = std::pair<uint8_t, uint16_t>;
    EXPECT(e.size() == 2 && e[0] == P(1, 146) && e[1] == P(2, 301));
    Message bad(MsgType::Indication, kAllCallStatusInd);
    bad.raw(0x14, {1, 1, 146});  // short: ignored, never misread
    EXPECT(parseCallEndReasons(bad).empty());
}

// ------------------------------------------------------------------ F59 / F60 data manager
struct DataFake {
    std::atomic<uint32_t> ip{0x0A000005}, dns{0x08080808}, mtu{1430};
    std::atomic<bool> settingsErr{false};
    std::atomic<int> discDuringSettings{0}, reconfigDuringSettings{0};
    std::atomic<bool> discOnStop{false};
    FakeModem* modem = nullptr;
};
static DataFake gD;

static Message pktStatus(uint8_t status, bool reconfig) {
    Message m(MsgType::Indication, wds::kPacketServiceStatus);
    m.raw(0x01, {status, static_cast<uint8_t>(reconfig ? 1 : 0)});
    return m;
}
static std::optional<Message> dataHandler(uint32_t svc, const Message& req) {
    if (svc != kSvcWds) return test::okResponse(req.msgId);
    if (req.msgId == wds::kStartNetwork) return test::okResponse(req.msgId).u32(0x01, 0x1111);
    if (req.msgId == wds::kStopNetwork && gD.discOnStop) gD.modem->indicate(kSvcWds, pktStatus(wds::kConnDisconnected, false));
    if (req.msgId == wds::kGetCurrentSettings) {
        if (gD.settingsErr) return test::errResponse(req.msgId, kErrInternal);
        auto r = test::okResponse(req.msgId);
        r.u32(0x15, gD.dns).u32(0x1E, gD.ip).u32(0x20, 0x0A000006).u32(0x21, 0xFFFFFFFC).u32(0x29, gD.mtu);
        // the session ends (or is flagged for reconfiguration) while this answer is in flight: the indication is
        // queued before the answer and the dispatcher consumes it while setup still runs
        if (gD.discDuringSettings > 0) {
            gD.discDuringSettings--;
            gD.modem->indicate(kSvcWds, pktStatus(wds::kConnDisconnected, false));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        if (gD.reconfigDuringSettings > 0) {
            gD.reconfigDuringSettings--;
            gD.modem->indicate(kSvcWds, pktStatus(wds::kConnConnected, true));
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
        return r;
    }
    return test::okResponse(req.msgId);
}
struct RecLink : radio::LinkOps {
    std::mutex m;
    std::vector<std::string> ops;
    int run(const std::string& op) {
        std::lock_guard<std::mutex> g(m);
        ops.push_back(op);
        return 0;
    }
    bool exists(const std::string& n) override { return n == "rmnet_ipa0"; }
    int createLink(const std::string&, const std::string& n, uint16_t, uint32_t) override { return run("create " + n); }
    int deleteLink(const std::string& n) override { return run("delete " + n); }
    int setUp(const std::string& n, bool up) override { return run(std::string(up ? "up " : "down ") + n); }
    int setMtu(const std::string& n, int mtu) override { return run("mtu " + n + " " + std::to_string(mtu)); }
    int addAddress(const std::string& n, const std::string& c) override { return run("addr " + c + " " + n); }
    bool has(const std::string& op) {
        std::lock_guard<std::mutex> g(m);
        for (auto& o : ops)
            if (o == op) return true;
        return false;
    }
};

static void testData() {
    FakeModem modem;
    gD.modem = &modem;
    modem.addService(kSvcWds);
    modem.setHandler(dataHandler);
    auto link = std::make_shared<RecLink>();
    radio::DataConfig cfg;
    cfg.setDataFormat = false;
    cfg.settingsRetryMs = 20;
    radio::DataCallManager dm(cfg, [&](const char* tag) { return std::make_unique<Client>(modem.transport(), tag); },
                              nullptr);
    dm.setLinkOps(link);
    std::mutex lm;
    std::vector<std::pair<int, uint64_t>> lost, reconf;
    dm.onLost([&](int cid, uint64_t g) {
        std::lock_guard<std::mutex> l(lm);
        lost.push_back({cid, g});
    });
    dm.onReconfig([&](int cid, uint64_t g) {
        std::lock_guard<std::mutex> l(lm);
        reconf.push_back({cid, g});
    });
    auto nLost = [&] { std::lock_guard<std::mutex> l(lm); return lost.size(); };
    auto nReconf = [&] { std::lock_guard<std::mutex> l(lm); return reconf.size(); };
    radio::DataRequest rq;
    rq.apn = "free";
    rq.protocol = radio::Protocol::V4;

    // ---- F59
    auto a = dm.setup(rq);
    EXPECT(a.ok && a.call.addresses == std::vector<std::string>{"10.0.0.5/30"} && a.call.mtuV4 == 1430);
    EXPECT(a.call.dnses == std::vector<std::string>{"8.8.8.8"});
    int q0 = count(modem, kSvcWds, wds::kGetCurrentSettings);
    gD.dns = 0x09090909;
    gD.mtu = 1280;
    modem.indicate(kSvcWds, pktStatus(wds::kConnConnected, true));  // reviewer case
    EXPECT(waitFor([&] { return nReconf() == 1; }, 3000));
    {
        std::lock_guard<std::mutex> l(lm);
        EXPECT(reconf[0] == std::make_pair(a.call.cid, a.call.generation));
    }
    EXPECT_EQ(nLost(), 0u);
    radio::DataCall c;
    EXPECT(dm.refresh(a.call.cid, a.call.generation, &c) == radio::DataCallManager::Refresh::Updated);
    EXPECT(count(modem, kSvcWds, wds::kGetCurrentSettings) == q0 + 1);  // was: no query after the event
    EXPECT(c.dnses == std::vector<std::string>{"9.9.9.9"} && c.mtuV4 == 1280 && c.generation == a.call.generation);
    EXPECT(dm.list().size() == 1 && dm.list()[0].dnses == c.dnses && dm.list()[0].mtuV4 == 1280);
    EXPECT(link->has("mtu rmnet_data0 1280"));
    EXPECT(dm.refresh(a.call.cid, a.call.generation, &c) == radio::DataCallManager::Refresh::Unchanged);
    // CONNECTED without the bit: nothing
    modem.indicate(kSvcWds, pktStatus(wds::kConnConnected, false));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(nReconf(), 1u);
    // the reviewer's address change (10.0.0.5 -> 10.0.0.9): not swappable in place -> invalidate (reconnect)
    gD.ip = 0x0A000009;
    EXPECT(dm.refresh(a.call.cid, a.call.generation, &c) == radio::DataCallManager::Refresh::Invalidate);
    EXPECT(dm.list()[0].addresses == std::vector<std::string>{"10.0.0.5/30"});  // the HAL tears it down
    gD.ip = 0x0A000005;
    gD.settingsErr = true;
    EXPECT(dm.refresh(a.call.cid, a.call.generation, &c) == radio::DataCallManager::Refresh::Invalidate);
    gD.settingsErr = false;
    EXPECT(dm.refresh(a.call.cid, a.call.generation + 99, &c) == radio::DataCallManager::Refresh::Gone);
    // our own teardown: the DISCONNECTED it causes is not reported as a network loss
    gD.discOnStop = true;
    EXPECT(dm.deactivate(a.call.cid));
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    EXPECT_EQ(nLost(), 0u);
    gD.discOnStop = false;
    EXPECT(dm.refresh(a.call.cid, a.call.generation, &c) == radio::DataCallManager::Refresh::Gone);
    // reconfiguration flagged while setup still runs: reported once the call is published
    gD.dns = 0x08080808;
    gD.mtu = 1430;
    gD.reconfigDuringSettings = 1;
    size_t rBefore = nReconf();
    auto b = dm.setup(rq);
    EXPECT(b.ok);
    EXPECT(waitFor([&] { return nReconf() >= rBefore + 1; }, 3000));
    {
        std::lock_guard<std::mutex> l(lm);
        EXPECT(reconf.back() == std::make_pair(b.call.cid, b.call.generation));
    }
    EXPECT(dm.deactivate(b.call.cid));

    // ---- F60: DISCONNECTED consumed while Get Current Settings is in flight
    gD.discDuringSettings = 1;
    size_t lBefore = nLost();
    auto d = dm.setup(rq);
    EXPECT(!d.ok);  // was: success with the dead session cached
    EXPECT(d.detail.find("ended-during-setup") != std::string::npos);
    EXPECT(dm.list().empty());
    EXPECT(waitFor([&] { return nLost() == lBefore + 1; }, 3000));  // the loss was seen (latched + reported)
    std::pair<int, uint64_t> l1;
    {
        std::lock_guard<std::mutex> l(lm);
        l1 = lost.back();
    }
    EXPECT(!dm.deactivateIfCurrent(l1.first, l1.second));  // the HAL's queued loss: stale, no effect
    // positive control: a later setup works and a later DISCONNECTED is reported for it
    auto e = dm.setup(rq);
    EXPECT(e.ok && e.call.generation != l1.second);
    modem.indicate(kSvcWds, pktStatus(wds::kConnDisconnected, false));
    EXPECT(waitFor([&] { return nLost() == lBefore + 2; }, 3000));
    EXPECT(dm.deactivateIfCurrent(e.call.cid, e.call.generation));
    dm.deactivateAll();
}

// ------------------------------------------------------------------ F55 / F56 / F57 / F58 / F61 source contracts
static void testSources() {
    const std::string dir = A6L_HAL_DIR;
    auto v = slurp(dir + "/RadioMessagingVoice.cpp");
    auto tty = body(v, "ScopedAStatus A6lRadioVoice::setTtyMode(");
    EXPECT(!tty.empty() && tty.find("REQUEST_NOT_SUPPORTED") != std::string::npos);
    EXPECT(tty.find("mTty = mode") == std::string::npos);  // was: stored and acknowledged
    auto dt = body(v, "ScopedAStatus A6lRadioVoice::sendDtmf(");
    EXPECT(dt.find("qv::finiteDtmf") != std::string::npos && dt.find("INVALID_ARGUMENTS") != std::string::npos);
    auto rj = body(v, "ScopedAStatus A6lRadioVoice::rejectCall(");
    EXPECT(rj.find("rejectRingingOrWaiting") != std::string::npos && rj.find("INVALID_STATE") != std::string::npos);
    EXPECT(rj.find("kSupsReleaseHeldOrWaiting") == std::string::npos);
    auto lc = body(v, "ScopedAStatus A6lRadioVoice::getLastCallFailCause(");
    EXPECT(lc.find("lastCallFail()") != std::string::npos && lc.find("vendorCause = f.vendor") != std::string::npos);
    auto n = slurp(dir + "/RadioNetworkData.cpp");
    auto nz = body(n, "void A6lRadioNetwork::onNitz(");
    EXPECT(nz.find("nitzAgeMs(receivedMs") != std::string::npos && nz.find(", 0);") == std::string::npos);
    auto rc = body(n, "void A6lRadioData::onDataCallReconfigured(");
    EXPECT(rc.find("refresh(cid, generation") != std::string::npos && rc.find("setupDataCallBase") != std::string::npos);
    auto mc = slurp(dir + "/ModemCore.h");
    EXPECT(mc.find("mLastCallFailCause = 16") == std::string::npos);
}

// ------------------------------------------------------------------ F61 pure
static void testNitzAge() {
    EXPECT(nitzAgeMs(10000, 14000) == std::optional<int64_t>(4000));  // reviewer case: reference = 10000
    EXPECT(nitzAgeMs(10000, 10000) == std::optional<int64_t>(0));     // immediate delivery
    EXPECT(!nitzAgeMs(15000, 14000));                                 // stamp after now: invalid, dropped
    EXPECT(!nitzAgeMs(0, 14000));                                     // no stamp: dropped, never age 0
    int64_t t0 = bootTimeMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    int64_t t1 = bootTimeMs();
    EXPECT(t1 - t0 >= 29 && t1 - t0 < 2000);
    timespec ts{};
    clock_gettime(CLOCK_BOOTTIME, &ts);
    EXPECT(std::llabs(bootTimeMs() - (ts.tv_sec * 1000LL + ts.tv_nsec / 1000000)) < 50);
}

struct NL : ModemCore::Listener {
    std::atomic<int64_t> nitzRecv{0};
    std::atomic<int> nitz{0};
    void onNitz(const std::string&, int64_t receivedMs) override {
        nitzRecv = receivedMs;
        nitz++;
    }
};

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    testCauseMapping();
    testNitzAge();
    testSources();
    testVoice();
    testData();

    // ---- F58 / F61 in the real ModemCore (single slot)
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady}});
    auto* modem = new FakeModem();  // leaked: core threads outlive main
    modem->setHandler(handler);
    ModemCore::setTransportFactory([modem] { return modem->transport(); });
    for (auto s : kSvcs) modem->addService(s);
    auto& core = ModemCore::get(1);
    auto* l = new L();
    auto* nl = new NL();
    core.addListener(l);
    core.addListener(nl);
    core.start();
    EXPECT(waitFor([&] { return core.ready() && l->ready >= 1; }));
    EXPECT_EQ(core.lastCallFail().cause, 0xffff);  // nothing terminated yet: not NORMAL

    auto callInd = [&](std::vector<std::array<uint8_t, 2>> calls, std::vector<uint8_t> endTlv = {}) {
        Message m(MsgType::Indication, voice::kAllCallStatusInd);
        std::vector<uint8_t> v{static_cast<uint8_t>(calls.size())};
        for (auto& c : calls) v.insert(v.end(), {c[0], c[1], 0, voice::kDirMo, voice::kModeLte, 0, 0});
        m.raw(0x01, v);
        if (!endTlv.empty()) m.raw(0x14, endTlv);
        modem->indicate(kSvcVoice, m);
    };
    auto settle = [&](size_t n) { return waitFor([&] { return core.calls().size() == n; }); };
    // busy: END with reason USER_BUSY (146), then removed -> BUSY kept after removal
    callInd({{1, voice::kStateOrigination}});
    EXPECT(settle(1));
    callInd({{1, voice::kStateEnd}}, {1, 1, 146, 0});
    EXPECT(waitFor([&] { return core.lastCallFail().cause == 17; }));
    EXPECT_EQ(core.lastCallFail().vendor, std::string("qmi-end-reason=146"));
    callInd({});
    EXPECT(settle(0));
    EXPECT_EQ(core.lastCallFail().cause, 17);
    // id reuse: call 1 again, normal remote clearing
    callInd({{1, voice::kStateConversation}});
    EXPECT(settle(1));
    callInd({{1, voice::kStateEnd}}, {1, 1, 145, 0});
    EXPECT(waitFor([&] { return core.lastCallFail().cause == 16; }));
    callInd({});
    EXPECT(settle(0));
    // END without a reason first (unknown), then the same END with CALL_REJECTED (149): the reason wins
    callInd({{2, voice::kStateConversation}});
    EXPECT(settle(1));
    callInd({{2, voice::kStateEnd}});
    EXPECT(waitFor([&] { return core.lastCallFail().cause == 0xffff; }));
    callInd({{2, voice::kStateEnd}}, {1, 2, 149, 0});
    EXPECT(waitFor([&] { return core.lastCallFail().cause == 21; }));
    callInd({});
    EXPECT(settle(0));
    EXPECT_EQ(core.lastCallFail().cause, 21);
    // a call vanishing without END and without reason: unknown, not NORMAL
    callInd({{3, voice::kStateConversation}});
    EXPECT(settle(1));
    callInd({});
    EXPECT(waitFor([&] { return core.lastCallFail().cause == 0xffff; }));
    EXPECT_EQ(core.lastCallFail().vendor, std::string("qmi-end-reason=none"));
    // two calls, the held one ends with a failure while the other continues: most recent termination
    callInd({{4, voice::kStateConversation}, {5, voice::kStateHold}});
    EXPECT(settle(2));
    callInd({{4, voice::kStateConversation}, {5, voice::kStateEnd}}, {1, 5, 225, 0});
    EXPECT(waitFor([&] { return core.lastCallFail().cause == 255; }));
    callInd({{4, voice::kStateConversation}});
    EXPECT(settle(1));
    // reviewer case: VOICE service removed with an active call -> RADIO_INTERNAL_ERROR (was: NORMAL_CLEARING)
    modem->removeService(kSvcVoice);
    EXPECT(waitFor([&] { return core.calls().empty(); }));
    EXPECT_EQ(core.lastCallFail().cause, 250);
    EXPECT_EQ(core.lastCallFail().vendor, std::string("voice-service-lost"));
    modem->addService(kSvcVoice);
    EXPECT(waitFor([&] { return core.ctl().hasService(kSvcVoice); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(core.lastCallFail().cause, 250);  // re-registration with no calls does not invent a new termination

    // ---- F61: the receipt stamp is the boot clock at indication time
    Message t(MsgType::Indication, nas::kNetworkTimeInd);
    t.raw(0x01, {0xEA, 0x07, 9, 28, 12, 0, 0, 0});  // 2026-09-28 12:00:00
    t.u8(0x10, 8);  // round11 F65: a NITZ needs a known time zone (+2 h)
    int64_t before = bootTimeMs();
    modem->indicate(kSvcNas, t);
    EXPECT(waitFor([&] { return nl->nitz >= 1; }));
    int64_t after = bootTimeMs();
    EXPECT(nl->nitzRecv >= before && nl->nitzRecv <= after);
    auto age = nitzAgeMs(nl->nitzRecv, after + 4000);  // delivered 4 s later: age carries the queue time
    EXPECT(age && *age >= 4000 && *age <= 4000 + (after - before));

    printf("a6l-review7 (rounds 7/8) tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
