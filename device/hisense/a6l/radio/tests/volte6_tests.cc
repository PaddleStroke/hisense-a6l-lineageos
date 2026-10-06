// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte6, 29 Sep 2026): offline tests for the volte6 changes (docs/volte6-20260929.md):
//  * the IMSDCM answers to the requests the modem sent on 29 Sep are byte-identical to the stock imsdatadaemon
//    (result TLV only: qmi_csi_send_resp with a zeroed 8/16-byte struct at 0x18a34, 0x18e10, 0x18f90, 0x175b8),
//  * 0x33 SUB_DESTROY_INSTANCE is named with QmiImsDcmInstanceId (1 = GLOBAL) and raised as a modem event,
//  * IMSS force/toggle (stock ImsService re-sends IMS on after every SIM load, whatever the modem reports),
//  * NAS voice domain preference read/set with the stock qcril TLV ids (get 0x20/0x1F, set 0x23 only),
//  * the re-assert (kick) policy of a6l-imsdcm.
#include "fake_modem.h"

#include <a6lqmi/ims.h>
#include <a6lqmi/ims_setup.h>
#include <a6lqmi/imsdcm_service.h>
#include <a6lqmi/log.h>
#include <a6lqmi/services.h>

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

struct Lines {
    std::mutex l;
    std::vector<std::string> v;
    radio::ImsReportFn fn() {
        return [this](const std::string& s) {
            std::lock_guard<std::mutex> g(l);
            v.push_back(s);
        };
    }
    bool saw(const std::string& n) {
        std::lock_guard<std::mutex> g(l);
        for (auto& s : v)
            if (s.find(n) != std::string::npos) return true;
        return false;
    }
};

static std::optional<Message> waitFor(FakeModem& fm, MsgType t, uint16_t id, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        auto m = fm.waitModemClientMsg(100);
        if (m && m->type == t && m->msgId == id) return m;
    }
    return std::nullopt;
}

