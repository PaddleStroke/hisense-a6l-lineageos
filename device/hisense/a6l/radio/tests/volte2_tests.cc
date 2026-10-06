// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): offline tests for the IMSDCM (770) server, the
// QRTR server helper and the IMSA codec. The fake modem plays both sides: it serves WDS to our
// client and acts as the modem IMS stack (QMI client of our 770 server).
#include "fake_modem.h"

#include <a6lqmi/ims.h>
#include <a6lqmi/imsdcm_service.h>
#include <a6lqmi/log.h>
#include <a6lqmi/server.h>
#include <a6lqmi/services.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using a6l::test::okResponse;
using a6l::test::errResponse;
namespace dcm = a6l::qmi::imsdcm;

static int gFail = 0, gPass = 0;
#define EXPECT(c)                                                                  \
    do {                                                                           \
        if (c) gPass++;                                                            \
        else { gFail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } \
    } while (0)
#define EXPECT_EQ(a, b)                                                            \
    do {                                                                           \
        auto _a = (a); auto _b = (b);                                              \
        if (_a == _b) gPass++;                                                     \
        else { gFail++; fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); } \
    } while (0)

static std::vector<uint8_t> H(const char* s) { return *unhex(s); }

// ------------------------------------------------------------------ codec
static void testCodec() {
    dcm::PdpActivateReq q;
    q.apn = "ims";
    q.apnType = dcm::kApnIms;
    q.rat = dcm::kRatLte;
    q.family = dcm::kFamilyV4;
    q.profile = 6;
    q.seq = 7;
    q.instance = 1;
    Message m = dcm::buildPdpActivateReq(q);
    m.txn = 3;
    // IDL T0.2: {string<=100 (u8 len), u32 apn type, u32 rat, u32 family, u32 profile}; libqmi same
    // verified by hand: len 0x25 = 23 + 7 + 7
    EXPECT_EQ(hex(m.encode()), std::string("0003002000250001140003696D7300000000010000000000000006000000") +
                                   "10040007000000" + "13040001000000");
    auto p = dcm::parsePdpActivate(*Message::decode(m.encode()));
    EXPECT(p.has_value());
    EXPECT_EQ(p->apn, std::string("ims"));
    EXPECT_EQ(p->profile, 6u);
    EXPECT_EQ(p->rat, 1u);
    EXPECT(p->seq && *p->seq == 7);
    EXPECT(!p->slot);
    // truncated struct -> nullopt
    Message bad = Message::request(dcm::kPdpActivate);
    bad.raw(0x01, H("03696d7300000000"));
    EXPECT(!dcm::parsePdpActivate(bad));
    Message none = Message::request(dcm::kPdpActivate);
    EXPECT(!dcm::parsePdpActivate(none));

    // indication: 0x02 result, 0x01 pdp id, 0x10 seq, 0x11 {u32 family, u8 len, addr}, 0x12 inst
    Message ind = dcm::buildPdpActivateInd(0, 1, 7u, dcm::encodeAddress(0, "10.1.2.3", true), 1u);
    EXPECT_EQ(hex(ind.encode()), std::string("04000020002900") + "01010001" + "02040000000000" + "10040007000000" +
                                     "110D0000000000083130" + "2E312E322E33" + "12040001000000");
    auto bin = dcm::encodeAddress(1, "2a01:cb00::1", false);
    EXPECT_EQ(bin.size(), 4u + 1u + 16u);
    EXPECT_EQ(bin[4], 16);
    EXPECT_EQ(bin[5], 0x2a);
    auto bin4 = dcm::encodeAddress(0, "10.245.241.164", false);
    EXPECT_EQ(hex(bin4), std::string("00000000040AF5F1A4"));
    Message fail = dcm::buildPdpActivateInd(kErrCallFailed, 2, std::nullopt, std::nullopt, std::nullopt);
    EXPECT(!fail.resultOk());
    EXPECT_EQ(fail.error(), static_cast<uint16_t>(kErrCallFailed));
    EXPECT(!fail.has(0x11));
    Message r = dcm::buildPdpActivateResp(0, 5, 9u, std::nullopt);
    EXPECT(r.resultOk());
    EXPECT_EQ((*r.get(0x10))[0], 5);
    EXPECT(r.has(0x11) && !r.has(0x12));
    EXPECT_EQ(std::string(dcm::msgName(0x20)), std::string("PDP_ACTIVATE"));

    // IMSA: response TLVs 0x10 flag, 0x11 err, 0x12 status, 0x13 text, 0x14 tech (stock IDL)
    Message rr(MsgType::Response, imsa::kGetRegStatus);
    rr.raw(kTlvResult, {0, 0, 0, 0});
    rr.u8(0x10, 1);
    rr.u32(0x12, imsa::kRegistered);
    rr.u32(0x14, imsa::kTechWwan);
    auto rs = imsa::parseRegStatus(rr);
    EXPECT(rs.registered());
    EXPECT_EQ(rs.summary(), std::string("status=registered tech=wwan"));
    // indication: 0x01 flag, 0x10 err, 0x11 status, 0x12 text, 0x13 tech
    Message ri(MsgType::Indication, imsa::kRegStatusInd);
    ri.u8(0x01, 0);
    ri.u16(0x10, 403);
    ri.u32(0x11, imsa::kNotRegistered);
    ri.raw(0x12, {'F', 'o', 'r', 'b', 'i', 'd', 'd', 'e', 'n'}); // top-level QMI string
    auto rsi = imsa::parseRegStatus(ri);
    EXPECT(!rsi.registered());
    EXPECT(rsi.errorCode && *rsi.errorCode == 403);
    EXPECT_EQ(rsi.errorText, std::string("Forbidden"));
    // older firmware: only the boolean
    Message ro(MsgType::Indication, imsa::kRegStatusInd);
    ro.u8(0x01, 1);
    EXPECT(imsa::parseRegStatus(ro).registered());
    Message ss(MsgType::Indication, imsa::kServicesStatusInd);
    ss.u32(0x10, imsa::kSvcAvailable);
    ss.u32(0x11, imsa::kSvcAvailable);
    ss.u32(0x14, imsa::kTechWwan);
    auto s = imsa::parseServicesStatus(ss);
    EXPECT(s.voiceAvailable());
    EXPECT_EQ(s.summary(), std::string("voice=available/wwan sms=available vt=- ut=-"));
}

