// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 bug hunt round 2 "radio" (29 Sep 2026): host tests over the real hal/ModemCore.cpp, the
// IMSDCM server logic and HAL source contracts (no phone, no SIM request, no real call).
//  R1: modem (DMS/UIM/NAS) lost while initModem() runs -> the loss must not be overwritten by the end of the init
//      (core must not report ready with indications registered on the dead instance; it re-initializes on return).
//  R2: the "repeated wrong PIN" guard is cleared when the PIN is proven/replaced (PUK unblock, change PIN, SC lock).
//  R3: an unbound slot 2 never reads/writes SIM 1's NAS/WMS settings (network selection, allowed types, SMSC).
//  R4: a6l-imsdcm observes the IMS PDN's WDS session from before START (DISCONNECTED during setup, WDS loss).
// Build/run: tests/run-host-tests.sh (bughunt2 binary).
#define main modemcore_tests_main
#include "modemcore_tests.cc"
#undef main

#include <a6lqmi/ims.h>
#include <a6lqmi/imsdcm_service.h>

#include <fstream>
#include <future>
#include <sstream>
#include <unistd.h>

namespace dcm = a6l::qmi::imsdcm;

static std::atomic<bool> gArmInitLoss{false};
static FakeModem* gM = nullptr;

static std::optional<Message> bhHandler(uint32_t svc, const Message& req) {
    // R1: the modem restarts while the core is still reading its state (last initModem step = VOICE call list)
    if (svc == kSvcVoice && req.msgId == voice::kGetAllCallInfo && gArmInitLoss.exchange(false)) {
        for (auto s : {kSvcDms, kSvcUim, kSvcNas}) gM->removeService(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));  // DEL_SERVER processed before the answer
    }
    return handler(svc, req);
}

static std::string readSrc(const char* name) {
    std::ifstream f(std::string(A6L_HAL_DIR) + "/" + name);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string bodyOf(const std::string& src, const std::string& sig) {
    auto at = src.find(sig);
    if (at == std::string::npos) return {};
    auto end = src.find("\n}\n", at);
    return src.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

// R2 / R3 source contracts (the AIDL HAL files are not host-compiled)
static void halContracts() {
    auto sim = readSrc("RadioSimModemConfig.cpp");
    for (const char* f : {"A6lRadioSim::supplyIccPukForApp(", "A6lRadioSim::changeIccPinForApp(",
                          "A6lRadioSim::setFacilityLockForApp("}) {
        auto b = bodyOf(sim, f);
        EXPECT(!b.empty());
        if (b.find("notePinResult(") == std::string::npos) fprintf(stderr, "R2: %s does not clear the PIN guard\n", f);
        EXPECT(b.find("notePinResult(") != std::string::npos);
    }
    auto net = readSrc("RadioNetworkData.cpp");
    for (const char* f : {"A6lRadioNetwork::getNetworkSelectionMode(", "A6lRadioNetwork::setNetworkSelectionModeAutomatic(",
                          "A6lRadioNetwork::setNetworkSelectionModeManual(", "A6lRadioNetwork::getAllowedNetworkTypesBitmap(",
                          "A6lRadioNetwork::setAllowedNetworkTypesBitmap("}) {
        auto b = bodyOf(net, f);
        EXPECT(!b.empty());
        if (b.find("bound()") == std::string::npos) fprintf(stderr, "R3: %s reaches SIM 1 when slot 2 is unbound\n", f);
        EXPECT(b.find("bound()") != std::string::npos);
        // the guard comes before any NAS request
        auto g = b.find("bound()"), q = b.find("ctl()");
        EXPECT(g != std::string::npos && q != std::string::npos && g < q);
    }
    auto msg = readSrc("RadioMessagingVoice.cpp");
    auto b = bodyOf(msg, "A6lRadioMessaging::getSmscAddress(");
    EXPECT(b.find("bound()") != std::string::npos && b.find("bound()") < b.find("ctl()"));
}

// ------------------------------------------------------------------ R4: IMSDCM WDS observers
struct DcmWds {
    std::mutex lock;
    std::atomic<int> starts{0}, stops{0};
    std::atomic<bool> discInSettings{false};
    FakeModem* fm = nullptr;
};

static std::optional<Message> waitModem(FakeModem& fm, MsgType t, uint16_t id, int ms = 3000) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        auto m = fm.waitModemClientMsg(100);
        if (m && m->type == t && m->msgId == id) return m;
    }
    return std::nullopt;
}

static void dcmActivate(FakeModem& fm, uint32_t seq) {
    dcm::PdpActivateReq q;
    q.apn = "ims";
    q.apnType = dcm::kApnIms;
    q.rat = dcm::kRatLte;
    q.family = 0;
    q.profile = 6;
    q.seq = seq;
    Message m = dcm::buildPdpActivateReq(q);
    m.txn = static_cast<uint16_t>(seq);
    fm.modemRequest(dcm::kService, m);
}