// ------------------------------------------------------------------ 29 Sep replay, byte-exact vs stock
static void testReplay29Sep() {
    EXPECT_EQ(std::string(dcm::instanceName(1)), std::string("GLOBAL"));
    EXPECT_EQ(std::string(dcm::instanceName(2)), std::string("ID1/sub0"));
    EXPECT_EQ(std::string(dcm::instanceName(3)), std::string("ID2/sub1"));
    EXPECT_EQ(std::string(dcm::instanceName(0xff)), std::string("NONE"));
    EXPECT_EQ(std::string(dcm::instanceName(0)), std::string("?"));

    FakeModem fm;
    fm.setHandler([](uint32_t, const Message& m) -> std::optional<Message> { return errResponse(m.msgId, kErrNotSupported); });
    radio::ImsDcmConfig cfg;
    cfg.noWds = true;
    cfg.muxId = 0;
    Lines L;
    std::mutex el;
    std::vector<std::pair<uint16_t, int>> events;
    radio::ImsDcmService svc(cfg, fm.transport(), [&fm](const char* tag) { return std::make_unique<Client>(fm.transport(), tag); });
    svc.onReport(L.fn());
    svc.onModemEvent([&](uint16_t id, std::optional<uint32_t> inst) {
        std::lock_guard<std::mutex> g(el);
        events.push_back({id, inst ? static_cast<int>(*inst) : -1});
    });
    EXPECT(svc.start());
    auto send = [&](uint16_t id, uint16_t txn, std::vector<std::pair<uint8_t, std::string>> tlvs) {
        Message m = Message::request(id);
        for (auto& [t, h] : tlvs) m.raw(t, *unhex(h));
        m.txn = txn;
        fm.modemRequest(dcm::kService, m);
        return waitFor(fm, MsgType::Response, id);
    };
    // exactly the 29 Sep bodies (research/volte6-20260929/volte5-logs/imsdcm.txt)
    std::string la = "8b230100000019" + hex(std::vector<uint8_t>({'f', 'e', '8', '0', ':', ':', '6', '4', 'c', '1', ':', 'f', '4', 'b', '5', ':', '7', '0', 'f', '5', ':', 'a', 'a', 'e', '6'}));
    auto r0 = send(dcm::kLinkAddr, 1, {{0x01, la}});
    auto r1 = send(dcm::kAppStateReq, 2, {{0x10, "01"}, {0x11, "00000000"}});
    auto r2 = send(dcm::kServiceEnableStatus, 3, {{0x01, "0000000000000000"}});
    auto r3 = send(dcm::kSubDestroyInstance, 4, {{0x01, "01000000"}});
    EXPECT(r0 && r1 && r2 && r3);
    // stock wire answers: type 2, txn, msg id, len 7, result TLV 02 0400 00000000 and nothing else
    if (r0) EXPECT_EQ(hex(r0->encode()), std::string("0201002300070002040000000000"));
    if (r1) EXPECT_EQ(hex(r1->encode()), std::string("0202002E00070002040000000000"));
    if (r2) EXPECT_EQ(hex(r2->encode()), std::string("0203003400070002040000000000"));
    if (r3) EXPECT_EQ(hex(r3->encode()), std::string("0204003300070002040000000000"));
    EXPECT(L.saw("A6L_IMSDCM_EVENT destroy instance=1 (GLOBAL)"));
    EXPECT(L.saw("SUB_DESTROY_INSTANCE instance=1 -> ok (stock behaviour)"));
    {
        std::lock_guard<std::mutex> g(el);
        EXPECT_EQ(events.size(), static_cast<size_t>(3));
        if (events.size() == 3) {
            EXPECT(events[0].first == dcm::kAppStateReq && events[0].second == 0);
            EXPECT(events[1].first == dcm::kServiceEnableStatus && events[1].second == -1);
            EXPECT(events[2].first == dcm::kSubDestroyInstance && events[2].second == 1);
        }
    }
    // a later per-subscription PDN request still works after the GLOBAL instance was destroyed
    dcm::PdpActivateReq q;
    q.apn = "ims";
    q.profile = 2;
    q.seq = 5;
    q.instance = dcm::kInstanceId1;
    Message pm = dcm::buildPdpActivateReq(q);
    pm.txn = 9;
    fm.modemRequest(dcm::kService, pm);
    auto pr = waitFor(fm, MsgType::Response, dcm::kPdpActivate);
    EXPECT(pr && pr->resultOk());
    if (pr) EXPECT(pr->get(0x12) && Reader(*pr->get(0x12)).u32() == dcm::kInstanceId1);
    EXPECT(L.saw("inst=2"));
    svc.stop();
}

// ------------------------------------------------------------------ fake modem: IMSS/IMSA/NAS
struct Modem6 {
    std::mutex lock;
    uint8_t imsService = 1, volte = 1;  // 29 Sep: already on
    std::vector<std::pair<int, int>> sets;  // (volte, ims) written; -1 = TLV absent
    uint32_t vdp = 2, usage = 1;
    uint8_t lteSrv = 2, vops = 0;
    uint32_t lteDom = 3;
    std::vector<std::string> nasSets;  // hex of every NAS 0x33 request body
    bool nasSetFails = false;
};

