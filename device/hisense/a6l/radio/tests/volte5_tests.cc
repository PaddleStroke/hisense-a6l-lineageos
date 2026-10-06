// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte5, 28 Sep 2026): offline tests for the IMS client setup the stock qcril does
// (IMSS/IMSA bind subscription, IMS service enable config) and for the stock answers to the IMSDCM
// requests the modem sent on 28 Sep (0x2e REGISTER_APP_STATE, 0x34 SERVICE_ENABLE_STATUS, 0x33
// SUB_DESTROY_INSTANCE, 0x32 WLAN_TZ). The fake modem behaves like the DSDS SDM660 modem seen on 28 Sep:
// IMSA/IMSS answer INVALID_OPERATION until the client binds to a subscription.
#include "fake_modem.h"

#include <a6lqmi/ims.h>
#include <a6lqmi/ims_setup.h>
#include <a6lqmi/imsdcm_service.h>
#include <a6lqmi/log.h>

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

static std::string enc(Message m, uint16_t txn = 1) {
    m.txn = txn;
    return hex(m.encode());
}

// ------------------------------------------------------------------ codecs
static void testCodecs() {
    // IMSA bind (stock: mov w1,#0x33; req {valid=1, u32 sub}) -> TLV 0x10 u32
    EXPECT_EQ(enc(imsa::buildBindSubscription(0)), std::string("0001003300070010040000000000"));
    EXPECT_EQ(enc(imsa::buildBindSubscription(1)), std::string("0001003300070010040001000000"));
    // IMSS bind (stock: mov w1,#0x98; req {u32 sub}) -> mandatory TLV 0x01 u32
    EXPECT_EQ(enc(imss::buildBindSubscription(0)), std::string("0001009800070001040000000000"));
    // IMSS set: 0x10 volte, 0x18 ims_service_enabled
    EXPECT_EQ(enc(imss::buildSetServiceEnableConfig(true, true)), std::string("0001008F0008001001000118010001"));
    EXPECT_EQ(enc(imss::buildSetServiceEnableConfig(std::nullopt, true)), std::string("0001008F00040018010001"));
    // GET response: 0x10 settings_resp, 0x11 volte, 0x12 vt, 0x15 wifi, 0x19 ims_service_enabled
    Message g = okResponse(imss::kGetServiceEnableConfig);
    g.u8(0x10, 0).u8(0x11, 1).u8(0x12, 0).u8(0x15, 0).u8(0x19, 0);
    auto c = imss::parseGetServiceEnableConfig(g);
    EXPECT(c.imsService && *c.imsService == 0);
    EXPECT(c.volte && *c.volte == 1);
    EXPECT_EQ(c.summary(), std::string("ims_service_enabled=0 volte=1 vt=0 wifi=0 settings_resp=0"));
    Message ind(MsgType::Indication, imss::kServiceEnableConfigInd);
    ind.u8(0x10, 1).u8(0x18, 1);
    auto ci = imss::parseServiceEnableConfigInd(ind);
    EXPECT(ci.imsService && *ci.imsService == 1 && ci.volte && *ci.volte == 1 && !ci.vt);
    // names confirmed from the stock imsdatadaemon
    EXPECT_EQ(std::string(dcm::msgName(0x2e)), std::string("REGISTER_APP_STATE"));
    EXPECT_EQ(std::string(dcm::msgName(0x32)), std::string("WLAN_TZ"));
    EXPECT_EQ(std::string(dcm::msgName(0x33)), std::string("SUB_DESTROY_INSTANCE"));
    EXPECT_EQ(std::string(dcm::msgName(0x34)), std::string("SERVICE_ENABLE_STATUS"));
    EXPECT_EQ(std::string(dcm::msgName(0x28)), std::string("HO_MEASUREMENT_INIT"));
    // WLAN_TZ response: 0x03 pdp, 0x04 seq, 0x05 {8 x u16, u64 utc}
    Message tz = dcm::buildWlanTzResp(3, 9, 1790000000);
    EXPECT(tz.resultOk());
    EXPECT(tz.get(0x03) && (*tz.get(0x03))[0] == 3);
    EXPECT(tz.get(0x04) && Reader(*tz.get(0x04)).u32() == 9);
    EXPECT(tz.get(0x05) && tz.get(0x05)->size() == 24);
    if (auto* v = tz.get(0x05); v && v->size() == 24) {
        uint64_t u = 0;
        for (int i = 7; i >= 0; i--) u = (u << 8) | (*v)[16 + i];
        EXPECT_EQ(u, 1790000000ull);
        int year = (*v)[10] | ((*v)[11] << 8), mon = (*v)[8] | ((*v)[9] << 8);
        EXPECT(year >= 2026 && year <= 2027);
        EXPECT(mon >= 1 && mon <= 12);
    }
}