static void imsdcmObservers() {
    // (a) the network releases the PDN between START and the settings answer: the modem must not be told "up"
    {
        auto* fm = new FakeModem();  // leaked: client threads may still reference it briefly
        auto* w = new DcmWds();
        w->fm = fm;
        fm->addService(kSvcWds);
        fm->setHandler([w](uint32_t svc, const Message& m) -> std::optional<Message> {
            if (svc != kSvcWds) return test::errResponse(m.msgId, kErrNotSupported);
            switch (m.msgId) {
                case wds::kStartNetwork: {
                    w->starts++;
                    Message r = test::okResponse(m.msgId);
                    r.u32(0x01, 0x1234);
                    return r;
                }
                case wds::kGetCurrentSettings: {
                    if (w->discInSettings.exchange(false)) {
                        Message ps(MsgType::Indication, wds::kPacketServiceStatus);
                        ps.raw(0x01, {wds::kConnDisconnected, 0});
                        ps.u16(0x10, 2);
                        w->fm->indicate(kSvcWds, ps);
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    Message r = test::okResponse(m.msgId);
                    r.u32(0x1E, 0x0AF5F1A4);
                    return r;
                }
                case wds::kStopNetwork: w->stops++; return test::okResponse(m.msgId);
                default: return test::okResponse(m.msgId);
            }
        });
        std::vector<std::string> lines;
        std::mutex ll;
        auto svc = std::make_unique<radio::ImsDcmService>(radio::ImsDcmConfig{}, fm->transport(),
                                                          [fm](const char* tag) { return std::make_unique<Client>(fm->transport(), tag); });
        svc->onReport([&](const std::string& s) {
            std::lock_guard<std::mutex> g(ll);
            lines.push_back(s);
        });
        EXPECT(svc->start());
        w->discInSettings = true;
        dcmActivate(*fm, 31);
        auto resp = waitModem(*fm, MsgType::Response, dcm::kPdpActivate);
        EXPECT(resp && resp->resultOk());
        auto ind = waitModem(*fm, MsgType::Indication, dcm::kPdpActivate);
        EXPECT(ind.has_value());
        // the PDN is gone: never a lasting success; either a failure IND now, or success followed by the lost IND
        bool failed = ind && !ind->resultOk();
        if (ind && ind->resultOk()) {
            auto lost = waitModem(*fm, MsgType::Indication, dcm::kPdpActivate, 1500);
            failed = lost && !lost->resultOk();
        }
        if (!failed) fprintf(stderr, "R4a: DISCONNECTED during IMS PDN setup was lost (PDN reported up)\n");
        EXPECT(failed);
        for (int i = 0; i < 50 && !svc->pdps().empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT(svc->pdps().empty());
        svc->stop();
    }
    // (b) WDS withdrawn (modem WDS restart) while the IMS PDN is up: the modem must be told the PDN is gone
    {
        auto* fm = new FakeModem();
        fm->addService(kSvcWds);
        fm->setHandler([](uint32_t svc, const Message& m) -> std::optional<Message> {
            if (svc != kSvcWds) return test::errResponse(m.msgId, kErrNotSupported);
            Message r = test::okResponse(m.msgId);
            if (m.msgId == wds::kStartNetwork) r.u32(0x01, 0x4321);
            if (m.msgId == wds::kGetCurrentSettings) r.u32(0x1E, 0x0AF5F1A4);
            return r;
        });
        auto svc = std::make_unique<radio::ImsDcmService>(radio::ImsDcmConfig{}, fm->transport(),
                                                          [fm](const char* tag) { return std::make_unique<Client>(fm->transport(), tag); });
        EXPECT(svc->start());
        dcmActivate(*fm, 41);
        auto resp = waitModem(*fm, MsgType::Response, dcm::kPdpActivate);
        EXPECT(resp && resp->resultOk());
        auto ind = waitModem(*fm, MsgType::Indication, dcm::kPdpActivate);
        EXPECT(ind && ind->resultOk());
        fm->removeService(kSvcWds);
        auto lost = waitModem(*fm, MsgType::Indication, dcm::kPdpActivate, 2000);
        if (!(lost && !lost->resultOk())) fprintf(stderr, "R4b: WDS loss not reported for the IMS PDN\n");
        EXPECT(lost && !lost->resultOk());
        for (int i = 0; i < 50 && !svc->pdps().empty(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
        EXPECT(svc->pdps().empty());
        svc->stop();
    }
}

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    halContracts();
    imsdcmObservers();

    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady}});
    auto* modem = new FakeModem();  // leaked: core threads outlive main
    gM = modem;
    modem->setHandler(bhHandler);
    ModemCore::setTransportFactory([modem] { return modem->transport(); });
    for (auto s : kSvcs) modem->addService(s);
    auto& core = ModemCore::get(1);
    auto* l = new L();
    core.addListener(l);
    auto nasReg = [&] { return count(*modem, kSvcNas, nas::kRegisterIndications); };

    // ---- R1: DMS/UIM/NAS withdrawn during the first initModem()
    gArmInitLoss = true;
    core.start();
    EXPECT(waitFor([&] { return !gArmInitLoss.load(); }, 5000));
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    const int regBefore = nasReg();
    EXPECT_EQ(regBefore, 1);
    if (core.ready()) fprintf(stderr, "R1: core reports ready although the modem vanished during init\n");
    EXPECT(!core.ready());  // the modem is gone: not ready
    for (auto s : {kSvcDms, kSvcUim, kSvcNas}) modem->addService(s);
    bool reinit = waitFor([&] { return nasReg() >= 2 && core.ready(); }, 8000);
    if (!reinit) fprintf(stderr, "R1: no re-initialization after the modem returned (NAS register count %d)\n", nasReg());
    EXPECT(reinit);
    EXPECT(waitFor([&] { return l->ready >= 1; }));

    // ---- R2: behaviour of the guard the HAL relies on (wrong PIN remembered per ICCID, cleared on a proven PIN)
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStatePin}});
    }
    using V = ModemCore::PinVerdict;
    EXPECT(core.pinPreflight("1234", 0).verdict == V::Send);
    core.notePinResult("1234", false);
    EXPECT(core.pinPreflight("1234", 0).verdict == V::RepeatedWrong);
    core.notePinResult("1234", true);  // what the HAL now does after a successful PUK unblock setting PIN 1234
    EXPECT(core.pinPreflight("1234", 0).verdict == V::Send);

    printf("a6l-bughunt2 (radio round 2) tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