static void install6(FakeModem& fm, Modem6& s) {
    fm.addService(kSvcImss);
    fm.addService(kSvcImsa);
    fm.addService(kSvcNas);
    fm.setHandler([&s](uint32_t svc, const Message& m) -> std::optional<Message> {
        std::lock_guard<std::mutex> l(s.lock);
        if (svc == kSvcImss) {
            if (m.msgId == imss::kBindSubscription) return okResponse(m.msgId);
            if (m.msgId == imss::kGetServiceEnableConfig)
                return okResponse(m.msgId).u8(0x10, 0).u8(0x11, s.volte).u8(0x12, 0).u8(0x15, 0).u8(0x19, s.imsService);
            if (m.msgId == imss::kSetServiceEnableConfig) {
                int v = -1, i = -1;
                if (auto* x = m.get(0x10)) s.volte = static_cast<uint8_t>(v = (*x)[0]);
                if (auto* x = m.get(0x18)) s.imsService = static_cast<uint8_t>(i = (*x)[0]);
                s.sets.push_back({v, i});
                return okResponse(m.msgId);
            }
            return errResponse(m.msgId, kErrNotSupported);
        }
        if (svc == kSvcImsa) {
            if (m.msgId == imsa::kGetRegStatus) return okResponse(m.msgId).u32(0x12, imsa::kNotRegistered);
            if (m.msgId == imsa::kGetServicesStatus) return okResponse(m.msgId);
            return okResponse(m.msgId);
        }
        if (svc == kSvcNas) {
            if (m.msgId == nas::kGetSystemSelectionPreference)
                return okResponse(m.msgId).u32(0x18, 2).u32(0x1F, s.usage).u32(0x20, s.vdp);
            if (m.msgId == nas::kGetSystemInfo) {
                Message r = okResponse(m.msgId);
                r.raw(0x14, {s.lteSrv, s.lteSrv, 0});
                r.u8(0x21, 1).u8(0x29, s.vops).u32(0x2A, s.lteDom);
                return r;
            }
            if (m.msgId == nas::kSetSystemSelectionPreference) {
                Message c = m;
                c.txn = 1;
                s.nasSets.push_back(hex(c.encode()));
                if (s.nasSetFails) return errResponse(m.msgId, kErrInternal);
                if (auto* x = m.get(0x23); x && x->size() >= 4) s.vdp = Reader(*x).u32();
                return okResponse(m.msgId);
            }
        }
        return errResponse(m.msgId, kErrNotSupported);
    });
}

static void testImssForceToggle() {
    FakeModem fm;
    Modem6 s;
    install6(fm, s);
    Client c(fm.transport(), "t");
    EXPECT(c.start({kSvcImss, kSvcImsa, kSvcNas}));
    EXPECT(c.waitForServices({kSvcImss, kSvcImsa, kSvcNas}, 2000).empty());
    // Enable (volte5): already on -> no write (what happened on 29 Sep)
    Lines L0;
    auto e = radio::imssSetup(c, 0u, radio::ImssMode::Enable, L0.fn());
    EXPECT(!e.wrote && e.enabled());
    EXPECT(s.sets.empty());
    // Force: written although already on (stock ImsService/qcril behaviour)
    Lines L1;
    auto f = radio::imssSetup(c, 0u, radio::ImssMode::Force, L1.fn());
    EXPECT(f.wrote && f.writeOk && f.enabled());
    EXPECT((s.sets == std::vector<std::pair<int, int>>{{1, 1}}));
    EXPECT(L1.saw("A6L_IMSDCM_IMSS set volte=1 ims_service_enabled=1: ok"));
    // Toggle: 0 (ims only) then 1 (volte + ims)
    s.sets.clear();
    Lines L2;
    auto t = radio::imssSetup(c, 0u, radio::ImssMode::Toggle, L2.fn(), 10);
    EXPECT(t.wrote && t.writeOk && t.enabled());
    EXPECT((s.sets == std::vector<std::pair<int, int>>{{-1, 0}, {1, 1}}));
    EXPECT(L2.saw("toggle set ims_service_enabled=0: ok"));
    EXPECT(radio::imssModeFromString("force") == radio::ImssMode::Force);
    EXPECT(radio::imssModeFromString("toggle") == radio::ImssMode::Toggle);
    EXPECT_EQ(std::string(radio::imssModeName(radio::ImssMode::Toggle)), std::string("toggle"));
    c.stop();
}

