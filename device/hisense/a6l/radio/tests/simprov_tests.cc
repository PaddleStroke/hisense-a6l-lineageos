// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (pinsafe, 27 Sep 2026): host tests of the SIM provisioning + PIN guard in the
// real hal/ModemCore.cpp over the fake QRTR modem.
//  - checkPin(): the PIN goes to the card only for app state PIN with >= 1 attempt and 4-8 digits
//  - slot 1: the modem left gw_primary=0xffff with the USIM app DETECTED (seen on 27 Sep) -> the core
//    sends UIM Change Provisioning Session (primary GW, slot 1, the app's AID) by itself
//  - a PIN the card rejected is never sent again automatically (per ICCID)
//  - LOW_POWER -> ONLINE powers the SIM down: the core re-provisions and re-reports the SIM state,
//    and never sends a PIN by itself
//  - provisioning is rate limited when the modem ignores it
// Build/run: tests/run-host-tests.sh (simprov binary).
#include "../hal/ModemCore.h"
#include "fake_modem.h"

#include <a6lqmi/log.h>
#include <a6lqmi/multisim.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <unistd.h>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using android::hardware::radio::a6l::ModemCore;
using V = ModemCore::PinVerdict;

static int gFail = 0, gPass = 0;
#define EXPECT(c)                                                                  \
    do {                                                                           \
        if (c) {                                                                   \
            gPass++;                                                               \
        } else {                                                                   \
            gFail++;                                                               \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);           \
        }                                                                          \
    } while (0)
#define EXPECT_EQ(a, b) EXPECT((a) == (b))

static const std::vector<uint8_t> kAid = {0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02, 0xFF, 0x33};

struct Sim {
    uint8_t cardState = uim::kCardPresent;
    uint16_t gwPrimary = 0xFFFF;
    uint8_t appState = uim::kAppStateDetected;
    uint8_t pin1State = uim::kPinEnabledNotVerified, pin1Retries = 3, puk1Retries = 10;
    bool upin = false;
    uint8_t upinState = 0, upinRetries = 0;
};

static void u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}
static std::vector<uint8_t> cardTlv(const Sim& s) {
    std::vector<uint8_t> v;
    u16le(v, s.gwPrimary);
    u16le(v, 0xFFFF);
    u16le(v, 0xFFFF);
    u16le(v, 0xFFFF);
    v.push_back(1);  // one card
    v.insert(v.end(), {s.cardState, s.upinState, s.upinRetries, 10, 0});
    if (s.cardState != uim::kCardPresent) {
        v.push_back(0);
        return v;
    }
    v.push_back(1);  // one app
    v.insert(v.end(), {uim::kAppUsim, s.appState, 0, 0, 0, 0});
    v.push_back(static_cast<uint8_t>(kAid.size()));
    v.insert(v.end(), kAid.begin(), kAid.end());
    v.insert(v.end(), {static_cast<uint8_t>(s.upin ? 1 : 0), s.pin1State, s.pin1Retries, s.puk1Retries,
                       uim::kPinEnabledNotVerified, 3, 10});
    return v;
}
static uim::CardStatus parsed(const Sim& s) { return *uim::parseCardStatus(cardTlv(s)); }

// ------------------------------------------------------------------ fake modem
struct State {
    std::mutex lock;
    Sim sim;
    uint8_t opMode = dms::kLowPower;
    bool provisionAccepted = true;
    bool pinEnabled = true;
};
static State gS;
static FakeModem* gModem = nullptr;

static void indicateCard() {
    std::vector<uint8_t> t;
    {
        std::lock_guard<std::mutex> l(gS.lock);
        t = cardTlv(gS.sim);
    }
    Message ind(MsgType::Indication, uim::kCardStatusInd);
    ind.raw(0x10, t);
    gModem->indicate(kSvcUim, ind);
}

