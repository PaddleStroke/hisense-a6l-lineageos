// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 deep review fixes (28 Sep 2026, docs/hardware-review-deep-20260928.md), host only, mock modem.
// NO real call is ever placed: every dial below goes to the in-process fake modem.
//  F23 IRadioVoice.emergencyDial: isTesting x routing x user intent, categories, fallback only after a definitive
//      refusal (no redial after timeout / lost service), test call to a real emergency number -> nothing sent
//  F24 IRadioVoice.dial CLIR: DEFAULT/INVOCATION/SUPPRESSION -> QMI VOICE Dial TLV 0x11 absent/2/1, invalid refused
//  F25 WMS cell broadcast (format 7) and ETWS (TLV 0x13) -> onNewBroadcastSms (real hal/ModemCore.cpp)
//  F26 stored-route SMS deleted only after Android's positive ack, kept on a negative ack
//  F27 one ordered SMS queue: stored A then transfer B -> A delivered alone, A's ack never touches B's transaction
// The ModemCore part reuses the modemcore_tests.cc fixture (fake modem handler, waitFor, count/last helpers).
#define main modemcore_original_main
#include "modemcore_tests.cc"
#undef main

#include <future>

// ================================================================== F23 / F24 (qmi voice, fake modem)
static std::optional<Message> firstVoice(FakeModem& fm, size_t from, size_t i) {
    size_t k = 0;
    auto rq = fm.requests();
    for (size_t j = from; j < rq.size(); j++)
        if (rq[j].first == kSvcVoice && rq[j].second.msgId == voice::kDialCall && k++ == i) return rq[j].second;
    return std::nullopt;
}
static int dialsSince(FakeModem& fm, size_t from) {
    int n = 0;
    auto rq = fm.requests();
    for (size_t j = from; j < rq.size(); j++)
        if (rq[j].first == kSvcVoice && rq[j].second.msgId == voice::kDialCall) n++;
    return n;
}
static bool isEmergencyReq(const Message& m) {
    auto* t = m.get(0x10);
    return t && t->size() == 1 && (*t)[0] == voice::kTypeEmergency;
}

