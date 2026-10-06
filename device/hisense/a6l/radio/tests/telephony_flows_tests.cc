// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (telephony-flows, 29 Sep 2026, docs/telephony-flows-20260929.md): offline mock tests, no phone,
// no real modem, NEVER a real emergency call (every "dial" below goes to the in-process fake modem).
//  U  USSD: QMI VOICE codec (encode ASCII/UCS2, decode ASCII/8-bit/UCS2/UTF-16, indications 0x3E/0x3D/0x43, sync
//     originate response), requests on the wire (No Wait / Answer / Cancel), flows::UssdSession dialogue decisions
//     (Originate vs Answer, NOTIFY/REQUEST/NW_RELEASE/NOT_SUPPORTED), real ModemCore: indication register carries
//     TLV 0x16, the three USSD indications reach Listener::onUssd.
//  E  Emergency: real ModemCore with no SIM / PIN-locked SIM / limited service -> core ready and an emergency dial
//     goes out as call type EMERGENCY (0x10 = 9) with the category; test calls to 112/15/17/18/114/191/196 are refused
//     with nothing sent; flows::emergencyNumbers (no-SIM list, FR 15/17/18/196 only on MCC 208); "emergency calls
//     only" registration variants.
//  M  MMS: a second PDN on a dedicated MMS APN (MVNO "orange.acte") next to the default one: own mux id + netdev,
//     APN/credentials on the wire, tearing it down keeps the default call.
//  H  HAL source contract (A6L_HAL_DIR): sendUssd on its own executor, cancel on the call executor, no SIM gate in
//     emergencyDial, USSD text never logged, EM variant for voice only.
#include "../hal/ModemCore.h"
#include "../hal/TelephonyFlows.h"
#include "fake_modem.h"

#include <a6lqmi/datacall.h>
#include <a6lqmi/log.h>
#include <a6lqmi/services.h>
#include <a6lqmi/ussd.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unistd.h>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using android::hardware::radio::a6l::ModemCore;
namespace flows = android::hardware::radio::a6l::flows;
namespace us = a6l::qmi::voice::ussd;

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

