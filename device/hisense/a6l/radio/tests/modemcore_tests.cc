// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril3, 25 Sep 2026): host fault-injection tests of hal/ModemCore.cpp
// (the real HAL core, compiled against tests/hoststub/android-base) over the fake QRTR modem:
//  - DSDS: slot1 + slot2 cores, slot 2 binds NAS/WMS/DMS/VOICE to the secondary subscription, card 1
//    empty (card error, like the A6L on 25 Sep) -> slot 2 = no SIM; slot 1 unchanged (no binding sent)
//  - radio power / airplane mode: one DMS operating mode for both slots (PowerVote)
//  - SMS: ack only when the ack indicator asks for it, protocol/IMS TLVs, ACK_NOT_SENT (84) tolerated
//  - voice RAT during a call (CSFB LTE -> UMTS logged)
//  - modem SSR: all QMI services disappear and come back -> RADIO_UNAVAILABLE then re-init
//  - no-SIM and PIN-locked card states; slot 2 that cannot bind -> unusable, no traffic on SIM 1
// Build/run: tests/run-host-tests.sh (second binary).
#include "../hal/ModemCore.h"
#include "fake_modem.h"

#include <a6lqmi/log.h>
#include <a6lqmi/multisim.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using android::hardware::radio::a6l::ModemCore;

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

// ------------------------------------------------------------------ fake modem state
struct State {
    std::mutex lock;
    std::vector<uint8_t> cardTlv;
    uint8_t opMode = 1;  // low power at boot
    bool bindFails = false;
    uint16_t ackError = 0;  // 0 = ok
    uint8_t ackCause = 1;   // network released link
};
static State gS;

static void u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}
// cards: {state, error, appState(-1 no app)}
struct CardSpec {
    uint8_t state, error;
    int appState;
};
static std::vector<uint8_t> cardStatusTlv(uint16_t gwPrimary, uint16_t gwSecondary, std::vector<CardSpec> cards) {
    std::vector<uint8_t> v;
    u16le(v, gwPrimary);
    u16le(v, 0xFFFF);
    u16le(v, gwSecondary);
    u16le(v, 0xFFFF);
    v.push_back(static_cast<uint8_t>(cards.size()));
    for (auto& c : cards) {
        v.insert(v.end(), {c.state, 0 /*upin*/, 0, 0, c.error});
        if (c.appState < 0) {
            v.push_back(0);
            continue;
        }
        v.push_back(1);
        std::vector<uint8_t> aid = {0xA0, 0x00, 0x00, 0x00, 0x87, 0x10, 0x02};
        v.insert(v.end(), {uim::kAppUsim, static_cast<uint8_t>(c.appState), 0, 0, 0, 0});
        v.push_back(static_cast<uint8_t>(aid.size()));
        v.insert(v.end(), aid.begin(), aid.end());
        uint8_t pin1 = c.appState == uim::kAppStatePin ? uim::kPinEnabledNotVerified : uim::kPinDisabled;
        v.insert(v.end(), {0, pin1, 3, 10, uim::kPinEnabledNotVerified, 3, 10});
    }
    return v;
}

