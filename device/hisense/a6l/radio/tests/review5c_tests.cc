// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL, r5 review round5 (28 Sep 2026, F42-F48): host tests. Pure policies (hal/SimNetPolicy.h),
// the real hal/ModemCore.cpp over a fake two-card modem, the real NAS builder, and a source contract for F47.
// No phone, SIM, PIN attempt or network operation: the fake modem never receives a VERIFY/UNBLOCK PIN here.
//  F42 setNumOfLiveModems / enableUiccApplications acknowledge only applied states
//  F43 each subscription's ICCID comes from its own physical card (reversed provisioning, mapping change, failures)
//  F44 PIN1 vs UPIN: the id the guard validated is the id sent; PUK/change/SC resolve the app's effective PIN
//  F45 allowed network types never broadened; applied subset reported
//  F46 MNC width reaches NAS Initiate Network Register (TLV 0x12)
//  F47 A6lRadioSim overrides every inherited emulator APDU/channel method
//  F48 FD facility query not invented
// Build/run: tests/run-host-tests.sh (review5c binary).
#include "../hal/ModemCore.h"
#include "../hal/SimNetPolicy.h"
#include "fake_modem.h"

#include <a6lqmi/log.h>
#include <a6lqmi/multisim.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using android::hardware::radio::a6l::ModemCore;
namespace policy = android::hardware::radio::a6l::policy;
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

static const std::vector<uint8_t> kUsim = {0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02};
static const std::vector<uint8_t> kIsim = {0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x04};

struct CardSpec {
    uint8_t state = uim::kCardPresent;
    uint8_t appState = uim::kAppStateReady;
    bool upin = false;  // USIM: UPIN replaces PIN1
    uint8_t upinState = 0, upinRetries = 0;
    bool isim = false;  // second app (ISIM, PIN1)
};
static void u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}
static void app(std::vector<uint8_t>& v, uint8_t type, uint8_t st, const std::vector<uint8_t>& aid, bool upin) {
    v.insert(v.end(), {type, st, 0, 0, 0, 0});
    v.push_back(static_cast<uint8_t>(aid.size()));
    v.insert(v.end(), aid.begin(), aid.end());
    uint8_t pin1 = st == uim::kAppStatePin ? uim::kPinEnabledNotVerified : uim::kPinDisabled;
    v.insert(v.end(), {static_cast<uint8_t>(upin ? 1 : 0), pin1, 3, 10, uim::kPinEnabledNotVerified, 3, 10});
}
static std::vector<uint8_t> cardTlv(uint16_t gwPrimary, uint16_t gwSecondary, const std::vector<CardSpec>& cards) {
    std::vector<uint8_t> v;
    u16le(v, gwPrimary);
    u16le(v, 0xFFFF);
    u16le(v, gwSecondary);
    u16le(v, 0xFFFF);
    v.push_back(static_cast<uint8_t>(cards.size()));
    for (auto& c : cards) {
        v.insert(v.end(), {c.state, c.upinState, c.upinRetries, 10, 0});
        if (c.state != uim::kCardPresent) {
            v.push_back(0);
            continue;
        }
        v.push_back(c.isim ? 2 : 1);
        app(v, uim::kAppUsim, c.appState, kUsim, c.upin);
        if (c.isim) app(v, uim::kAppIsim, c.appState, kIsim, false);
    }
    return v;
}