template <typename F>
static bool waitFor(F f, int ms = 8000) {
    for (int i = 0; i < ms / 20; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}
static std::vector<uint8_t> bytes(std::initializer_list<int> l) {
    std::vector<uint8_t> v;
    for (int x : l) v.push_back(static_cast<uint8_t>(x));
    return v;
}
static Message ind(uint16_t id) { return Message(MsgType::Indication, id); }

// ======================================================================== U: codec
static void testUssdCodec() {
    auto a = us::encode("*144#");
    EXPECT(a && a->dcs == us::kDcsAscii && a->data == bytes({'*', '1', '4', '4', '#'}));
    auto req = us::buildUssRequest(us::kOriginateNoWait, *a);
    EXPECT_EQ(req.msgId, 0x0043);
    EXPECT(req.get(0x01) && *req.get(0x01) == bytes({1, 5, '*', '1', '4', '4', '#'}));
    EXPECT_EQ(req.tlvs.size(), 1u);
    auto u = us::encode("1 \xC3\xA9");  // "1 é" -> UCS2 big endian
    EXPECT(u && u->dcs == us::kDcsUcs2 && u->data == bytes({0, '1', 0, ' ', 0, 0xE9}));
    EXPECT(!us::encode(""));
    EXPECT(!us::encode("\xC3"));              // truncated UTF-8
    EXPECT(!us::encode("\xC0\xAF"));          // overlong
    EXPECT(!us::encode("\xED\xA0\x80"));      // surrogate
    EXPECT(!us::encode("\xF0\x9F\x98\x80"));  // outside the BMP: no UCS2
    EXPECT(us::encode(std::string(182, '1')));
    EXPECT(!us::encode(std::string(183, '1')));
    // 91 UCS2 chars = 182 bytes ok, 92 = 184 too long
    std::string e91, e92;
    for (int i = 0; i < 91; i++) e91 += "\xC3\xA9";
    e92 = e91 + "\xC3\xA9";
    EXPECT(us::encode(e91) && us::encode(e91)->data.size() == 182);
    EXPECT(!us::encode(e92));

    auto t = us::decodeUssData(bytes({1, 3, 'O', 'K', '!'}));
    EXPECT(t.present && !t.decodeError && t.utf8 == "OK!");
    t = us::decodeUssData(bytes({2, 2, 'a', 0xE9}));  // 8-bit Latin-1
    EXPECT(!t.decodeError && t.utf8 == "a\xC3\xA9");
    t = us::decodeUssData(bytes({3, 4, 0x00, 'S', 0x20, 0xAC}));  // "S€"
    EXPECT(!t.decodeError && t.utf8 == "S\xE2\x82\xAC");
    EXPECT(us::decodeUssData(bytes({3, 3, 0, 'a', 0})).decodeError);  // odd UCS2
    EXPECT(us::decodeUssData(bytes({1, 5, 'a'})).decodeError);        // length overrun
    EXPECT(us::decodeUssData(bytes({1, 1, 'a', 'b'})).decodeError);   // trailing bytes
    EXPECT(us::decodeUssData(bytes({9, 1, 'a'})).decodeError);        // unknown DCS
    t = us::decodeUtf16(bytes({3, 'S', 0, 0x3D, 0xD8, 0x00, 0xDE}));  // "S" + U+1F600 (le16 surrogate pair)
    EXPECT(!t.decodeError && t.utf8 == "S\xF0\x9F\x98\x80");
    EXPECT(us::decodeUtf16(bytes({1, 0x3D, 0xD8})).decodeError);  // lone high surrogate
    EXPECT(us::decodeUtf16(bytes({2, 'a', 0})).decodeError);      // count overrun

    // USSD indication: UTF-16 preferred over USS data, user action mandatory
    Message m = ind(us::kUssdInd);
    m.u8(0x01, us::kActionRequired).raw(0x10, bytes({1, 3, 'x', 'y', 'z'})).raw(0x11, bytes({2, 'M', 0, 'N', 0}));
    auto e = us::parseIndication(m);
    EXPECT(e && e->kind == us::Event::Notification && e->userAction == us::kActionRequired && e->text.utf8 == "MN");
    Message m2 = ind(us::kUssdInd);
    m2.raw(0x10, bytes({1, 1, 'x'}));
    EXPECT(!us::parseIndication(m2));  // no user action TLV
    Message m3 = ind(us::kUssdInd);
    m3.u8(0x01, us::kActionNotRequired).raw(0x10, bytes({1, 2, 'h', 'i'})).raw(0x11, bytes({1, 0x00, 0xDC}));
    e = us::parseIndication(m3);  // broken UTF-16 -> USS data
    EXPECT(e && e->text.utf8 == "hi" && !e->text.decodeError);
    e = us::parseIndication(ind(us::kReleaseInd));
    EXPECT(e && e->kind == us::Event::Released);
    Message nw = ind(us::kOriginateNoWait);
    nw.u16(0x10, kErrNetworkNotReady).u16(0x11, 21);
    e = us::parseIndication(nw);
    EXPECT(e && e->kind == us::Event::OriginateResult && e->error && *e->error == kErrNetworkNotReady &&
           e->failureCause && *e->failureCause == 21 && !e->text.present);
    Message nw2 = ind(us::kOriginateNoWait);
    nw2.raw(0x12, bytes({1, 6, 'S', 'o', 'l', 'd', 'e', '5'}));
    e = us::parseIndication(nw2);
    EXPECT(e && !e->error && e->text.utf8 == "Solde5");
    EXPECT(!us::parseIndication(ind(0x002E)));  // not USSD

    // sync originate response (fallback path)
    Result r;
    r.status = Result::Ok;
    r.msg = test::okResponse(us::kOriginate);
    r.msg.raw(0x16, bytes({2, 'O', 0, 'K', 0}));
    auto ev = us::fromOriginateResponse(r);
    EXPECT(ev.kind == us::Event::OriginateResult && !ev.error && ev.text.utf8 == "OK");
    Result rf;
    rf.status = Result::QmiFailure;
    rf.qmiError = kErrInvalidTransition;
    ev = us::fromOriginateResponse(rf);
    EXPECT(ev.error && *ev.error == kErrInvalidTransition);
    Result rn;
    rn.status = Result::QmiFailure;
    rn.qmiError = kErrInvalidQmiCommand;
    EXPECT(us::isUnsupported(rn));
    EXPECT(!us::isUnsupported(rf));
    Result rt;
    rt.status = Result::Timeout;
    EXPECT(!us::isUnsupported(rt));
    printf("  U codec ok\n");
}

// ======================================================================== U: dialogue decisions
static void testUssdSession() {
    flows::UssdSession s;
    using Send = flows::UssdSession::Send;
    EXPECT(s.nextSend() == Send::Originate);
    EXPECT(!s.modemLost());  // idle: nothing to report
    s.sent(Send::Originate);
    EXPECT(s.active());
    // network menu: REQUEST, then our answer, then the final NOTIFY, then release
    us::Event req;
    req.kind = us::Event::Notification;
    req.userAction = us::kActionRequired;
    req.text = {true, false, "1: Solde 2: Options"};
    auto rep = s.onEvent(req);
    EXPECT(rep && rep->mode == flows::kRequest && rep->msg == "1: Solde 2: Options");
    EXPECT(s.nextSend() == Send::Answer);
    s.sent(Send::Answer);
    EXPECT(s.nextSend() == Send::Originate && s.active());
    us::Event note;
    note.kind = us::Event::Notification;
    note.userAction = us::kActionNotRequired;
    note.text = {true, false, "Solde: 5 EUR"};
    rep = s.onEvent(note);
    EXPECT(rep && rep->mode == flows::kNotify && rep->msg == "Solde: 5 EUR");
    us::Event rel;
    rel.kind = us::Event::Released;
    rep = s.onEvent(rel);
    EXPECT(rep && rep->mode == flows::kNwRelease && rep->msg.empty() && !s.active());
    // notification without text: nothing reported; undecodable text: nothing
    us::Event empty;
    empty.kind = us::Event::Notification;
    empty.userAction = us::kActionNotRequired;
    EXPECT(!s.onEvent(empty));
    empty.text = {true, true, ""};
    EXPECT(!s.onEvent(empty));
    // origination result: error -> NOT_SUPPORTED, text -> NOTIFY, neither -> NW_RELEASE (dialogue closed each time)
    s.sent(Send::Originate);
    us::Event res;
    res.kind = us::Event::OriginateResult;
    res.error = kErrNetworkNotReady;
    rep = s.onEvent(res);
    EXPECT(rep && rep->mode == flows::kNotSupported && !s.active());
    res.error.reset();
    res.text = {true, false, "Credit 3"};
    s.sent(Send::Originate);
    rep = s.onEvent(res);
    EXPECT(rep && rep->mode == flows::kNotify && rep->msg == "Credit 3" && !s.active());
    res.text = {};
    rep = s.onEvent(res);
    EXPECT(rep && rep->mode == flows::kNwRelease);
    // REQUEST then modem lost -> NOT_SUPPORTED once, back to Originate
    s.onEvent(req);
    rep = s.modemLost();
    EXPECT(rep && rep->mode == flows::kNotSupported);
    EXPECT(s.nextSend() == Send::Originate && !s.modemLost());
    // REQUEST then cancel -> Originate next
    s.onEvent(req);
    s.cancelled();
    EXPECT(s.nextSend() == Send::Originate && !s.active());
    // a refused Originate closes; a refused Answer does not reopen the question
    s.sent(Send::Originate);
    s.sendFailed(Send::Originate);
    EXPECT(!s.active());
    s.onEvent(req);
    s.sent(Send::Answer);
    s.sendFailed(Send::Answer);
    EXPECT(s.nextSend() == Send::Originate);
    // network-initiated REQUEST while idle opens the dialogue
    flows::UssdSession s2;
    rep = s2.onEvent(req);
    EXPECT(rep && rep->mode == flows::kRequest && s2.nextSend() == Send::Answer);
    // mode values == AIDL UssdModeType
    EXPECT(flows::kNotify == 0 && flows::kRequest == 1 && flows::kNwRelease == 2 && flows::kNotSupported == 4 &&
           flows::kNwTimeout == 5);
    printf("  U session ok\n");
}

// ======================================================================== fake modem for the real ModemCore
struct State {
    std::mutex lock;
    std::vector<uint8_t> cardTlv;
    std::vector<uint8_t> serving;  // NAS serving system TLV 0x01
    uint16_t noWaitErr = 0;        // 0 = accepted
};
static State gS;

static void u16le(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(x & 0xff);
    v.push_back(x >> 8);
}
struct CardSpec {
    uint8_t state, error;
    int appState;
};
static std::vector<uint8_t> cardStatusTlv(uint16_t gwPrimary, std::vector<CardSpec> cards) {
    std::vector<uint8_t> v;
    u16le(v, gwPrimary);
    u16le(v, 0xFFFF);
    u16le(v, 0xFFFF);
    u16le(v, 0xFFFF);
    v.push_back(static_cast<uint8_t>(cards.size()));
    for (auto& c : cards) {
        v.insert(v.end(), {c.state, 0, 0, 0, c.error});
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
    switch (svc) {
        case kSvcDms:
            if (req.msgId == dms::kGetIds) return ok().strNoLen(0x11, "865947040141299");
            if (req.msgId == dms::kGetRevision) return ok().strNoLen(0x01, "MPSS.AT.3.1-00819");
            if (req.msgId == dms::kGetOperatingMode) return ok().u8(0x01, dms::kOnline);
            return ok();
        case kSvcUim:
            if (req.msgId == uim::kGetCardStatus) return ok().raw(0x10, gS.cardTlv);
            if (req.msgId == uim::kReadTransparent) return test::errResponse(req.msgId, kErrSimFileNotFound);
            return ok();
        case kSvcNas:
            if (req.msgId == nas::kGetServingSystem) return ok().raw(0x01, gS.serving);
            return ok();
        case kSvcVoice:
            if (req.msgId == voice::kDialCall) return ok().u8(0x10, 7);  // fake call id, nothing real is dialled
            if (req.msgId == us::kOriginateNoWait && gS.noWaitErr) return test::errResponse(req.msgId, gS.noWaitErr);
            if (req.msgId == us::kOriginate) return ok().raw(0x12, bytes({1, 2, 'O', 'K'}));
            return ok();
        default:
            return ok();
    }
}

struct L : ModemCore::Listener {
    std::atomic<int> ready{0};
    std::mutex m;
    std::vector<us::Event> ussd;
    void onModemReady() override { ready++; }
    void onUssd(const us::Event& e) override {
        std::lock_guard<std::mutex> g(m);
        ussd.push_back(e);
    }
    size_t n() {
        std::lock_guard<std::mutex> g(m);
        return ussd.size();
    }
    us::Event at(size_t i) {
        std::lock_guard<std::mutex> g(m);
        return ussd.at(i);
    }
};

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

// ======================================================================== U + E over the real ModemCore
static void testCore(FakeModem& modem, ModemCore& core, L& l) {
    // ---- U: indication register carries USSD events next to the call/supplementary ones
    auto reg = last(modem, kSvcVoice, voice::kIndicationRegister);
    EXPECT(reg && reg->get(0x16) && *reg->get(0x16) == bytes({1}));
    EXPECT(reg && reg->get(0x13) && reg->get(0x12));
    // ---- U: the three indications reach the listener (worker thread), malformed ones do not
    Message i1 = ind(us::kUssdInd);
    i1.u8(0x01, us::kActionRequired).raw(0x10, bytes({1, 4, 'M', 'e', 'n', 'u'}));
    modem.indicate(kSvcVoice, i1);
    EXPECT(waitFor([&] { return l.n() == 1; }));
    if (l.n() >= 1) EXPECT(l.at(0).kind == us::Event::Notification && l.at(0).text.utf8 == "Menu");
    Message bad = ind(us::kUssdInd);
    bad.raw(0x10, bytes({1, 1, 'x'}));
    modem.indicate(kSvcVoice, bad);
    modem.indicate(kSvcVoice, ind(us::kReleaseInd));
    EXPECT(waitFor([&] { return l.n() == 2; }));
    if (l.n() >= 2) EXPECT(l.at(1).kind == us::Event::Released);
    Message i3 = ind(us::kOriginateNoWait);
    i3.u16(0x10, kErrCallFailed);
    modem.indicate(kSvcVoice, i3);
    EXPECT(waitFor([&] { return l.n() == 3; }));
    if (l.n() >= 3) EXPECT(l.at(2).kind == us::Event::OriginateResult && l.at(2).error == kErrCallFailed);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT_EQ(l.n(), 3u);  // the malformed one was dropped

    // ---- U: requests on the wire through the core's client
    auto enc = us::encode("*#100#");
    EXPECT(us::originateNoWait(core.ctl(), *enc).ok());
    auto o = last(modem, kSvcVoice, us::kOriginateNoWait);
    EXPECT(o && o->get(0x01) && *o->get(0x01) == bytes({1, 6, '*', '#', '1', '0', '0', '#'}));
    EXPECT(us::answer(core.ctl(), *us::encode("1")).ok());
    auto an = last(modem, kSvcVoice, us::kAnswer);
    EXPECT(an && an->get(0x01) && *an->get(0x01) == bytes({1, 1, '1'}));
    EXPECT(us::cancel(core.ctl()).ok());
    auto cn = last(modem, kSvcVoice, us::kCancel);
    EXPECT(cn && cn->tlvs.empty());
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.noWaitErr = kErrInvalidQmiCommand;  // older VOICE: HAL falls back to the sync originate
    }
    auto nr = us::originateNoWait(core.ctl(), *enc);
    EXPECT(!nr.ok() && us::isUnsupported(nr));
    auto sr = us::originate(core.ctl(), *enc, 5000);
    auto sev = us::fromOriginateResponse(sr);
    EXPECT(sr.ok() && sev.text.utf8 == "OK");
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.noWaitErr = 0;
    }

    // ---- E: no SIM, PIN-locked SIM, limited service: the core stays usable and the emergency dial goes out as an
    // emergency call type (fake modem only)
    struct Case {
        const char* name;
        std::vector<uint8_t> card;
        std::vector<uint8_t> serving;
        bool simAbsent;
        int32_t em;  // expected voice reg state after flows::emergencyOnlyVariant
    };
    const std::vector<Case> cases = {
            {"no-sim", cardStatusTlv(0xFFFF, {{uim::kCardAbsent, 0, -1}}), bytes({0, 0, 0, 2, 1, nas::kRifLte}), true,
             flows::kNotRegNotSearchingEm},
            {"pin-locked", cardStatusTlv(0x0000, {{uim::kCardPresent, 0, uim::kAppStatePin}}),
             bytes({3, 0, 0, 2, 1, nas::kRifUmts}), false, flows::kRegDeniedEm},
            {"no-service", cardStatusTlv(0x0000, {{uim::kCardPresent, 0, uim::kAppStateReady}}),
             bytes({2, 0, 0, 2, 0}), false, flows::kNotRegSearching},
    };
    for (auto& c : cases) {
        {
            std::lock_guard<std::mutex> g(gS.lock);
            gS.cardTlv = c.card;
            gS.serving = c.serving;
        }
        auto sim = core.slotSim(true);
        bool absent = !sim.valid || !sim.cardInfo || sim.cardInfo->state != uim::kCardPresent;
        EXPECT_EQ(absent, c.simAbsent);
        auto s = core.serving(true);
        EXPECT(s.has_value());
        EXPECT(core.ready());
        if (s) {
            int32_t base = s->regState == nas::kSearching ? flows::kNotRegSearching
                           : s->regState == nas::kDenied  ? flows::kRegDenied
                                                          : flows::kNotRegNotSearching;
            EXPECT_EQ(flows::emergencyOnlyVariant(base, !s->radioIfs.empty()), c.em);
        }
        int before = count(modem, kSvcVoice, voice::kDialCall);
        voice::EmergencyDialRequest er;
        er.number = "112";
        er.categories = flows::kCatAmbulance;
        er.routing = voice::EmergencyRouting::Emergency;
        uint8_t id = 0;
        auto out = voice::emergencyDial(core.ctl(), er, &id);
        EXPECT(out.result.ok());
        EXPECT_EQ(id, 7);
        EXPECT_EQ(count(modem, kSvcVoice, voice::kDialCall), before + 1);
        auto d = last(modem, kSvcVoice, voice::kDialCall);
        EXPECT(d && d->get(0x10) && *d->get(0x10) == bytes({voice::kTypeEmergency}));
        EXPECT(d && d->get(0x14) && *d->get(0x14) == bytes({flows::kCatAmbulance}));
        EXPECT(d && d->get(0x01) && std::string(d->get(0x01)->begin(), d->get(0x01)->end()) == "112");
        printf("  E %s: emergency dial type 9 sent to the fake modem\n", c.name);
    }
    // ---- E: a test emergency call to a real emergency number is refused with nothing sent (FR numbers included)
    int before = count(modem, kSvcVoice, voice::kDialCall);
    for (const char* n : {"112", "15", "17", "18", "114", "115", "119", "191", "196", "911"}) {
        voice::EmergencyDialRequest er;
        er.number = n;
        er.isTesting = true;
        uint8_t id = 0;
        auto out = voice::emergencyDial(core.ctl(), er, &id);
        EXPECT(out.refusedTestToEmergencyNumber);
        EXPECT(out.attempts.empty());
    }
    EXPECT_EQ(count(modem, kSvcVoice, voice::kDialCall), before);
}