static std::optional<Message> handler(uint32_t svc, const Message& req) {
    bool sendInd = false;
    std::optional<Message> resp;
    {
        std::lock_guard<std::mutex> l(gS.lock);
        auto ok = [&] { return test::okResponse(req.msgId); };
        switch (svc) {
            case kSvcDms:
                if (req.msgId == dms::kGetIds) resp = ok().strNoLen(0x11, "865947040141299");
                else if (req.msgId == dms::kGetRevision) resp = ok().strNoLen(0x01, "MPSS.AT.3.1-00819");
                else if (req.msgId == dms::kGetOperatingMode) resp = ok().u8(0x01, gS.opMode);
                else if (req.msgId == dms::kSetOperatingMode) {
                    auto* t = req.get(0x01);
                    uint8_t m = t && !t->empty() ? (*t)[0] : gS.opMode;
                    if (m != gS.opMode) {
                        // the A6L modem powers the SIM down in LOW_POWER and does NOT re-provision
                        // the primary session when it comes back online
                        gS.sim.gwPrimary = 0xFFFF;
                        gS.sim.appState = uim::kAppStateDetected;
                        gS.sim.pin1State = uim::kPinEnabledNotVerified;
                        sendInd = true;
                    }
                    gS.opMode = m;
                    resp = ok();
                } else if (req.msgId == dms::kUimGetIccid) resp = ok().strNoLen(0x01, "8933010000000000042");
                else resp = ok();
                break;
            case kSvcUim:
                if (req.msgId == uim::kGetCardStatus) resp = ok().raw(0x10, cardTlv(gS.sim));
                else if (req.msgId == multisim::kUimChangeProvisioningSession) {
                    if (gS.provisionAccepted) {
                        gS.sim.gwPrimary = 0x0000;
                        gS.sim.appState = gS.pinEnabled ? uim::kAppStatePin : uim::kAppStateReady;
                        sendInd = true;
                    }
                    resp = ok();
                } else if (req.msgId == uim::kReadTransparent) resp = test::errResponse(req.msgId, kErrSimFileNotFound);
                else resp = ok();
                break;
            default:
                resp = ok();
        }
    }
    if (sendInd)
        std::thread([] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            indicateCard();
        }).detach();
    return resp;
}

struct L : ModemCore::Listener {
    std::atomic<int> ready{0}, sim{0};
    void onModemReady() override { ready++; }
    void onSimChanged() override { sim++; }
};