// ------------------------------------------------------------------ pure checks
static void policyChecks() {
    // F42
    EXPECT(policy::liveModems(2, 2) == policy::Verdict::Accept);
    EXPECT(policy::liveModems(1, 2) == policy::Verdict::NotSupported);  // was: success, getter still 2
    EXPECT(policy::liveModems(1, 1) == policy::Verdict::Accept);
    EXPECT(policy::liveModems(2, 1) == policy::Verdict::InvalidArguments);
    EXPECT(policy::liveModems(0, 2) == policy::Verdict::InvalidArguments);
    EXPECT(policy::liveModems(-1, 2) == policy::Verdict::InvalidArguments);
    EXPECT(policy::uiccApplications(true) == policy::Verdict::Accept);
    EXPECT(policy::uiccApplications(false) == policy::Verdict::NotSupported);  // was: success + nothing applied

    // F45
    namespace raf = policy::raf;
    EXPECT(!policy::allowedModes(0).has_value());            // was: mode 0x1c
    EXPECT(!policy::allowedModes(1 << 20).has_value());      // NR only, was: mode 0x1c
    EXPECT(!policy::allowedModes((1 << 6) | (1 << 7)).has_value());  // CDMA only
    auto m = policy::allowedModes(raf::kLte | raf::kLteCa);
    EXPECT(m && m->modeMask == nas::kModeLte && m->applied == (raf::kLte | raf::kLteCa));
    m = policy::allowedModes(raf::kUmts | raf::kHspap);
    EXPECT(m && m->modeMask == nas::kModeUmts && m->applied == (raf::kUmts | raf::kHspap));
    m = policy::allowedModes(raf::kLte | (1 << 20));  // LTE + NR: only LTE applied / reported
    EXPECT(m && m->modeMask == nas::kModeLte && m->applied == raf::kLte);
    m = policy::allowedModes(raf::kGsmFamily | raf::kUmtsFamily | raf::kLteFamily | (1 << 20));
    EXPECT(m && m->modeMask == (nas::kModeGsm | nas::kModeUmts | nas::kModeLte) && m->applied == raf::kSupported);
    m = policy::allowedModes(raf::kGprs);
    EXPECT(m && m->modeMask == nas::kModeGsm);
    EXPECT_EQ(policy::bitmapForModes(nas::kModeLte), raf::kLteFamily);
    EXPECT_EQ(policy::bitmapForModes(nas::kModeGsm | nas::kModeUmts), raf::kGsmFamily | raf::kUmtsFamily);
    EXPECT_EQ(policy::bitmapForModes(0), 0);

    // F46
    auto p = policy::parsePlmn("00101");
    EXPECT(p && p->mcc == 1 && p->mnc == 1 && !p->mncThreeDigits);
    auto q = policy::parsePlmn("001001");
    EXPECT(q && q->mcc == 1 && q->mnc == 1 && q->mncThreeDigits);
    p = policy::parsePlmn("310260");
    EXPECT(p && p->mcc == 310 && p->mnc == 260 && p->mncThreeDigits);
    p = policy::parsePlmn("20801");
    EXPECT(p && p->mcc == 208 && p->mnc == 1 && !p->mncThreeDigits);
    for (const char* bad : {"", "0010", "0010011", "00a01", "001 1", "-0101", "+00101"})
        EXPECT(!policy::parsePlmn(bad).has_value());

    // F48
    EXPECT(policy::facilityQuery("SC") == policy::FacilityQuery::SimPinLock);
    EXPECT(policy::facilityQuery("FD") == policy::FacilityQuery::NotSupported);  // was: success, "disabled"
    EXPECT(policy::facilityQuery("AO") == policy::FacilityQuery::NotSupported);
    EXPECT(policy::facilityQuery("") == policy::FacilityQuery::NotSupported);
}