// ======================================================================== E: number list policy
static void testEmergencyNumbers() {
    auto has = [](const std::vector<flows::EccEntry>& l, const char* n, const char* mcc, int32_t cat) {
        for (auto& e : l)
            if (e.number == n && e.mcc == mcc && e.categories == cat) return true;
        return false;
    };
    auto withSim = flows::emergencyNumbers(false, "");
    EXPECT_EQ(withSim.size(), 2u);
    EXPECT(has(withSim, "112", "", 0) && has(withSim, "911", "", 0));
    auto noSim = flows::emergencyNumbers(true, "");
    EXPECT_EQ(noSim.size(), 8u);
    for (const char* n : {"112", "911", "000", "08", "110", "118", "119", "999"}) EXPECT(has(noSim, n, "", 0));
    auto fr = flows::emergencyNumbers(false, "208");
    EXPECT_EQ(fr.size(), 6u);
    EXPECT(has(fr, "15", "208", flows::kCatAmbulance));
    EXPECT(has(fr, "17", "208", flows::kCatPolice));
    EXPECT(has(fr, "18", "208", flows::kCatFire));
    EXPECT(has(fr, "196", "208", flows::kCatMarine));
    auto de = flows::emergencyNumbers(false, "262");  // abroad: no 15/17/18
    EXPECT_EQ(de.size(), 2u);
    auto frNoSim = flows::emergencyNumbers(true, "208");
    EXPECT_EQ(frNoSim.size(), 12u);
    // every number the HAL reports is also refused as a test-call target
    for (auto& e : frNoSim) EXPECT(voice::isWellKnownEmergencyNumber(e.number));
    // category bits == AIDL EmergencyServiceCategory
    EXPECT(flows::kCatPolice == 1 && flows::kCatAmbulance == 2 && flows::kCatFire == 4 && flows::kCatMarine == 8);
    // registration variants: only unregistered + camped
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kNotRegNotSearching, true), flows::kNotRegNotSearchingEm);
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kNotRegSearching, true), flows::kNotRegSearchingEm);
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kRegDenied, true), flows::kRegDeniedEm);
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kRegUnknown, true), flows::kUnknownEm);
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kRegHome, true), flows::kRegHome);
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kRegRoaming, true), flows::kRegRoaming);
    EXPECT_EQ(flows::emergencyOnlyVariant(flows::kNotRegSearching, false), flows::kNotRegSearching);
    printf("  E numbers/reg-state ok\n");
}