// ------------------------------------------------------------------ fake WDS
struct FakeWds {
    std::mutex lock;
    int starts = 0, stops = 0, binds = 0;
    uint8_t lastProfile = 0, lastFamily = 0, lastMux = 0;
    std::string lastApn;
    uint32_t stoppedHandle = 0;
    bool failStart = false;
    bool noEffect = false;
    uint32_t handle = 0x1234;
};

static void installWds(FakeModem& fm, FakeWds& w) {
    fm.addService(kSvcWds);
    fm.setHandler([&w](uint32_t svc, const Message& m) -> std::optional<Message> {
        if (svc != kSvcWds) return errResponse(m.msgId, kErrNotSupported);
        std::lock_guard<std::mutex> l(w.lock);
        switch (m.msgId) {
            case wds::kBindMuxDataPort:
                w.binds++;
                if (auto* v = m.get(0x11)) w.lastMux = (*v)[0];
                return okResponse(m.msgId);
            case wds::kSetIpFamily:
                w.lastFamily = (*m.get(0x01))[0];
                return okResponse(m.msgId);
            case wds::kStartNetwork: {
                w.starts++;
                w.lastProfile = m.has(0x31) ? (*m.get(0x31))[0] : 0;
                if (auto* v = m.get(0x14)) w.lastApn.assign(v->begin(), v->end());
                if (w.failStart) {
                    Message r = errResponse(m.msgId, kErrCallFailed);
                    r.u16(0x10, 1000);
                    return r;
                }
                if (w.noEffect) return errResponse(m.msgId, kErrNoEffect);
                Message r = okResponse(m.msgId);
                r.u32(0x01, w.handle);
                return r;
            }
            case wds::kGetCurrentSettings: {
                Message r = okResponse(m.msgId);
                if (w.lastFamily == 6) {
                    std::vector<uint8_t> a = H("2a01cb000000000000000000000000010040");
                    r.raw(0x25, a);
                } else {
                    r.u32(0x1E, 0x0AF5F1A4);  // 10.245.241.164
                    r.u32(0x29, 1400);
                }
                return r;
            }
            case wds::kStopNetwork:
                w.stops++;
                w.stoppedHandle = Reader(*m.get(0x01)).u32();
                return okResponse(m.msgId);
            default: return okResponse(m.msgId);
        }
    });
}