// ------------------------------------------------------------------ 28 Sep replay (IMSDCM handshake)
static std::optional<Message> waitFor(FakeModem& fm, MsgType t, uint16_t id, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        auto m = fm.waitModemClientMsg(100);
        if (m && m->type == t && m->msgId == id) return m;
    }
    return std::nullopt;
}

static void testReplay28Sep() {
    FakeModem fm;
    fm.setHandler([](uint32_t, const Message& m) -> std::optional<Message> { return errResponse(m.msgId, kErrNotSupported); });
    radio::ImsDcmConfig cfg;
    cfg.noWds = true;
    cfg.muxId = 0;
    std::mutex rl;
    std::vector<std::string> lines;
    radio::ImsDcmService svc(cfg, fm.transport(), [&fm](const char* tag) { return std::make_unique<Client>(fm.transport(), tag); });
    svc.onReport([&](const std::string& s) {
        std::lock_guard<std::mutex> l(rl);
        lines.push_back(s);
    });
    EXPECT(svc.start());
    auto saw = [&](const std::string& n) {
        std::lock_guard<std::mutex> l(rl);
        for (auto& s : lines)
            if (s.find(n) != std::string::npos) return true;
        return false;
    };
    auto send = [&](uint16_t id, uint16_t txn, const char* tlvsHex) -> std::optional<Message> {
        // body as captured on 28 Sep (a6l-imsdcm dump: t28/volte2-logs/imsdcm.txt)
        Message m = Message::request(id);
        std::string h = tlvsHex;
        size_t p = 0;
        while (p + 2 <= h.size()) {  // "TT:VVVV;" pairs
            uint8_t t = static_cast<uint8_t>(strtoul(h.substr(p, 2).c_str(), nullptr, 16));
            size_t e = h.find(';', p);
            m.raw(t, *unhex(h.substr(p + 3, e - p - 3)));
            p = e + 1;
        }
        m.txn = txn;
        fm.modemRequest(dcm::kService, m);
        return waitFor(fm, MsgType::Response, id);
    };
    // LINK_ADDR {port 9099, family v6, "fe80::41e0:e573:3749:6a32"}
    std::string la = "8b230100000019" + hex(std::vector<uint8_t>({'f', 'e', '8', '0', ':', ':', '4', '1', 'e', '0', ':', 'e', '5', '7', '3', ':', '3', '7', '4', '9', ':', '6', 'a', '3', '2'}));
    auto r0 = send(dcm::kLinkAddr, 1, ("01:" + la + ";").c_str());
    EXPECT(r0 && r0->resultOk());
    auto r1 = send(dcm::kAppStateReq, 2, "10:01;11:00000000;");
    EXPECT(r1 && r1->resultOk() && r1->txn == 2 && !r1->has(0x10));  // stock: plain success
    auto r2 = send(dcm::kServiceEnableStatus, 3, "01:0000000000000000;");
    EXPECT(r2 && r2->resultOk() && r2->txn == 3);
    auto r3 = send(dcm::kSubDestroyInstance, 6, "01:01000000;");
    EXPECT(r3 && r3->resultOk());
    auto r4 = send(dcm::kSubDestroyInstance, 7, "01:02000000;");
    EXPECT(r4 && r4->resultOk());
    auto r5 = send(dcm::kServiceEnableStatus, 8, "01:0300000000000000;");
    EXPECT(r5 && r5->resultOk());
    EXPECT(saw("REGISTER_APP_STATE app=1 instance=0 -> ok (stock behaviour)"));
    EXPECT(saw("SERVICE_ENABLE_STATUS rcs_mask=0x0 -> ok"));
    EXPECT(saw("SERVICE_ENABLE_STATUS rcs_mask=0x3 -> ok"));
    EXPECT(saw("SUB_DESTROY_INSTANCE instance=2 -> ok"));
    EXPECT(saw("LINK_ADDR port=9099 family=v6 addr=fe80::41e0:e573:3749:6a32"));
    // the full QMI hex is in the report line (never masked by the scripts: see volte5-test.sh mask)
    EXPECT(saw("[01]0000000000000000"));
    // after the handshake the PDN request still works (noWds -> failure indication, answered at once)
    dcm::PdpActivateReq q;
    q.apn = "ims";
    q.profile = 6;
    q.seq = 4;
    Message pm = dcm::buildPdpActivateReq(q);
    pm.txn = 9;
    fm.modemRequest(dcm::kService, pm);
    auto pr = waitFor(fm, MsgType::Response, dcm::kPdpActivate);
    EXPECT(pr && pr->resultOk());
    auto pi = waitFor(fm, MsgType::Indication, dcm::kPdpActivate);
    EXPECT(pi && !pi->resultOk());
    svc.stop();
}