// ======================================================================== M: MMS on a secondary PDN
struct FakeLink : radio::LinkOps {
    std::mutex m;
    std::vector<std::string> ops;
    int run(const std::string& op) {
        std::lock_guard<std::mutex> g(m);
        ops.push_back(op);
        return 0;
    }
    bool exists(const std::string& n) override { return n == "rmnet_ipa0"; }
    int createLink(const std::string&, const std::string& n, uint16_t mux, uint32_t) override {
        return run("create " + n + " mux " + std::to_string(mux));
    }
    int deleteLink(const std::string& n) override { return run("delete " + n); }
    int setUp(const std::string& n, bool up) override { return run(std::string(up ? "up " : "down ") + n); }
    int setMtu(const std::string& n, int mtu) override { return run("mtu " + n + " " + std::to_string(mtu)); }
    int addAddress(const std::string& n, const std::string& c) override { return run("addr " + c + " " + n); }
    std::string joined() {
        std::lock_guard<std::mutex> g(m);
        std::string o;
        for (auto& x : ops) o += (o.empty() ? "" : ",") + x;
        return o;
    }
};
static std::atomic<uint32_t> gHandle{0x100};
static std::optional<Message> wdsHandler(uint32_t svc, const Message& req) {
    if (svc != kSvcWds) return test::okResponse(req.msgId);
    if (req.msgId == wds::kStartNetwork) return test::okResponse(req.msgId).u32(0x01, gHandle++);
    if (req.msgId == wds::kGetCurrentSettings) {
        auto r = test::okResponse(req.msgId);
        uint32_t base = 0x0A000000 + (gHandle.load() << 4);
        r.u32(0x15, 0x0A0B0C0D).u32(0x1E, base + 5).u32(0x20, base + 6).u32(0x21, 0xFFFFFFFC).u32(0x29, 1500);
        return r;
    }
    return test::okResponse(req.msgId);
}
static void testMmsSecondaryPdn() {
    FakeModem modem;
    modem.addService(kSvcWds);
    modem.setHandler(wdsHandler);
    auto link = std::make_shared<FakeLink>();
    radio::DataConfig cfg;
    cfg.setDataFormat = false;
    cfg.settingsRetryMs = 20;
    radio::DataCallManager dm(cfg, [&](const char* tag) { return std::make_unique<Client>(modem.transport(), tag); },
                              nullptr);
    dm.setLinkOps(link);
    radio::DataRequest def;  // Orange World (default,dun,supl,xcap,mms on one APN: MMS needs no second PDN)
    def.apn = "orange";
    def.user = def.password = "orange";
    def.auth = 1;
    def.protocol = radio::Protocol::V4;
    auto d = dm.setup(def);
    EXPECT(d.ok);
    radio::DataRequest mms;  // MVNO on Orange (C le mobile / NRJ): type=mms only, APN orange.acte
    mms.apn = "orange.acte";
    mms.user = mms.password = "orange";
    mms.auth = 1;
    mms.protocol = radio::Protocol::V4;
    auto m = dm.setup(mms);
    EXPECT(m.ok);
    EXPECT(d.call.cid != m.call.cid && d.call.muxId != m.call.muxId && d.call.ifname != m.call.ifname);
    EXPECT_EQ(dm.list().size(), 2u);
    int starts = 0;
    bool sawMmsApn = false;
    for (auto& [s, msg] : modem.requests()) {
        if (s != kSvcWds || msg.msgId != wds::kStartNetwork) continue;
        starts++;
        auto* apn = msg.get(0x14);
        if (apn && std::string(apn->begin(), apn->end()) == "orange.acte") {
            sawMmsApn = msg.get(0x16) && *msg.get(0x16) == bytes({1}) && msg.get(0x17) && msg.get(0x18);
        }
    }
    EXPECT_EQ(starts, 2);
    EXPECT(sawMmsApn);
    std::string ops = link->joined();
    EXPECT(ops.find("create " + d.call.ifname) != std::string::npos);
    EXPECT(ops.find("create " + m.call.ifname) != std::string::npos);
    // MMS done: its PDN goes, the default one stays
    EXPECT(dm.deactivate(m.call.cid));
    auto left = dm.list();
    EXPECT(left.size() == 1 && left[0].cid == d.call.cid && left[0].apn == "orange");
    EXPECT(dm.deactivate(d.call.cid));
    printf("  M secondary MMS PDN ok (%s + %s)\n", d.call.ifname.c_str(), m.call.ifname.c_str());
}