static void testVoice() {
    FakeModem fm;
    fm.addService(kSvcVoice);
    // reply script: per dial request, the error to return (0 = ok, 0xFFFF = stay silent -> client timeout)
    std::mutex sl;
    std::deque<uint16_t> script;
    fm.setHandler([&](uint32_t, const Message& r) -> std::optional<Message> {
        uint16_t e = 0;
        {
            std::lock_guard<std::mutex> g(sl);
            if (r.msgId == voice::kDialCall && !script.empty()) {
                e = script.front();
                script.pop_front();
            }
        }
        if (e == 0xFFFF) return std::nullopt;
        if (e) return test::errResponse(r.msgId, e);
        auto ok = test::okResponse(r.msgId);
        if (r.msgId == voice::kDialCall) ok.u8(0x10, 7);
        return ok;
    });
    auto setScript = [&](std::initializer_list<uint16_t> s) {
        std::lock_guard<std::mutex> g(sl);
        script.assign(s);
    };
    Client c(fm.transport(), "t");
    EXPECT(c.start({kSvcVoice}));
    EXPECT(c.waitForServices({kSvcVoice}, 2000).empty());
    uint8_t id = 0;

    // ---- F24: CLIR mapping and wire encoding
    std::optional<uint8_t> clir;
    EXPECT(voice::clirFromAndroid(0, &clir) && !clir);
    EXPECT(voice::clirFromAndroid(1, &clir) && clir == voice::kClirInvocation);
    EXPECT(voice::clirFromAndroid(2, &clir) && clir == voice::kClirSuppression);
    EXPECT(!voice::clirFromAndroid(3, &clir) && !voice::clirFromAndroid(-1, &clir));
    for (int a = 0; a <= 2; a++) {
        voice::clirFromAndroid(a, &clir);
        size_t from = fm.requests().size();
        setScript({});
        EXPECT(voice::dial(c, voice::DialRequest{"+33600000000", false, clir, std::nullopt}, &id).ok());
        auto m = firstVoice(fm, from, 0);
        EXPECT(m && !m->get(0x10) && !m->get(0x14));  // ordinary call: no emergency type
        auto* t = m ? m->get(0x11) : nullptr;
        if (a == 0) EXPECT(t == nullptr);
        if (a == 1) EXPECT(t && *t == std::vector<uint8_t>{2});
        if (a == 2) EXPECT(t && *t == std::vector<uint8_t>{1});
    }
    // modem refusal of a restricted call is returned as is (no silent retry without CLIR)
    {
        size_t from = fm.requests().size();
        setScript({kErrInvalidArgument});
        auto r = voice::dial(c, voice::DialRequest{"+33600000000", false, uint8_t(voice::kClirInvocation), std::nullopt}, &id);
        EXPECT(!r.ok() && r.qmiError == kErrInvalidArgument && dialsSince(fm, from) == 1);
    }

    using R = voice::EmergencyRouting;
    auto run = [&](voice::EmergencyDialRequest q, std::initializer_list<uint16_t> s, size_t* fromOut) {
        *fromOut = fm.requests().size();
        setScript(s);
        return voice::emergencyDial(c, q, &id);
    };
    size_t from;
    // ---- F23: isTesting never produces an emergency request, whatever routing / intent / categories
    for (R rt : {R::Unknown, R::Emergency, R::Normal})
        for (bool intent : {false, true}) {
            auto o = run({"5550100", 1, 3, rt, intent, true}, {kErrCallFailed}, &from);
            EXPECT(dialsSince(fm, from) == 1);
            auto m = firstVoice(fm, from, 0);
            EXPECT(m && !isEmergencyReq(*m) && !m->get(0x14));
            EXPECT(m && m->get(0x11) && *m->get(0x11) == std::vector<uint8_t>{2});  // CLIR carried on the test call
            EXPECT(!o.result.ok() && o.attempts.size() == 1);  // refused test call: no emergency fallback
        }
    for (const char* n : {"112", "911", "+112", "999", "000", "08"}) {
        auto o = run({n, 0, 0, R::Emergency, true, true}, {}, &from);
        EXPECT(o.refusedTestToEmergencyNumber && !o.result.ok() && dialsSince(fm, from) == 0);
    }
    EXPECT(!voice::isWellKnownEmergencyNumber("5550100") && !voice::isWellKnownEmergencyNumber("*112#"));
    // ---- EMERGENCY / UNKNOWN routing: one emergency request with the category
    for (R rt : {R::Emergency, R::Unknown}) {
        auto o = run({"112", 1, 0x03 | 0x40, rt, false, false}, {}, &from);
        EXPECT(o.result.ok() && dialsSince(fm, from) == 1);
        auto m = firstVoice(fm, from, 0);
        EXPECT(m && isEmergencyReq(*m) && m->get(0x14) && *m->get(0x14) == std::vector<uint8_t>{0x03});
        EXPECT(m && !m->get(0x11));  // no CLIR on the emergency request
    }
    {  // no category -> no TLV 0x14
        auto o = run({"112", 0, 0, R::Emergency, false, false}, {}, &from);
        auto m = firstVoice(fm, from, 0);
        EXPECT(o.result.ok() && m && isEmergencyReq(*m) && !m->get(0x14));
    }
    // explicit rejection (not an encoding problem): no second, non-emergency request
    {
        auto o = run({"112", 0, 1, R::Emergency, false, false}, {kErrCallFailed}, &from);
        EXPECT(!o.result.ok() && dialsSince(fm, from) == 1);
    }
    // encoding rejection: category dropped, then legacy plain dial
    {
        auto o = run({"112", 0, 1, R::Emergency, false, false}, {kErrInvalidArgument, kErrInvalidArgument}, &from);
        EXPECT(o.result.ok() && dialsSince(fm, from) == 3);
        auto m0 = firstVoice(fm, from, 0), m1 = firstVoice(fm, from, 1), m2 = firstVoice(fm, from, 2);
        EXPECT(m0 && isEmergencyReq(*m0) && m0->get(0x14));
        EXPECT(m1 && isEmergencyReq(*m1) && !m1->get(0x14));
        EXPECT(m2 && !m2->get(0x10));
    }
    // ---- NORMAL routing without known intent: normal first, emergency after a definitive refusal
    {
        auto o = run({"112", 1, 1, R::Normal, false, false}, {}, &from);
        auto m = firstVoice(fm, from, 0);
        EXPECT(o.result.ok() && dialsSince(fm, from) == 1 && m && !isEmergencyReq(*m));
        EXPECT(m && m->get(0x11) && *m->get(0x11) == std::vector<uint8_t>{2});
        o = run({"112", 0, 1, R::Normal, false, false}, {kErrNoNetworkFound}, &from);
        auto m1 = firstVoice(fm, from, 1);
        EXPECT(o.result.ok() && dialsSince(fm, from) == 2 && m1 && isEmergencyReq(*m1));
        // encoding rejection of the emergency request after a normal attempt: no third (normal) request
        o = run({"112", 0, 0, R::Normal, false, false}, {kErrNoNetworkFound, kErrInvalidArgument}, &from);
        EXPECT(!o.result.ok() && dialsSince(fm, from) == 2);
    }
    // NORMAL routing with known user intent -> emergency directly
    {
        auto o = run({"112", 0, 0, R::Normal, true, false}, {}, &from);
        auto m = firstVoice(fm, from, 0);
        EXPECT(o.result.ok() && dialsSince(fm, from) == 1 && m && isEmergencyReq(*m));
    }
    // ambiguous outcome (timeout: the call may exist) -> never a second dial
    {
        auto o = run({"112", 0, 0, R::Normal, false, false}, {0xFFFF}, &from);
        EXPECT(o.result.status == Result::Timeout && dialsSince(fm, from) == 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // lost service during an emergency dial -> no fallback either
    {
        fm.removeService(kSvcVoice);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto o = run({"112", 0, 0, R::Emergency, false, false}, {}, &from);
        EXPECT(!o.result.ok() && o.result.status != Result::QmiFailure && o.attempts.size() == 1);
    }
    c.stop();
}

// ================================================================== F25-F27 (real ModemCore)
static FakeModem* gModem;
struct SmsL : ModemCore::Listener {
    std::mutex m;
    std::vector<std::vector<uint8_t>> sms, cb;
    std::vector<int> deletesAtDelivery;
    void onNewSms(const std::vector<uint8_t>& pdu, bool) override {
        std::lock_guard<std::mutex> g(m);
        sms.push_back(pdu);
        deletesAtDelivery.push_back(count(*gModem, kSvcWms, wms::kDelete));
    }
    void onNewBroadcastSms(const std::vector<uint8_t>& d) override {
        std::lock_guard<std::mutex> g(m);
        cb.push_back(d);
    }
    size_t nSms() { std::lock_guard<std::mutex> g(m); return sms.size(); }
    size_t nCb() { std::lock_guard<std::mutex> g(m); return cb.size(); }
};
static Message transferInd(uint8_t ackInd, uint32_t txn, uint8_t fmt, std::vector<uint8_t> data) {
    Message m(MsgType::Indication, wms::kSetEventReport);
    std::vector<uint8_t> v = {ackInd, uint8_t(txn), uint8_t(txn >> 8), uint8_t(txn >> 16), uint8_t(txn >> 24), fmt,
                              uint8_t(data.size()), uint8_t(data.size() >> 8)};
    v.insert(v.end(), data.begin(), data.end());
    m.raw(0x11, v);
    return m;
}
static Message storedInd(uint32_t idx) {
    Message m(MsgType::Indication, wms::kSetEventReport);
    m.raw(0x10, {wms::kStorageNv, uint8_t(idx), 0, 0, 0});
    return m;
}
static uint32_t ackTxn(const Message& a) {
    auto& b = *a.get(0x01);
    return b[0] | b[1] << 8 | b[2] << 16 | uint32_t(b[3]) << 24;
}
static void drain(ModemCore& core) {
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    core.post([p] { p->set_value(); });
    f.wait_for(std::chrono::seconds(3));
}

static void testSms() {
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady}});
    gModem = new FakeModem();  // leaked: core threads outlive the test
    gModem->setHandler([](uint32_t svc, const Message& req) -> std::optional<Message> {
        if (svc == kSvcWms && req.msgId == wms::kRawRead) {
            auto* id = req.get(0x01);
            uint8_t idx = id && id->size() >= 2 ? (*id)[1] : 0;
            uint8_t fmt = idx >= 100 ? wms::kFormatGwBc : wms::kFormatGwPp;
            return test::okResponse(req.msgId).raw(0x01, {0, fmt, 3, 0, 0x04, 0x00, idx});
        }
        return handler(svc, req);
    });
    ModemCore::setTransportFactory([] { return gModem->transport(); });
    for (auto s : kSvcs) gModem->addService(s);
    auto& core = ModemCore::get(1);
    auto* l = new SmsL();
    core.addListener(l);
    core.start();
    EXPECT(waitFor([&] { return core.ready(); }));
    auto& fm = *gModem;

    // ---- F25: GSM CB page (88 bytes, format 7, ack indicator DO_NOT_SEND) -> newBroadcastSms, no ack, no NACK
    std::vector<uint8_t> page(88, 0x11);
    page[2] = 0x11;  // message id 0x1112 (presidential alert range)
    page[3] = 0x12;
    int acks0 = count(fm, kSvcWms, wms::kSendAck);
    fm.indicate(kSvcWms, transferInd(1, 0x10, wms::kFormatGwBc, page));
    EXPECT(waitFor([&] { return l->nCb() == 1; }));
    drain(core);
    {
        std::lock_guard<std::mutex> g(l->m);
        EXPECT(l->cb.size() == 1 && l->cb[0] == page);
        EXPECT(l->sms.empty());  // not the point-to-point path
    }
    EXPECT(count(fm, kSvcWms, wms::kSendAck) == acks0);
    EXPECT(!hoststub::logContains("ignoring MT SMS in format 7"));
    EXPECT(core.pendingSmsCount() == 0);
    // modem asks for an ack on a broadcast page: positive, never a NACK, never queued for Android
    fm.indicate(kSvcWms, transferInd(0, 0x11, wms::kFormatGwBc, page));
    EXPECT(waitFor([&] { return l->nCb() == 2; }));
    EXPECT(waitFor([&] { return count(fm, kSvcWms, wms::kSendAck) == acks0 + 1; }));
    auto a = last(fm, kSvcWms, wms::kSendAck);
    EXPECT(a && ackTxn(*a) == 0x11 && (*a->get(0x01))[5] == 1 && !a->get(0x11));
    // ETWS primary notification (TLV 0x13) -> newBroadcastSms
    std::vector<uint8_t> etws(56, 0x22);
    Message em(MsgType::Indication, wms::kSetEventReport);
    std::vector<uint8_t> t13 = {0, uint8_t(etws.size()), 0};
    t13.insert(t13.end(), etws.begin(), etws.end());
    em.raw(0x13, t13);
    fm.indicate(kSvcWms, em);
    EXPECT(waitFor([&] { return l->nCb() == 3; }));
    {
        std::lock_guard<std::mutex> g(l->m);
        EXPECT(l->cb.size() == 3 && l->cb[2] == etws);
    }
    // stored broadcast page -> newBroadcastSms, then deleted
    int del0 = count(fm, kSvcWms, wms::kDelete);
    fm.indicate(kSvcWms, storedInd(100));
    EXPECT(waitFor([&] { return l->nCb() == 4; }));
    EXPECT(waitFor([&] { return count(fm, kSvcWms, wms::kDelete) == del0 + 1; }));

    // ---- F26 + F27: worker held; stored A then transfer B (txn 0x2222) arrive
    auto gate = std::make_shared<std::promise<void>>();
    auto gf = gate->get_future().share();
    std::atomic<bool> blocked{false};
    core.post([&blocked, gf] {
        blocked = true;
        gf.wait();
    });
    EXPECT(waitFor([&] { return blocked.load(); }));
    size_t sms0 = l->nSms();
    del0 = count(fm, kSvcWms, wms::kDelete);
    acks0 = count(fm, kSvcWms, wms::kSendAck);
    fm.indicate(kSvcWms, storedInd(7));
    fm.indicate(kSvcWms, transferInd(0, 0x2222, wms::kFormatGwPp, {0x04, 0x00, 0x42}));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    gate->set_value();
    EXPECT(waitFor([&] { return l->nSms() == sms0 + 1; }));
    drain(core);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    EXPECT(l->nSms() == sms0 + 1);  // B waits for A's acknowledgement (one outstanding newSms)
    {
        std::lock_guard<std::mutex> g(l->m);
        EXPECT(!l->sms[sms0].empty() && l->sms[sms0].back() == 7);  // A (stored index 7) first
        EXPECT(l->deletesAtDelivery[sms0] == del0);                    // not deleted before delivery
    }
    EXPECT(count(fm, kSvcWms, wms::kDelete) == del0);
    EXPECT(core.pendingSmsCount() == 2);
    // Android rejects A (memory full): A stays in modem storage, B's transaction untouched
    EXPECT(core.ackLastSms(false, 0x16, 0xD3));
    EXPECT(waitFor([&] { return l->nSms() == sms0 + 2; }));
    EXPECT(count(fm, kSvcWms, wms::kDelete) == del0);
    EXPECT(count(fm, kSvcWms, wms::kSendAck) == acks0);
    {
        std::lock_guard<std::mutex> g(l->m);
        EXPECT(l->sms[sms0 + 1].back() == 0x42);  // B
    }
    // framework reconnects before acking B: B delivered again, one ack consumes it
    core.smsClientReconnected();
    EXPECT(waitFor([&] { return l->nSms() == sms0 + 3; }));
    EXPECT(core.ackLastSms(true, 0, 0));
    a = last(fm, kSvcWms, wms::kSendAck);
    EXPECT(count(fm, kSvcWms, wms::kSendAck) == acks0 + 1 && a && ackTxn(*a) == 0x2222 && (*a->get(0x01))[5] == 1);
    EXPECT(core.pendingSmsCount() == 0);
    EXPECT(!core.ackLastSms(true, 0, 0));  // nothing outstanding -> NO_SMS_TO_ACK
    // stored C acknowledged positively -> deleted only now
    fm.indicate(kSvcWms, storedInd(9));
    EXPECT(waitFor([&] { return l->nSms() == sms0 + 4; }));
    drain(core);
    EXPECT(count(fm, kSvcWms, wms::kDelete) == del0);
    EXPECT(core.ackLastSms(true, 0, 0));
    EXPECT(count(fm, kSvcWms, wms::kDelete) == del0 + 1);
    auto d = last(fm, kSvcWms, wms::kDelete);
    EXPECT(d && d->get(0x10) && (*d->get(0x10))[0] == 9);
    // duplicate stored indication while queued -> delivered once
    fm.indicate(kSvcWms, storedInd(11));
    fm.indicate(kSvcWms, storedInd(11));
    EXPECT(waitFor([&] { return l->nSms() == sms0 + 5; }));
    drain(core);
    drain(core);
    EXPECT(core.pendingSmsCount() == 1);
    EXPECT(core.ackLastSms(true, 0, 0));
}

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    testVoice();
    testSms();
    printf("a6l-review5-deep tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    std::_Exit(gFail ? 1 : 0);
}