static void pinChecks() {
    // F44: UPIN replaces PIN1, UPIN needs verification with 3 attempts, application PIN1 disabled (review repro)
    CardSpec c;
    c.appState = uim::kAppStatePin;
    c.upin = true;
    c.upinState = uim::kPinEnabledNotVerified;
    c.upinRetries = 3;
    c.isim = true;
    auto cs = *uim::parseCardStatus(cardTlv(0x0000, 0xFFFF, {c}));
    auto k = ModemCore::checkPin(cs, 0, "1234");
    EXPECT(k.verdict == V::Send);
    EXPECT_EQ(k.retries, 3);
    EXPECT_EQ(k.pinId, uim::kUpin);  // was: the HAL then sent PIN id 1
    EXPECT(ModemCore::pinIdFor(cs, 0, "") == std::optional<uint8_t>(uim::kUpin));
    EXPECT(ModemCore::pinIdFor(cs, 0, hex(kUsim)) == std::optional<uint8_t>(uim::kUpin));
    EXPECT(ModemCore::pinIdFor(cs, 0, "a0000000871002") == std::optional<uint8_t>(uim::kUpin));  // lower case
    EXPECT(ModemCore::pinIdFor(cs, 0, hex(kIsim)) == std::optional<uint8_t>(uim::kPin1));  // ISIM keeps PIN1
    EXPECT(!ModemCore::pinIdFor(cs, 0, "A0000000871009").has_value());  // unknown app: nothing sent
    EXPECT(!ModemCore::pinIdFor(cs, 0, "A00").has_value());             // malformed AID
    c.upinRetries = 0;  // blocked UPIN: guard refuses whatever the id
    cs = *uim::parseCardStatus(cardTlv(0x0000, 0xFFFF, {c}));
    k = ModemCore::checkPin(cs, 0, "1234");
    EXPECT(k.verdict == V::Blocked);
    EXPECT_EQ(k.retries, 0);
    // plain PIN1 card
    CardSpec d;
    d.appState = uim::kAppStatePin;
    cs = *uim::parseCardStatus(cardTlv(0x0000, 0xFFFF, {d}));
    k = ModemCore::checkPin(cs, 0, "1234");
    EXPECT(k.verdict == V::Send);
    EXPECT_EQ(k.pinId, uim::kPin1);
    EXPECT(ModemCore::pinIdFor(cs, 0, "") == std::optional<uint8_t>(uim::kPin1));
    // absent card: nothing to address
    CardSpec a;
    a.state = uim::kCardAbsent;
    cs = *uim::parseCardStatus(cardTlv(0xFFFF, 0xFFFF, {a}));
    EXPECT(!ModemCore::pinIdFor(cs, 0, "").has_value());
    // secondary subscription on card 2 with UPIN, primary on card 1 PIN1
    CardSpec u = c;
    u.upinRetries = 2;
    cs = *uim::parseCardStatus(cardTlv(0x0000, 0x0100, {d, u}));
    EXPECT(ModemCore::pinIdFor(cs, 0, "") == std::optional<uint8_t>(uim::kPin1));
    EXPECT(ModemCore::pinIdFor(cs, 1, "") == std::optional<uint8_t>(uim::kUpin));
    k = ModemCore::checkPin(cs, 1, "1234");
    EXPECT(k.verdict == V::Send && k.pinId == uim::kUpin && k.retries == 2);
}

// F47: the inherited libminradio RadioSim answers channels/APDUs from its emulator (fatal CHECKs on ordinary input).
// A6lRadioSim must override all four so none of them can reach it.
static void apduContract() {
    std::ifstream f(std::string(A6L_HAL_DIR) + "/RadioImpl.h");
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    auto b = s.find("class A6lRadioSim ");
    auto e = s.find("class A6lRadioNetwork ");
    EXPECT(b != std::string::npos && e != std::string::npos && b < e);
    if (b == std::string::npos || e == std::string::npos) return;
    std::string cls = s.substr(b, e - b);
    for (const char* m : {"iccOpenLogicalChannel(", "iccCloseLogicalChannelWithSessionInfo(", "iccTransmitApduBasicChannel(",
                          "iccTransmitApduLogicalChannel(", "enableUiccApplications(", "areUiccApplicationsEnabled(",
                          "getFacilityLockForApp(", "iccIoForApp("})
        EXPECT(cls.find(m) != std::string::npos);
    std::ifstream g(std::string(A6L_HAL_DIR) + "/RadioSimModemConfig.cpp");
    std::stringstream gs;
    gs << g.rdbuf();
    std::string src = gs.str();
    for (const char* m : {"A6lRadioSim::iccOpenLogicalChannel(", "A6lRadioSim::iccCloseLogicalChannelWithSessionInfo(",
                          "A6lRadioSim::iccTransmitApduBasicChannel(", "A6lRadioSim::iccTransmitApduLogicalChannel("}) {
        auto at = src.find(m);
        EXPECT(at != std::string::npos);
        if (at == std::string::npos) continue;
        auto body = src.substr(at, src.find("\n}\n", at) - at);
        EXPECT(body.find("REQUEST_NOT_SUPPORTED") != std::string::npos);
        EXPECT(body.find("mAppManager") == std::string::npos);
    }
    EXPECT(src.find("mAppManager") == std::string::npos);
    EXPECT(src.find("minimal::RadioSim::icc") == std::string::npos);
}

// ------------------------------------------------------------------ fake two-card modem (F43 / F44 live / F46 wire)
struct State {
    std::mutex lock;
    std::vector<uint8_t> cardTlv;
    bool failCard1 = false, failCard2 = false;
};
static State gS;
static FakeModem* gModem = nullptr;