// ------------------------------------------------------------------ IMSS / IMSA setup on a DSDS-like modem
struct ImsModem {
    std::mutex lock;
    bool imssBound = false, imsaBound = false;
    uint32_t imssSub = 99, imsaSub = 99;
    uint8_t imsService = 0, volte = 0;
    bool failSet = false;
    bool registeredWhenEnabled = true;
    std::vector<uint16_t> imssLog, imsaLog;
};

static void install(FakeModem& fm, ImsModem& s) {
    fm.addService(kSvcImss);
    fm.addService(kSvcImsa);
    fm.setHandler([&s](uint32_t svc, const Message& m) -> std::optional<Message> {
        std::lock_guard<std::mutex> l(s.lock);
        if (svc == kSvcImss) {
            s.imssLog.push_back(m.msgId);
            if (m.msgId == imss::kBindSubscription) {
                auto* v = m.get(0x01);
                if (!v || v->size() < 4) return errResponse(m.msgId, kErrMissingArgument);
                s.imssBound = true;
                s.imssSub = Reader(*v).u32();
                return okResponse(m.msgId);
            }
            if (!s.imssBound) return errResponse(m.msgId, kErrInvalidOperation);
            if (m.msgId == imss::kGetServiceEnableConfig) {
                Message r = okResponse(m.msgId);
                r.u8(0x10, 0).u8(0x11, s.volte).u8(0x12, 0).u8(0x15, 0).u8(0x19, s.imsService);
                return r;
            }
            if (m.msgId == imss::kSetServiceEnableConfig) {
                if (s.failSet) return errResponse(m.msgId, kErrInternal);
                if (auto* v = m.get(0x10)) s.volte = (*v)[0];
                if (auto* v = m.get(0x18)) s.imsService = (*v)[0];
                return okResponse(m.msgId);
            }
            return errResponse(m.msgId, kErrNotSupported);
        }
        if (svc == kSvcImsa) {
            s.imsaLog.push_back(m.msgId);
            if (m.msgId == imsa::kBindSubscription) {
                auto* v = m.get(0x10);
                s.imsaBound = true;
                s.imsaSub = v && v->size() >= 4 ? Reader(*v).u32() : 0;
                return okResponse(m.msgId);
            }
            if (!s.imsaBound) return errResponse(m.msgId, kErrInvalidOperation);
            bool reg = s.registeredWhenEnabled && s.imsService == 1;
            if (m.msgId == imsa::kGetRegStatus)
                return okResponse(m.msgId).u8(0x10, reg ? 1 : 0).u32(0x12, reg ? imsa::kRegistered : imsa::kNotRegistered);
            if (m.msgId == imsa::kGetServicesStatus)
                return okResponse(m.msgId).u32(0x11, reg ? imsa::kSvcAvailable : imsa::kSvcUnavailable);
            return okResponse(m.msgId);
        }
        return errResponse(m.msgId, kErrNotSupported);
    });
}

struct Lines {
    std::vector<std::string> v;
    radio::ImsReportFn fn() {
        return [this](const std::string& s) { v.push_back(s); };
    }
    bool saw(const std::string& n) const {
        for (auto& s : v)
            if (s.find(n) != std::string::npos) return true;
        return false;
    }
};