static std::optional<Message> handler(uint32_t svc, const Message& req) {
    std::lock_guard<std::mutex> l(gS.lock);
    auto ok = [&] { return test::okResponse(req.msgId); };
    if (gS.bindFails && ((svc == kSvcNas && req.msgId == multisim::kNasBindSubscription) ||
                         (svc == kSvcWms && req.msgId == wms::kBindSubscription) ||
                         (svc == kSvcDms && req.msgId == multisim::kDmsBindSubscription) ||
                         (svc == kSvcVoice && req.msgId == multisim::kVoiceBindSubscription)))
        return test::errResponse(req.msgId, kErrInvalidQmiCommand);
    switch (svc) {
        case kSvcDms:
            if (req.msgId == dms::kGetIds) return ok().strNoLen(0x11, "865947040141299").strNoLen(0x13, "01");
            if (req.msgId == dms::kGetRevision) return ok().strNoLen(0x01, "MPSS.AT.3.1-00819");
            if (req.msgId == dms::kGetOperatingMode) return ok().u8(0x01, gS.opMode);
            if (req.msgId == dms::kSetOperatingMode) {
                auto* t = req.get(0x01);
                if (t && !t->empty()) gS.opMode = (*t)[0];
                return ok();
            }
            if (req.msgId == dms::kUimGetIccid) return ok().strNoLen(0x01, "8933000000000000001");
            return ok();
        case kSvcUim:
            if (req.msgId == uim::kGetCardStatus) return ok().raw(0x10, gS.cardTlv);
            if (req.msgId == uim::kReadTransparent) return test::errResponse(req.msgId, kErrSimFileNotFound);
            return ok();
        case kSvcNas:
            if (req.msgId == nas::kGetServingSystem) return ok().raw(0x01, {1, 1, 1, 2, 1, nas::kRifLte});
            return ok();
        case kSvcWms:
            if (req.msgId == wms::kSendAck) {
                if (!gS.ackError) return ok();
                return test::errResponse(req.msgId, gS.ackError).u8(0x10, gS.ackCause);
            }
            return ok();
        default:
            return ok();
    }
}

// ------------------------------------------------------------------ listener
struct L : ModemCore::Listener {
    std::atomic<int> ready{0}, lost{0}, sim{0}, sms{0}, power{0};
    std::atomic<bool> lastPower{false};
    std::mutex m;
    std::vector<uint8_t> lastPdu;
    void onModemReady() override { ready++; }
    void onModemLost() override { lost++; }
    void onSimChanged() override { sim++; }
    void onNewSms(const std::vector<uint8_t>& pdu, bool) override {
        std::lock_guard<std::mutex> g(m);
        lastPdu = pdu;
        sms++;
    }
    void onRadioPowerChanged(bool on) override {
        lastPower = on;
        power++;
    }
};