static std::vector<uint8_t> iccidEf(uint8_t digit) { return std::vector<uint8_t>(10, digit); }

static std::optional<Message> handler(uint32_t svc, const Message& req) {
    std::lock_guard<std::mutex> l(gS.lock);
    auto ok = [&] { return test::okResponse(req.msgId); };
    switch (svc) {
        case kSvcDms:
            if (req.msgId == dms::kGetIds) return ok().strNoLen(0x11, "865947040141299");
            if (req.msgId == dms::kGetRevision) return ok().strNoLen(0x01, "MPSS.AT.3.1-00819");
            if (req.msgId == dms::kGetOperatingMode) return ok().u8(0x01, dms::kLowPower);
            if (req.msgId == dms::kUimGetIccid) return ok().strNoLen(0x01, "99999999999999999999");  // ambiguous
            return ok();
        case kSvcUim:
            if (req.msgId == uim::kGetCardStatus) return ok().raw(0x10, gS.cardTlv);
            if (req.msgId == uim::kReadTransparent) {
                auto* s = req.get(0x01);
                uint8_t session = s && !s->empty() ? (*s)[0] : 0xff;
                if (session == multisim::kSessionCardSlot1 && !gS.failCard1) {
                    auto ef = iccidEf(0x11);
                    std::vector<uint8_t> v{static_cast<uint8_t>(ef.size()), 0};
                    v.insert(v.end(), ef.begin(), ef.end());
                    return ok().raw(0x11, v);
                }
                if (session == multisim::kSessionCardSlot2 && !gS.failCard2) {
                    auto ef = iccidEf(0x22);
                    std::vector<uint8_t> v{static_cast<uint8_t>(ef.size()), 0};
                    v.insert(v.end(), ef.begin(), ef.end());
                    return ok().raw(0x11, v);
                }
                return test::errResponse(req.msgId, kErrSimFileNotFound);
            }
            return ok();
        default:
            return ok();
    }
}
static void setCards(std::vector<uint8_t> tlv, bool indicate = true) {
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.cardTlv = tlv;
    }
    if (indicate) {
        Message ind(MsgType::Indication, uim::kCardStatusInd);
        ind.raw(0x10, tlv);
        gModem->indicate(kSvcUim, ind);
    }
}
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