static void testImssEnable() {
    FakeModem fm;
    ImsModem s;
    install(fm, s);
    Client c(fm.transport(), "t");
    EXPECT(c.start({kSvcImss, kSvcImsa}));
    EXPECT(c.waitForServices({kSvcImss, kSvcImsa}, 2000).empty());
    // 28 Sep: unbound -> INVALID_OPERATION (err 70) on both services
    imsa::RegStatus rs;
    auto r = imsa::getRegStatus(c, &rs);
    EXPECT(r.status == Result::QmiFailure && r.qmiError == kErrInvalidOperation);
    Lines L;
    auto o = radio::imssSetup(c, 0u, radio::ImssMode::Enable, L.fn());
    EXPECT(o.bound && o.readOk && o.wrote && o.writeOk && o.readAfterOk);
    EXPECT(o.enabled());
    EXPECT(s.imssSub == 0 && s.imsService == 1 && s.volte == 1);
    EXPECT((s.imssLog == std::vector<uint16_t>{imss::kBindSubscription, imss::kGetServiceEnableConfig,
                                                imss::kSetServiceEnableConfig, imss::kGetServiceEnableConfig}));
    EXPECT(L.saw("A6L_IMSDCM_IMSS bind sub=0: ok"));
    EXPECT(L.saw("A6L_IMSDCM_IMSS get ims_service_enabled=0 volte=0"));
    EXPECT(L.saw("A6L_IMSDCM_IMSS set volte=1 ims_service_enabled=1: ok"));
    EXPECT(L.saw("A6L_IMSDCM_IMSS get-after ims_service_enabled=1 volte=1"));
    Lines A;
    auto a = radio::imsaAttach(c, 0u, A.fn());
    EXPECT(a.bound && a.indOk && a.regOk && a.svcOk);
    EXPECT(a.reg.registered() && a.services.voiceAvailable());
    EXPECT(s.imsaLog.size() >= 4 && s.imsaLog[1] == imsa::kBindSubscription);  // [0] = the unbound probe above
    EXPECT(A.saw("A6L_IMSDCM_IMSA reg status=registered"));
    EXPECT(A.saw("A6L_IMSDCM_IMSA bind sub=0: ok"));
    // second run: already on -> no write
    Lines L2;
    auto o2 = radio::imssSetup(c, 0u, radio::ImssMode::Enable, L2.fn());
    EXPECT(!o2.wrote && o2.enabled());
    EXPECT(L2.saw("already on (no write)"));
    c.stop();
}

static void testImssReadAndOff() {
    FakeModem fm;
    ImsModem s;
    install(fm, s);
    Client c(fm.transport(), "t");
    EXPECT(c.start({kSvcImss, kSvcImsa}));
    EXPECT(c.waitForServices({kSvcImss}, 2000).empty());
    Lines L;
    auto o = radio::imssSetup(c, 0u, radio::ImssMode::Read, L.fn());
    EXPECT(o.readOk && !o.wrote && !o.enabled());
    EXPECT(s.imsService == 0);
    size_t n = s.imssLog.size();
    auto off = radio::imssSetup(c, 0u, radio::ImssMode::Off, L.fn());
    EXPECT(!off.readOk && !off.wrote);
    EXPECT_EQ(s.imssLog.size(), n);  // off = nothing sent
    // no bind (A6L_IMSDCM_SUB=none) on a fresh client of a fresh modem: reads fail like 28 Sep
    FakeModem fm2;
    ImsModem s2;
    install(fm2, s2);
    Client c2(fm2.transport(), "t2");
    EXPECT(c2.start({kSvcImss, kSvcImsa}));
    EXPECT(c2.waitForServices({kSvcImss, kSvcImsa}, 2000).empty());
    Lines L2;
    auto o2 = radio::imssSetup(c2, std::nullopt, radio::ImssMode::Read, L2.fn());
    EXPECT(!o2.bound && !o2.readOk);
    EXPECT(L2.saw("INVALID_OPERATION") || L2.saw("70"));
    auto a2 = radio::imsaAttach(c2, std::nullopt, L2.fn());
    EXPECT(!a2.regOk);
    // set failure is reported, not enabled
    s2.failSet = true;
    Lines L3;
    auto o3 = radio::imssSetup(c2, 0u, radio::ImssMode::Enable, L3.fn());
    EXPECT(o3.wrote && !o3.writeOk && !o3.enabled());
    EXPECT(L3.saw("A6L_IMSDCM_IMSS set"));
    c.stop();
    c2.stop();
    EXPECT(radio::imssModeFromString("enable") == radio::ImssMode::Enable);
    EXPECT(radio::imssModeFromString("") == radio::ImssMode::Read);
    EXPECT(radio::imssModeFromString("off") == radio::ImssMode::Off);
}

int main(int argc, char** argv) {
    gLogLevel = (argc > 1 && !strcmp(argv[1], "-v")) ? 4 : 0;
    for (int round = 0; round < 3; round++) {
        testCodecs();
        testReplay28Sep();
        testImssEnable();
        testImssReadAndOff();
    }
    printf("a6l-volte5 tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