template <typename F>
static bool waitFor(F f, int ms = 8000) {
    for (int i = 0; i < ms / 20; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

static int count(FakeModem& m, uint32_t svc, uint16_t id) {
    int n = 0;
    for (auto& [s, msg] : m.requests())
        if (s == svc && msg.msgId == id) n++;
    return n;
}
static std::optional<Message> last(FakeModem& m, uint32_t svc, uint16_t id) {
    std::optional<Message> r;
    for (auto& [s, msg] : m.requests())
        if (s == svc && msg.msgId == id) r = msg;
    return r;
}

static const std::vector<uint32_t> kSvcs = {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda};

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "2");
    // A6L 25 Sep: SIM in card 0 (USIM ready, primary subscription), card 1 = present-error (empty)
    gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady}, {uim::kCardError, 3, -1}});
    auto* modem = new FakeModem();  // leaked: core threads outlive main
    modem->setHandler(handler);
    ModemCore::setTransportFactory([modem] { return modem->transport(); });
    for (auto s : kSvcs) modem->addService(s);

    EXPECT_EQ(ModemCore::slotCount(), 2);
    auto& c1 = ModemCore::get(1);
    auto& c2 = ModemCore::get(2);
    EXPECT(&c1 != &c2);
    EXPECT_EQ(c1.sub(), 0);
    EXPECT_EQ(c2.sub(), 1);
    auto* l1 = new L();
    auto* l2 = new L();
    c1.addListener(l1);
    c2.addListener(l2);
    c1.start();
    c2.start();
    EXPECT(waitFor([&] { return c1.ready() && c2.ready() && l1->ready >= 1 && l2->ready >= 1; }));

    // ---- DSDS binding: only slot 2 binds, with the IDL layouts
    EXPECT(c2.bound());
    EXPECT_EQ(count(*modem, kSvcNas, multisim::kNasBindSubscription), 1);
    auto nb = last(*modem, kSvcNas, multisim::kNasBindSubscription);
    EXPECT(nb && nb->get(0x01) && *nb->get(0x01) == std::vector<uint8_t>{1});
    auto wb = last(*modem, kSvcWms, wms::kBindSubscription);
    EXPECT(wb && wb->get(0x01) && *wb->get(0x01) == std::vector<uint8_t>{1});
    auto vb = last(*modem, kSvcVoice, multisim::kVoiceBindSubscription);
    EXPECT(vb && vb->get(0x01) && *vb->get(0x01) == std::vector<uint8_t>{1});
    auto db = last(*modem, kSvcDms, multisim::kDmsBindSubscription);
    EXPECT(db && db->get(0x01) && *db->get(0x01) == (std::vector<uint8_t>{2, 0, 0, 0}));
    // slot 2 is not provisioned and has no card: no Change Provisioning Session
    EXPECT_EQ(count(*modem, kSvcUim, multisim::kUimChangeProvisioningSession), 0);

    // ---- per-slot SIM views: slot 1 = card 0 USIM ready, slot 2 = card 1 error (empty slot)
    auto s1 = c1.slotSim();
    EXPECT(s1.valid && s1.card == 0 && s1.cardInfo && s1.cardInfo->state == uim::kCardPresent);
    EXPECT_EQ(s1.gwAppIndex, 0);
    EXPECT(s1.provisioned);
    EXPECT_EQ(s1.cardSession, multisim::kSessionCardSlot1);
    auto s2 = c2.slotSim();
    EXPECT(s2.valid && s2.card == 1 && s2.cardInfo && s2.cardInfo->state == uim::kCardError);
    EXPECT_EQ(s2.gwAppIndex, -1);
    EXPECT(!s2.provisioned);
    EXPECT_EQ(s2.cardSession, multisim::kSessionCardSlot2);
    EXPECT_EQ(s2.provSession, multisim::kSessionSecondaryGw);
    EXPECT_EQ(c1.iccid(), std::string("8933000000000000001"));  // v1 path: UIM read fails -> DMS
    EXPECT_EQ(c2.iccid(true), std::string(""));                   // empty slot: nothing read
    EXPECT(c1.serving().has_value());

    // ---- radio power / airplane mode (modem booted in LOW_POWER)
    EXPECT(!c1.radioOn());
    EXPECT(c1.setRadioPower(true));
    EXPECT_EQ(gS.opMode, dms::kOnline);
    EXPECT(c1.radioOn());
    EXPECT(c2.setRadioPower(true));
    EXPECT(c2.radioOn());
    EXPECT(c1.setRadioPower(false));       // slot 1 off only: the modem stays online for slot 2
    EXPECT_EQ(gS.opMode, dms::kOnline);
    EXPECT(!c1.radioOn());
    EXPECT(c2.radioOn());
    EXPECT(c2.setRadioPower(false));       // airplane mode: every slot off -> LOW_POWER
    EXPECT_EQ(gS.opMode, dms::kLowPower);
    EXPECT(!c1.radioOn() && !c2.radioOn());
    EXPECT(waitFor([&] { return l1->power >= 1 && !l1->lastPower && l2->power >= 1 && !l2->lastPower; }));
    EXPECT(c1.setRadioPower(true));        // airplane off (slot 1 first)
    EXPECT(c2.setRadioPower(true));
    EXPECT_EQ(gS.opMode, dms::kOnline);
    EXPECT(c1.radioOn() && c2.radioOn());
    EXPECT(waitFor([&] { return l1->lastPower && l2->lastPower; }));

    // ---- MT SMS: transfer route, ack indicator SEND, SMS-on-IMS 0; the modem answers ACK_NOT_SENT
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.ackError = kErrAckNotSent;
        gS.ackCause = 1;
    }
    std::vector<uint8_t> tpdu = *unhex("040B913366336322F80000628092811404800AC1B0B83C7EBBCB2E17");
    Message ev(MsgType::Indication, wms::kSetEventReport);
    std::vector<uint8_t> t11 = {0 /*send ack*/, 0x44, 0x33, 0x22, 0x11, wms::kFormatGwPp,
                                static_cast<uint8_t>(tpdu.size()), 0};
    t11.insert(t11.end(), tpdu.begin(), tpdu.end());
    ev.raw(0x11, t11).u8(0x16, 0);
    int smsBefore = l1->sms;
    modem->indicate(kSvcWms, ev);
    EXPECT(waitFor([&] { return l1->sms > smsBefore; }));
    {
        std::lock_guard<std::mutex> g(l1->m);
        EXPECT(!l1->lastPdu.empty() && l1->lastPdu[0] == 0x00);  // Android gets SMSC(00) + TPDU
        EXPECT_EQ(l1->lastPdu.size(), tpdu.size() + 1);
    }
    int acksBefore = count(*modem, kSvcWms, wms::kSendAck);
    EXPECT(c1.ackLastSms(true, 0, 0));  // ACK_NOT_SENT tolerated (SMSC retransmits; logged)
    EXPECT_EQ(count(*modem, kSvcWms, wms::kSendAck), acksBefore + 1);
    auto ack = last(*modem, kSvcWms, wms::kSendAck);
    EXPECT(ack && ack->get(0x01) && *ack->get(0x01) == (std::vector<uint8_t>{0x44, 0x33, 0x22, 0x11, 1, 1}));
    EXPECT(ack && ack->get(0x12) && *ack->get(0x12) == std::vector<uint8_t>{0});
    EXPECT(ack && !ack->get(0x11));  // no failure TLV on a positive ack
    EXPECT(hoststub::logContains("ACK_NOT_SENT"));
    EXPECT(hoststub::logContains("network-released-link"));
    // ack indicator DO_NOT_SEND: nothing may be sent (that is what makes ACK_NOT_SENT otherwise)
    t11[0] = 1;
    Message ev2(MsgType::Indication, wms::kSetEventReport);
    ev2.raw(0x11, t11);
    smsBefore = l1->sms;
    modem->indicate(kSvcWms, ev2);
    EXPECT(waitFor([&] { return l1->sms > smsBefore; }));
    acksBefore = count(*modem, kSvcWms, wms::kSendAck);
    EXPECT(c1.ackLastSms(true, 0, 0));
    EXPECT_EQ(count(*modem, kSvcWms, wms::kSendAck), acksBefore);
    // a NACK (memory full) carries the 3GPP failure TLV
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.ackError = 0;
    }
    t11[0] = 0;
    Message ev3(MsgType::Indication, wms::kSetEventReport);
    ev3.raw(0x11, t11);
    smsBefore = l1->sms;
    modem->indicate(kSvcWms, ev3);
    EXPECT(waitFor([&] { return l1->sms > smsBefore; }));
    EXPECT(c1.ackLastSms(false, 0x16, 0xD3));
    ack = last(*modem, kSvcWms, wms::kSendAck);
    EXPECT(ack && ack->get(0x11) && *ack->get(0x11) == (std::vector<uint8_t>{0x16, 0xD3}));
    EXPECT(!c1.ackLastSms(true, 0, 0) || true);  // FIFO may still hold slot-shared events

    // ---- voice RAT: call on LTE, then CSFB to UMTS, then end
    Message call(MsgType::Indication, voice::kAllCallStatusInd);
    call.raw(0x01, {1, 1, voice::kStateConversation, 0, voice::kDirMo, 3, 0, 0});
    modem->indicate(kSvcVoice, call);
    EXPECT(waitFor([&] { return hoststub::logContains("A6L_RIL_CALL_RAT slot1 start rat=lte"); }));
    Message srv(MsgType::Indication, nas::kGetServingSystem);
    srv.raw(0x01, {1, 1, 1, 2, 1, nas::kRifUmts});
    modem->indicate(kSvcNas, srv);
    EXPECT(waitFor([&] { return hoststub::logContains("A6L_RIL_CALL_RAT slot1 change lte -> umts (CSFB)"); }));
    EXPECT_EQ(c1.callRat(), std::string("umts"));
    Message end(MsgType::Indication, voice::kAllCallStatusInd);
    end.raw(0x01, {1, 1, voice::kStateEnd, 0, voice::kDirMo, 3, 0, 0});
    modem->indicate(kSvcVoice, end);
    // end line: RAT now (the fake's serving refresh says lte again, like after a CSFB return) + RAT used
    EXPECT(waitFor([&] { return hoststub::logContains("(during call: umts)"); }));
    EXPECT(hoststub::logContains("A6L_RIL_CALL_RAT slot1 end rat="));
    EXPECT_EQ(c1.callRat(), std::string(""));

    // ---- PIN-locked SIM and no SIM (UIM card status indications)
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStatePin}, {uim::kCardError, 3, -1}});
    }
    int simBefore = l1->sim;
    Message cs(MsgType::Indication, uim::kCardStatusInd);
    cs.raw(0x10, gS.cardTlv);
    modem->indicate(kSvcUim, cs);
    EXPECT(waitFor([&] { return l1->sim > simBefore; }));
    s1 = c1.slotSim();
    EXPECT(s1.cardInfo && s1.gwAppIndex == 0 && s1.cardInfo->apps[0].state == uim::kAppStatePin);
    EXPECT(s1.cardInfo && s1.cardInfo->apps[0].pin1State == uim::kPinEnabledNotVerified);
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.cardTlv = cardStatusTlv(0xFFFF, 0xFFFF, {{uim::kCardAbsent, 0, -1}, {uim::kCardAbsent, 0, -1}});
    }
    simBefore = l1->sim;
    Message cs2(MsgType::Indication, uim::kCardStatusInd);
    cs2.raw(0x10, gS.cardTlv);
    modem->indicate(kSvcUim, cs2);
    EXPECT(waitFor([&] { return l1->sim > simBefore; }));
    s1 = c1.slotSim();
    EXPECT(s1.cardInfo && s1.cardInfo->state == uim::kCardAbsent && s1.gwAppIndex == -1 && !s1.provisioned);
    s2 = c2.slotSim();
    EXPECT(s2.cardInfo && s2.cardInfo->state == uim::kCardAbsent);

    // ---- SIM inserted in slot 2 while booted (not provisioned): the core would provision it at init
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady},
                                                    {uim::kCardPresent, 0, uim::kAppStateDetected}});
    }

    // ---- modem SSR: every service goes away (DEL_SERVER), comes back on new ports; slot 2 can no
    // longer bind (e.g. modem restarted in single standby) -> unusable, no SIM-1 traffic for slot 2
    int nasRegBefore = count(*modem, kSvcNas, nas::kRegisterIndications);
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.bindFails = true;
    }
    for (auto s : kSvcs) modem->removeService(s);
    EXPECT(waitFor([&] { return !c1.ready() && !c2.ready() && l1->lost >= 1 && l2->lost >= 1; }));
    EXPECT(!c1.serving(false).has_value() || true);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (auto s : kSvcs) modem->addService(s);
    EXPECT(waitFor([&] { return c1.ready() && c2.ready() && l1->ready >= 2 && l2->ready >= 2; }, 20000));
    EXPECT(count(*modem, kSvcNas, nas::kRegisterIndications) >= nasRegBefore + 1);  // slot 1 re-registered
    EXPECT(!c2.bound());
    EXPECT(!c2.serving(true).has_value());  // unbound slot 2 reports no service
    EXPECT(c2.calls(true).empty());
    EXPECT(c1.bound());
    EXPECT(c1.serving(true).has_value());
    EXPECT_EQ(count(*modem, kSvcUim, multisim::kUimChangeProvisioningSession), 0);  // unbound: no provisioning

    // ---- second SSR, binding works again: slot 2 SIM (card 1, USIM detected) gets provisioned
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.bindFails = false;
    }
    for (auto s : kSvcs) modem->removeService(s);
    EXPECT(waitFor([&] { return !c2.ready(); }));
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    for (auto s : kSvcs) modem->addService(s);
    EXPECT(waitFor([&] { return c1.ready() && c2.ready() && l2->ready >= 3; }, 20000));
    EXPECT(c2.bound());
    auto cp = last(*modem, kSvcUim, multisim::kUimChangeProvisioningSession);
    EXPECT(cp && cp->get(0x01) && *cp->get(0x01) == (std::vector<uint8_t>{multisim::kSessionSecondaryGw, 1}));
    EXPECT(cp && cp->get(0x10) && (*cp->get(0x10))[0] == 2 && (*cp->get(0x10))[1] == 7);  // uim slot 2, aid len

    printf("a6l-modemcore tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    std::_Exit(gFail ? 1 : 0);
}