static std::optional<Message> waitFor(FakeModem& fm, MsgType t, uint16_t id, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        auto m = fm.waitModemClientMsg(100);
        if (m && m->type == t && m->msgId == id) return m;
    }
    return std::nullopt;
}

struct Rig {
    FakeModem fm;
    FakeWds w;
    std::unique_ptr<radio::ImsDcmService> svc;
    std::mutex rl;
    std::vector<std::string> lines;
    explicit Rig(radio::ImsDcmConfig cfg = {}) {
        installWds(fm, w);
        svc = std::make_unique<radio::ImsDcmService>(cfg, fm.transport(), [this](const char* tag) {
            return std::make_unique<Client>(fm.transport(), tag);
        });
        svc->onReport([this](const std::string& s) {
            std::lock_guard<std::mutex> l(rl);
            lines.push_back(s);
        });
    }
    ~Rig() { svc.reset(); }  // stop() still reports into lines/rl
    bool saw(const std::string& needle) {
        std::lock_guard<std::mutex> l(rl);
        for (auto& s : lines)
            if (s.find(needle) != std::string::npos) return true;
        return false;
    }
    std::optional<Message> activate(uint32_t family = 0, uint32_t profile = 6, uint32_t seq = 11) {
        dcm::PdpActivateReq q;
        q.apn = "ims";
        q.apnType = dcm::kApnIms;
        q.rat = dcm::kRatLte;
        q.family = family;
        q.profile = profile;
        q.seq = seq;
        Message m = dcm::buildPdpActivateReq(q);
        m.txn = static_cast<uint16_t>(seq);
        fm.modemRequest(dcm::kService, m);
        return waitFor(fm, MsgType::Response, dcm::kPdpActivate);
    }
};

static void testServerPublishAndUnknown() {
    Rig r;
    EXPECT(r.svc->start());
    uint32_t iw = 0;
    EXPECT(r.fm.published(dcm::kService, &iw));
    EXPECT_EQ(iw, 1u);  // IDL major 1, instance 0
    EXPECT(r.saw("A6L_IMSDCM_PUBLISHED service=770"));
    // REGISTER_APP_STATE? (0x2e) -> ok
    Message m = Message::request(dcm::kAppStateReq);
    m.u8(0x10, 1);
    m.txn = 77;
    r.fm.modemRequest(dcm::kService, m);
    auto resp = waitFor(r.fm, MsgType::Response, dcm::kAppStateReq);
    EXPECT(resp && resp->resultOk() && resp->txn == 77);
    // WLAN_TZ (0x32; volte5: stock answers success + local time) with the IDL's mandatory TLVs present
    Message w = Message::request(0x0032);
    w.u8(0x01, 1);
    w.u32(0x02, 0);
    r.fm.modemRequest(dcm::kService, w);
    auto wr = waitFor(r.fm, MsgType::Response, 0x0032);
    EXPECT(wr && wr->resultOk() && wr->has(0x03) && wr->has(0x05) && wr->get(0x05)->size() == 24);
    // missing mandatory TLV
    r.fm.modemRequest(dcm::kService, Message::request(dcm::kPdpActivate));
    auto mr = waitFor(r.fm, MsgType::Response, dcm::kPdpActivate);
    EXPECT(mr && mr->error() == kErrMissingArgument);
    r.svc->stop();
    EXPECT(!r.fm.published(dcm::kService));
}