// ======================================================================== H: HAL source contract
static std::string readFile(const std::string& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static std::string body(const std::string& src, const std::string& sig) {
    auto b = src.find(sig);
    if (b == std::string::npos) return "";
    auto e = src.find("\n}\n", b);
    return src.substr(b, e == std::string::npos ? std::string::npos : e - b);
}
static void testHalContract() {
#ifdef A6L_HAL_DIR
    auto v = readFile(std::string(A6L_HAL_DIR) + "/RadioMessagingVoice.cpp");
    auto n = readFile(std::string(A6L_HAL_DIR) + "/RadioNetworkData.cpp");
    auto h = readFile(std::string(A6L_HAL_DIR) + "/RadioImpl.h");
    EXPECT(!v.empty() && !n.empty() && !h.empty());
    auto send = body(v, "ScopedAStatus A6lRadioVoice::sendUssd(");
    EXPECT(send.find("mUssdExec.post") != std::string::npos);
    EXPECT(send.find("mUssd.nextSend()") != std::string::npos);
    EXPECT(send.find("originateNoWait") != std::string::npos && send.find("isUnsupported") != std::string::npos);
    EXPECT(send.find("core.bound()") != std::string::npos);
    EXPECT(send.find("<< ussd <<") == std::string::npos && send.find("<< ussd;") == std::string::npos &&
           send.find("<< e.data") == std::string::npos);  // only the length is logged
    auto cancel = body(v, "ScopedAStatus A6lRadioVoice::cancelPendingUssd(");
    EXPECT(cancel.find("mExec.post") != std::string::npos && cancel.find("kErrNoEffect") != std::string::npos);
    auto rep = body(v, "void A6lRadioVoice::reportUssd(");
    EXPECT(rep.find("r.msg.size()") != std::string::npos && rep.find("<< r.msg ") == std::string::npos &&
           rep.find("<< r.msg;") == std::string::npos);
    auto emerg = body(v, "ScopedAStatus A6lRadioVoice::emergencyDial(");
    EXPECT(emerg.find("qv::emergencyDial") != std::string::npos);
    EXPECT(emerg.find("slotSim") == std::string::npos && emerg.find("cardInfo") == std::string::npos &&
           emerg.find("bound()") == std::string::npos);  // no SIM / binding gate on emergency calls
    auto ecbm = body(v, "ScopedAStatus A6lRadioVoice::exitEmergencyCallbackMode(");
    EXPECT(ecbm.find("noError(serial)") != std::string::npos);
    EXPECT(v.find("flows::emergencyNumbers(") != std::string::npos);
    EXPECT(v.find("void A6lRadioVoice::onSimChanged() { publishEmergencyNumbers(false); }") != std::string::npos);
    EXPECT(h.find("Executor mUssdExec{\"a6l-ussd\"}") != std::string::npos);
    auto reg = body(n, "aidlNet::RegStateResult A6lRadioNetwork::buildRegState(");
    EXPECT(reg.find("if (voice && s->regState != nas::kRegistered)") != std::string::npos);
    EXPECT(reg.find("flows::emergencyOnlyVariant") != std::string::npos);
    printf("  H HAL contract ok\n");
#endif
}

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    testUssdCodec();
    testUssdSession();
    testEmergencyNumbers();
    testMmsSecondaryPdn();
    testHalContract();

    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    gS.cardTlv = cardStatusTlv(0x0000, {{uim::kCardPresent, 0, uim::kAppStateReady}});
    gS.serving = bytes({1, 1, 1, 2, 1, nas::kRifLte});
    auto* modem = new FakeModem();  // leaked: core threads outlive main
    modem->setHandler(handler);
    ModemCore::setTransportFactory([modem] { return modem->transport(); });
    for (auto s : {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda}) modem->addService(s);
    auto& core = ModemCore::get(1);
    auto* l = new L();
    core.addListener(l);
    core.start();
    EXPECT(waitFor([&] { return core.ready() && l->ready >= 1; }));
    if (core.ready()) testCore(*modem, core, *l);

    printf("a6l-telephony-flows: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