template <typename F>
static bool waitFor(F f, int ms = 8000) {
    for (int i = 0; i < ms / 20; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}
static int count(uint32_t svc, uint16_t id) {
    int n = 0;
    for (auto& [s, msg] : gModem->requests())
        if (s == svc && msg.msgId == id) n++;
    return n;
}
static std::optional<Message> last(uint32_t svc, uint16_t id) {
    std::optional<Message> r;
    for (auto& [s, msg] : gModem->requests())
        if (s == svc && msg.msgId == id) r = msg;
    return r;
}

static void pureChecks() {
    Sim s;
    s.cardState = uim::kCardAbsent;
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::NoCard);
    s = Sim{};  // present, gw_primary 0xffff, DETECTED (27 Sep)
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::NotProvisioned);
    s.gwPrimary = 0x0000;
    s.appState = uim::kAppStatePin;
    auto c = ModemCore::checkPin(parsed(s), 0, "1234");
    EXPECT(c.verdict == V::Send);
    EXPECT_EQ(c.retries, 3);
    s.pin1Retries = 1;  // last attempt: still allowed (the user decides)
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::Send);
    s.pin1Retries = 0;  // no attempt left: never sent, PASSWORD_INCORRECT with 0 left
    c = ModemCore::checkPin(parsed(s), 0, "1234");
    EXPECT(c.verdict == V::Blocked);
    EXPECT_EQ(c.retries, 0);
    s.pin1Retries = 3;
    s.appState = uim::kAppStatePuk;
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::Blocked);
    s.appState = uim::kAppStatePin;
    s.pin1State = uim::kPinBlocked;
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::Blocked);
    s.pin1State = uim::kPinEnabledNotVerified;
    s.appState = uim::kAppStateReady;
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::AlreadyReady);
    s.appState = uim::kAppStateDetected;  // provisioned but not asking for a PIN yet
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::NotPinState);
    s.appState = uim::kAppStatePin;
    for (const char* bad : {"123", "123456789", "12a4", "", "12 34"})
        EXPECT(ModemCore::checkPin(parsed(s), 0, bad).verdict == V::BadFormat);
    EXPECT(ModemCore::checkPin(parsed(s), 0, "00000000").verdict == V::Send);
    s.upin = true;  // UPIN replaces PIN1: the card's UPIN counters count
    s.upinState = uim::kPinEnabledNotVerified;
    s.upinRetries = 0;
    EXPECT(ModemCore::checkPin(parsed(s), 0, "1234").verdict == V::Blocked);
    s.upinRetries = 2;
    c = ModemCore::checkPin(parsed(s), 0, "1234");
    EXPECT(c.verdict == V::Send);
    EXPECT_EQ(c.retries, 2);
}

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    pureChecks();

    gModem = new FakeModem();  // leaked: core threads outlive main
    gModem->setHandler(handler);
    ModemCore::setTransportFactory([] { return gModem->transport(); });
    for (auto s : {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda}) gModem->addService(s);

    auto& core = ModemCore::get(1);
    auto* l = new L();
    core.addListener(l);
    core.start();
    EXPECT(waitFor([&] { return core.ready() && l->ready >= 1; }));

    // ---- 27 Sep: gw_primary=0xffff + DETECTED -> the core activates the primary session itself
    EXPECT(waitFor([&] { return count(kSvcUim, multisim::kUimChangeProvisioningSession) >= 1; }));
    auto cp = last(kSvcUim, multisim::kUimChangeProvisioningSession);
    EXPECT(cp && cp->get(0x01) && *cp->get(0x01) == (std::vector<uint8_t>{multisim::kSessionPrimaryGw, 1}));
    std::vector<uint8_t> app{1, static_cast<uint8_t>(kAid.size())};
    app.insert(app.end(), kAid.begin(), kAid.end());
    EXPECT(cp && cp->get(0x10) && *cp->get(0x10) == app);
    EXPECT(waitFor([&] {
        auto s = core.slotSim();
        return s.provisioned && s.cardInfo && s.cardInfo->apps.size() == 1 && s.cardInfo->apps[0].state == uim::kAppStatePin;
    }));
    EXPECT(waitFor([&] { return l->sim >= 1; }));

    // ---- PIN guard on the live core
    auto p = core.pinPreflight("1234");
    EXPECT(p.verdict == V::Send);
    EXPECT_EQ(p.retries, 3);
    core.notePinResult("1234", false);  // the card said INCORRECT_PIN
    EXPECT(core.pinPreflight("1234").verdict == V::RepeatedWrong);  // never re-sent automatically
    EXPECT(core.pinPreflight("5678").verdict == V::Send);           // a new PIN typed by the user
    core.notePinResult("5678", true);
    EXPECT(core.pinPreflight("1234").verdict == V::Send);           // cleared after success
    EXPECT(core.pinPreflight("12").verdict == V::BadFormat);
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.sim.pin1Retries = 0;
    }
    p = core.pinPreflight("1234");
    EXPECT(p.verdict == V::Blocked);
    EXPECT_EQ(p.retries, 0);
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.sim.pin1Retries = 3;
    }

    // ---- LOW_POWER -> ONLINE: SIM powered down, the modem does not re-provision; the core does,
    //      re-reports the SIM (PIN needed again) and never sends a PIN by itself
    int prov0 = count(kSvcUim, multisim::kUimChangeProvisioningSession);
    EXPECT(core.setRadioPower(true));
    EXPECT(waitFor([&] { return count(kSvcUim, multisim::kUimChangeProvisioningSession) > prov0; }));
    int prov1 = count(kSvcUim, multisim::kUimChangeProvisioningSession);
    int sim1 = l->sim;
    EXPECT(core.setRadioPower(false));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT(core.setRadioPower(true));
    EXPECT(waitFor([&] { return count(kSvcUim, multisim::kUimChangeProvisioningSession) > prov1; }));
    EXPECT(waitFor([&] { return l->sim >= sim1 + 2; }, 4000));  // indication + the delayed re-check
    EXPECT(waitFor([&] { return core.pinPreflight("4321").verdict == V::Send; }));  // PIN state visible again
    EXPECT_EQ(count(kSvcUim, uim::kVerifyPin), 0);  // the core itself never sent a PIN

    // ---- modem ignores provisioning: rate limited (5 s apart), no storm on repeated indications
    std::this_thread::sleep_for(std::chrono::milliseconds(6500));  // let the delayed re-checks finish
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.provisionAccepted = false;
        gS.sim.gwPrimary = 0xFFFF;
        gS.sim.appState = uim::kAppStateDetected;
    }
    int prov2 = count(kSvcUim, multisim::kUimChangeProvisioningSession);
    for (int i = 0; i < 6; i++) {
        indicateCard();
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    int burst = count(kSvcUim, multisim::kUimChangeProvisioningSession) - prov2;
    EXPECT(burst >= 1 && burst <= 1);
    EXPECT(core.pinPreflight("1234").verdict == V::NotProvisioned);  // forced once more, still refused
    EXPECT_EQ(count(kSvcUim, uim::kVerifyPin), 0);

    printf("A6L_SIMPROV_TESTS %s (%d passed, %d failed)\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