static void testActivateV4() {
    Rig r;
    EXPECT(r.svc->start());
    auto resp = r.activate(0, 6, 11);
    EXPECT(resp && resp->resultOk());
    EXPECT(resp && resp->get(0x10) && (*resp->get(0x10))[0] == 1);
    EXPECT(resp && resp->txn == 11);
    auto ind = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
    EXPECT(ind && ind->resultOk());
    if (ind) {
        EXPECT_EQ((*ind->get(0x01))[0], 1);
        EXPECT_EQ(Reader(*ind->get(0x10)).u32(), 11u);
        Reader a(*ind->get(0x11));
        EXPECT_EQ(a.u32(), 0u);
        EXPECT_EQ(a.str8(), std::string("10.245.241.164"));
    }
    {
        std::lock_guard<std::mutex> l(r.w.lock);
        EXPECT_EQ(r.w.lastProfile, 6);
        EXPECT_EQ(r.w.lastFamily, 4);
        EXPECT_EQ(r.w.lastMux, 9);
        EXPECT_EQ(r.w.starts, 1);
    }
    EXPECT(r.saw("A6L_IMSDCM_PDP id=1 apn='ims' family=v4 state=up addr=10.245.241.164"));
    // duplicate activation for the same type/family -> same id, success indication again
    auto again = r.activate(0, 6, 12);
    EXPECT(again && (*again->get(0x10))[0] == 1);
    auto ind2 = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
    EXPECT(ind2 && ind2->resultOk() && Reader(*ind2->get(0x10)).u32() == 12u);
    {
        std::lock_guard<std::mutex> l(r.w.lock);
        EXPECT_EQ(r.w.starts, 1);
    }
    // GET_IP_ADDRESS -> resp + ind 0x22
    Message g = Message::request(dcm::kGetIpAddress);
    g.u8(0x01, 1);
    r.fm.modemRequest(dcm::kService, g);
    auto gr = waitFor(r.fm, MsgType::Response, dcm::kGetIpAddress);
    EXPECT(gr && gr->resultOk());
    auto gi = waitFor(r.fm, MsgType::Indication, dcm::kGetIpAddress);
    EXPECT(gi && gi->has(0x10));
    // unknown PDP id
    Message d9 = Message::request(dcm::kPdpDeactivate);
    d9.u8(0x01, 9);
    r.fm.modemRequest(dcm::kService, d9);
    auto dr9 = waitFor(r.fm, MsgType::Response, dcm::kPdpDeactivate);
    EXPECT(dr9 && dr9->error() == kErrInvalidHandle);
    // deactivate -> WDS Stop Network with the handle
    Message d = Message::request(dcm::kPdpDeactivate);
    d.u8(0x01, 1);
    r.fm.modemRequest(dcm::kService, d);
    auto dr = waitFor(r.fm, MsgType::Response, dcm::kPdpDeactivate);
    EXPECT(dr && dr->resultOk() && (*dr->get(0x10))[0] == 1);
    for (int i = 0; i < 100; i++) {
        {
            std::lock_guard<std::mutex> l(r.w.lock);
            if (r.w.stops) break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT(r.svc->pdps().empty());
    {
        std::lock_guard<std::mutex> l(r.w.lock);
        EXPECT_EQ(r.w.stops, 1);
        EXPECT_EQ(r.w.stoppedHandle, 0x1234u);
    }
    r.svc->stop();
}

static void testActivateV6AndLost() {
    Rig r;
    EXPECT(r.svc->start());
    auto resp = r.activate(1, 6, 21);
    EXPECT(resp && resp->resultOk());
    auto ind = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
    EXPECT(ind && ind->resultOk());
    if (ind) {
        Reader a(*ind->get(0x11));
        EXPECT_EQ(a.u32(), 1u);
        EXPECT_EQ(a.str8(), std::string("2a01:cb00::1"));
    }
    // network drops the PDN -> failure indication (stock: PDP_ACTIVATE_IND on eCS_ENETNONET)
    Message ps(MsgType::Indication, wds::kPacketServiceStatus);
    ps.raw(0x01, {wds::kConnDisconnected, 0});
    ps.u16(0x10, 2);
    r.fm.indicate(kSvcWds, ps);
    auto lost = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
    EXPECT(lost && !lost->resultOk() && (*lost->get(0x01))[0] == 1);
    for (int i = 0; i < 50 && r.svc->pdps().size(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT(r.svc->pdps().empty());
    EXPECT(r.saw("A6L_IMSDCM_PDP_LOST id=1"));
    r.svc->stop();
}

static void testFailuresAndClientGone() {
    {
        Rig r;
        r.w.failStart = true;
        EXPECT(r.svc->start());
        auto resp = r.activate();
        EXPECT(resp && resp->resultOk());
        auto ind = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
        EXPECT(ind && ind->error() == kErrCallFailed && !ind->has(0x11));
        EXPECT(r.saw("state=failed"));
        EXPECT(r.saw("end=1000"));
        EXPECT(r.svc->pdps().empty());
        r.svc->stop();
    }
    {  // NO_EFFECT = PDN already up (modem attached with IMS as a bearer): success
        Rig r;
        r.w.noEffect = true;
        EXPECT(r.svc->start());
        r.activate();
        auto ind = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
        EXPECT(ind && ind->resultOk());
        r.svc->stop();
    }
    {  // modem client disappears (SSR / BYE) -> the call is stopped
        radio::ImsDcmConfig c;
        c.muxId = 0;
        c.profileOverride = 3;
        Rig r(c);
        EXPECT(r.svc->start());
        r.activate(0, 6, 1);
        auto ind = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
        EXPECT(ind && ind->resultOk());
        {
            std::lock_guard<std::mutex> l(r.w.lock);
            EXPECT_EQ(r.w.binds, 0);
            EXPECT_EQ(r.w.lastProfile, 3);
        }
        r.fm.modemClientGone(dcm::kService, true);
        for (int i = 0; i < 100; i++) {
            {
                std::lock_guard<std::mutex> l(r.w.lock);
                if (r.w.stops) break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        EXPECT(r.svc->pdps().empty());
        EXPECT(r.saw("A6L_IMSDCM_CLIENT_GONE"));
        std::lock_guard<std::mutex> l(r.w.lock);
        EXPECT_EQ(r.w.stops, 1);
    }
    {  // profile 0 -> by APN name
        Rig r;
        EXPECT(r.svc->start());
        r.activate(0, 0, 1);
        auto ind = waitFor(r.fm, MsgType::Indication, dcm::kPdpActivate);
        EXPECT(ind && ind->resultOk());
        std::lock_guard<std::mutex> l(r.w.lock);
        EXPECT_EQ(r.w.lastProfile, 0);
        EXPECT_EQ(r.w.lastApn, std::string("ims"));
    }
}

static void testImsaClient() {
    FakeModem fm;
    fm.addService(kSvcImsa);
    std::atomic<int> regs{0};
    fm.setHandler([&](uint32_t svc, const Message& m) -> std::optional<Message> {
        if (svc != kSvcImsa) return std::nullopt;
        if (m.msgId == imsa::kRegisterIndications) {
            if (m.has(0x10) && m.has(0x11)) regs++;
            return okResponse(m.msgId);
        }
        if (m.msgId == imsa::kGetRegStatus) {
            Message r = okResponse(m.msgId);
            r.u8(0x10, 0);
            r.u32(0x12, imsa::kRegistering);
            return r;
        }
        if (m.msgId == imsa::kGetServicesStatus) {
            Message r = okResponse(m.msgId);
            r.u32(0x11, imsa::kSvcUnavailable);
            return r;
        }
        return errResponse(m.msgId, kErrNotSupported);
    });
    Client c(fm.transport(), "imsa");
    EXPECT(c.start({kSvcImsa}));
    EXPECT(c.waitForServices({kSvcImsa}, 2000).empty());
    EXPECT(imsa::registerIndications(c).ok());
    EXPECT_EQ(regs.load(), 1);
    imsa::RegStatus rs;
    EXPECT(imsa::getRegStatus(c, &rs).ok());
    EXPECT(rs.status && *rs.status == imsa::kRegistering);
    imsa::ServicesStatus ss;
    EXPECT(imsa::getServicesStatus(c, &ss).ok());
    EXPECT(!ss.voiceAvailable());
    std::atomic<bool> got{false};
    c.onIndication(kSvcImsa, imsa::kRegStatusInd, [&](const Message& m) {
        got = imsa::parseRegStatus(m).registered();
    });
    Message ind(MsgType::Indication, imsa::kRegStatusInd);
    ind.u8(0x01, 1);
    ind.u32(0x11, imsa::kRegistered);
    fm.indicate(kSvcImsa, ind);
    for (int i = 0; i < 50 && !got; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT(got.load());
    c.stop();
}

int main(int argc, char** argv) {
    gLogLevel = (argc > 1 && !strcmp(argv[1], "-v")) ? 4 : 0;
    for (int round = 0; round < 3; round++) {
        testCodec();
        testServerPublishAndUnknown();
        testActivateV4();
        testActivateV6AndLost();
        testFailuresAndClientGone();
        testImsaClient();
    }
    printf("a6l-volte2 tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