static void testNas() {
    // stock qcril layout: only TLV 0x23 u32
    EXPECT_EQ(enc(radio::buildSetVoiceDomainPref(3)), std::string("0001003300070023040003000000"));
    EXPECT(radio::vdpModeFromString("set") == radio::VdpMode::Set);
    EXPECT(radio::vdpModeFromString("") == radio::VdpMode::Read);
    EXPECT_EQ(std::string(radio::vdpName(3)), std::string("ims-pref"));
    FakeModem fm;
    Modem6 s;
    install6(fm, s);
    Client c(fm.transport(), "t");
    EXPECT(c.start({kSvcNas}));
    EXPECT(c.waitForServices({kSvcNas}, 2000).empty());
    radio::NasImsState st;
    EXPECT(radio::readNasImsState(c, &st).ok());
    EXPECT_EQ(st.summary(), std::string("vdp=2(cs-pref) usage=1(voice) vops=0 lte_voice_domain=3(cs) lte_srv=2"));
    EXPECT(st.lteFullService());
    // read mode: never writes
    Lines L0;
    EXPECT(!radio::nasVoiceDomainSetup(c, radio::VdpMode::Read, L0.fn()));
    EXPECT(s.nasSets.empty());
    EXPECT(L0.saw("A6L_IMSDCM_NAS get vdp=2(cs-pref)"));
    // set mode: one write of exactly the stock request, then read back
    Lines L1;
    EXPECT(radio::nasVoiceDomainSetup(c, radio::VdpMode::Set, L1.fn(), &st));
    EXPECT_EQ(s.nasSets.size(), static_cast<size_t>(1));
    if (!s.nasSets.empty()) EXPECT_EQ(s.nasSets[0], std::string("0001003300070023040003000000"));
    EXPECT(L1.saw("A6L_IMSDCM_NAS set voice_domain_pref=3(ims-pref): ok"));
    EXPECT(L1.saw("get-after vdp=3(ims-pref)"));
    EXPECT(st.vdp && *st.vdp == 3);
    // already 3: no second write
    Lines L2;
    EXPECT(radio::nasVoiceDomainSetup(c, radio::VdpMode::Set, L2.fn()));
    EXPECT_EQ(s.nasSets.size(), static_cast<size_t>(1));
    EXPECT(L2.saw("already 3 (no write)"));
    // failure reported, still not 3
    s.vdp = 0;
    s.nasSetFails = true;
    Lines L3;
    EXPECT(!radio::nasVoiceDomainSetup(c, radio::VdpMode::Set, L3.fn()));
    EXPECT(L3.saw("A6L_IMSDCM_NAS set voice_domain_pref=3(ims-pref): qmi-error INTERNAL"));
    c.stop();
    // service missing -> summary with dashes, no crash
    radio::NasImsState none;
    EXPECT_EQ(none.summary(), std::string("vdp=- usage=- vops=- lte_voice_domain=- lte_srv=-"));
}

static void testKickPolicy() {
    radio::NasImsState full, lim, unknown;
    full.lteSrv = 2;
    lim.lteSrv = 1;
    std::string why;
    {
        radio::ImsKickPolicy k(3, 3);
        EXPECT(!k.due(0, &why));
        k.onDestroy(1, 10);  // 29 Sep: GLOBAL destroyed
        EXPECT(!k.due(12, &why));
        EXPECT(k.due(13, &why));
        EXPECT_EQ(why, std::string("destroy-instance-1"));
        EXPECT(!k.due(14, &why));  // consumed
        EXPECT_EQ(k.kicks(), 1);
    }
    {
        radio::ImsKickPolicy k(3, 3);
        k.onNas(lim, 0);
        EXPECT(!k.due(10, &why));
        k.onNas(full, 20);  // SIM ready + attached
        k.onNas(full, 21);  // still full: no new trigger
        EXPECT(k.due(23, &why));
        EXPECT_EQ(why, std::string("lte-full-service"));
        k.onNas(unknown, 24);  // query failed: keeps state
        k.onNas(full, 25);
        EXPECT(!k.due(40, &why));
        k.onNas(lim, 41);  // lost, back -> again
        k.onNas(full, 42);
        EXPECT(k.due(45, &why));
        EXPECT_EQ(k.kicks(), 2);
    }
    {
        radio::ImsKickPolicy k(2, 3);  // max kicks + merged reasons + spacing
        k.onNas(full, 0);
        k.onDestroy(1, 1);
        EXPECT(k.due(3, &why));
        EXPECT_EQ(why, std::string("lte-full-service+destroy-instance-1"));
        k.onDestroy(2, 3);  // right after a kick: not before t=6
        EXPECT(!k.due(5, &why));
        EXPECT(k.due(6, &why));
        k.onDestroy(3, 7);
        EXPECT(!k.due(100, &why));  // max reached
        EXPECT_EQ(k.kicks(), 2);
    }
}

int main(int argc, char** argv) {
    gLogLevel = (argc > 1 && !strcmp(argv[1], "-v")) ? 4 : 0;
    for (int round = 0; round < 3; round++) {
        testReplay29Sep();
        testImssForceToggle();
        testNas();
        testKickPolicy();
    }
    printf("a6l-volte6 tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