static const std::string k1 = "11111111111111111111", k2 = "22222222222222222222";

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "2");
    policyChecks();
    pinChecks();
    apduContract();

    CardSpec ready;
    // F43 review repro: primary subscription provisioned from physical card 2, secondary from card 1
    gS.cardTlv = cardTlv(0x0100, 0x0000, {ready, ready});
    gModem = new FakeModem();  // leaked: core threads outlive main
    gModem->setHandler(handler);
    ModemCore::setTransportFactory([] { return gModem->transport(); });
    for (auto s : {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda}) gModem->addService(s);
    auto& c1 = ModemCore::get(1);
    auto& c2 = ModemCore::get(2);
    c1.start();
    c2.start();
    EXPECT(waitFor([&] { return c1.ready() && c2.ready(); }));
    EXPECT_EQ(c1.slotSim(true).card, 1);
    EXPECT_EQ(c2.slotSim(true).card, 0);
    EXPECT_EQ(c1.iccid(true), k2);  // was: 1111 (card 1) for both
    EXPECT_EQ(c2.iccid(true), k1);

    // normal provisioning after a mapping change (indication): each follows its card
    setCards(cardTlv(0x0000, 0x0100, {ready, ready}));
    EXPECT(waitFor([&] { return c1.slotSim().card == 0 && c2.slotSim().card == 1; }));
    EXPECT(waitFor([&] { return c1.iccid(true) == k1 && c2.iccid(true) == k2; }));

    // mapping moves primary to card 2 whose EF_ICCID read fails: the old card-1 identity is dropped, and the DMS
    // (not tied to card 2) is not used
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.failCard2 = true;
    }
    setCards(cardTlv(0x0100, 0x0000, {ready, ready}));
    EXPECT(waitFor([&] { return c1.slotSim().card == 1; }));
    EXPECT(waitFor([&] { return c1.iccid(true).empty(); }));
    EXPECT(c1.iccid(true) != "99999999999999999999");
    // a failed re-read of the SAME card keeps its identity (transient error)
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.failCard2 = false;
    }
    EXPECT_EQ(c1.iccid(true), k2);
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.failCard2 = true;
    }
    EXPECT_EQ(c1.iccid(true), k2);
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.failCard2 = false;
    }
    // primary on physical card 1 with a failing EF read: v1 DMS fallback still allowed (same card)
    setCards(cardTlv(0x0000, 0xFFFF, {ready, CardSpec{uim::kCardAbsent}}));
    EXPECT(waitFor([&] { return c1.slotSim().card == 0; }));
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.failCard1 = true;
    }
    EXPECT_EQ(c1.iccid(true), "99999999999999999999");
    {
        std::lock_guard<std::mutex> l(gS.lock);
        gS.failCard1 = false;
    }
    EXPECT_EQ(c1.iccid(true), k1);
    // one card missing: that subscription has no identity
    setCards(cardTlv(0xFFFF, 0x0000, {ready, CardSpec{uim::kCardAbsent}}));
    EXPECT(waitFor([&] { return c1.slotSim().card == 1; }));
    EXPECT(waitFor([&] { return c1.iccid(true).empty(); }));
    EXPECT_EQ(c2.iccid(true), k1);

    // F44 live: fresh card status (UPIN replaces PIN1 on the primary's card) -> the guard's id is UPIN
    CardSpec up;
    up.appState = uim::kAppStatePin;
    up.upin = true;
    up.upinState = uim::kPinEnabledNotVerified;
    up.upinRetries = 3;
    setCards(cardTlv(0x0000, 0x0100, {up, ready}));
    auto chk = c1.pinPreflight("1234", 0);
    EXPECT(chk.verdict == V::Send && chk.pinId == uim::kUpin && chk.retries == 3);
    EXPECT(c1.pinTarget("") == std::optional<uint8_t>(uim::kUpin));
    EXPECT(c2.pinTarget("") == std::optional<uint8_t>(uim::kPin1));
    EXPECT(!c1.pinTarget("A0000000871009").has_value());
    // card replaced by a PIN1 card: the next resolution follows the new card
    CardSpec p1;
    p1.appState = uim::kAppStatePin;
    setCards(cardTlv(0x0000, 0x0100, {p1, ready}));
    EXPECT(c1.pinTarget("") == std::optional<uint8_t>(uim::kPin1));
    chk = c1.pinPreflight("1234", 0);
    EXPECT(chk.verdict == V::Send && chk.pinId == uim::kPin1);
    // nothing in this test ever sends a credential
    EXPECT_EQ(count(kSvcUim, uim::kVerifyPin), 0);
    EXPECT_EQ(count(kSvcUim, uim::kUnblockPin), 0);
    EXPECT_EQ(count(kSvcUim, uim::kChangePin), 0);
    EXPECT_EQ(count(kSvcUim, uim::kSetPinProtection), 0);

    // F46 wire: 00101 and 001001 differ on the wire (TLV 0x12), same 0x10
    auto sel = [&](const char* op) -> std::optional<Message> {
        auto p = policy::parsePlmn(op);
        if (!p) return std::nullopt;
        nas::setNetworkSelection(c1.ctl(), true, p->mcc, p->mnc, nas::kRifLte, p->mncThreeDigits);
        return last(kSvcNas, nas::kInitiateNetworkRegister);
    };
    auto a = sel("00101"), b = sel("001001");
    EXPECT(a && b && a->get(0x10) && b->get(0x10) && *a->get(0x10) == *b->get(0x10));
    EXPECT(a && a->get(0x12) && *a->get(0x12) == std::vector<uint8_t>{0});
    EXPECT(b && b->get(0x12) && *b->get(0x12) == std::vector<uint8_t>{1});
    EXPECT(a && a->get(0x11) && *a->get(0x11) == std::vector<uint8_t>{1});
    nas::setNetworkSelection(c1.ctl(), false, 0, 0, 0);
    auto au = last(kSvcNas, nas::kInitiateNetworkRegister);
    EXPECT(au && au->get(0x01) && *au->get(0x01) == std::vector<uint8_t>{1} && !au->get(0x10) && !au->get(0x12));

    printf("A6L_REVIEW5C_TESTS %s (%d passed, %d failed)\n", gFail ? "FAIL" : "PASS", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
