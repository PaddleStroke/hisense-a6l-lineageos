// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 review pass2/deep fixes (28 Sep 2026) in the real hal/ModemCore.cpp over the fake modem
// (reuses the modemcore_tests.cc fake card/modem/listener; single slot):
//  F18 rejected radio power-off: failure reported, radioOn()/vote unchanged; rejection with the modem already in the
//      target mode counts as applied (readback reconciliation),
//  F29 an older serving-system query answer does not overwrite a newer roaming indication,
//  F17 VOICE alone withdrawn: cached calls + voice path cleared; on return re-registered once; WMS likewise,
//  F16 fatal receive error on the control client: modem lost -> transport reopened -> re-initialized.
#define main modemcore_tests_main
#include "modemcore_tests.cc"
#undef main

#include <future>

static std::atomic<bool> gRejectPower{false}, gHoldServing{false}, gServingEntered{false};
static std::promise<void> gReleaseServing;
static std::shared_future<void> gServingGate = gReleaseServing.get_future().share();

struct FaultTransport : Transport {
    std::unique_ptr<Transport> inner;
    std::atomic<bool> fail{false};
    std::atomic<int> opens{0};
    explicit FaultTransport(std::unique_ptr<Transport> t) : inner(std::move(t)) {}
    bool open() override {
        ++opens;
        return inner->open();
    }
    void close() override { inner->close(); }
    uint32_t localNode() const override { return inner->localNode(); }
    uint32_t localPort() const override { return inner->localPort(); }
    bool send(const Addr& a, const std::vector<uint8_t>& b) override { return inner->send(a, b); }
    int recv(Addr* a, std::vector<uint8_t>* b, int ms) override {
        if (fail.exchange(false)) return -1;
        return inner->recv(a, b, ms);
    }
};
static std::atomic<FaultTransport*> gCtlTransport{nullptr};

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady}});
    auto* modem = new FakeModem();
    modem->setHandler([](uint32_t svc, const Message& req) -> std::optional<Message> {
        if (gRejectPower && svc == kSvcDms && req.msgId == dms::kSetOperatingMode)
            return test::errResponse(req.msgId, kErrInternal);
        if (svc == kSvcNas && req.msgId == nas::kGetServingSystem && gHoldServing) {
            auto home = test::okResponse(req.msgId).raw(0x01, {1, 1, 1, 2, 1, nas::kRifLte}).u8(0x10, 1);
            gServingEntered = true;
            gServingGate.wait();
            return home;
        }
        return handler(svc, req);
    });
    ModemCore::setTransportFactory([modem]() -> std::unique_ptr<Transport> {
        auto t = std::make_unique<FaultTransport>(modem->transport());
        FaultTransport* none = nullptr;
        gCtlTransport.compare_exchange_strong(none, t.get());  // the first one is the control client's
        return t;
    });
    for (auto s : kSvcs) modem->addService(s);
    auto& core = ModemCore::get(1);
    auto* l = new L();
    core.addListener(l);
    core.start();
    EXPECT(waitFor([&] { return core.ready() && l->ready >= 1; }));

    // ---- F18
    EXPECT(core.setRadioPower(true));
    EXPECT(core.radioOn());
    gRejectPower = true;
    EXPECT(!core.setRadioPower(false));  // rejected
    EXPECT(core.radioOn());              // was: reported OFF with the modem ONLINE
    {
        std::lock_guard<std::mutex> g(gS.lock);
        EXPECT_EQ(gS.opMode, dms::kOnline);
    }
    EXPECT(ModemCore::powerVote().wants(0));
    EXPECT(hoststub::logContains("not applied; readback ok mode 0 (vote kept)"));
    // rejected, but the modem is already LOW_POWER (e.g. an earlier timed-out request went through): applied
    {
        std::lock_guard<std::mutex> g(gS.lock);
        gS.opMode = dms::kLowPower;
    }
    EXPECT(core.setRadioPower(false));
    EXPECT(!core.radioOn());
    EXPECT(!ModemCore::powerVote().wants(0));
    gRejectPower = false;
    EXPECT(core.setRadioPower(true));
    EXPECT(core.radioOn());

    // ---- F29: older query answer held while a newer roaming indication arrives
    gHoldServing = true;
    std::thread q([&] { core.serving(true); });
    EXPECT(waitFor([] { return gServingEntered.load(); }));
    Message roam(MsgType::Indication, nas::kGetServingSystem);
    roam.raw(0x01, {1, 1, 1, 2, 1, nas::kRifLte}).u8(0x10, 0);  // roaming indicator ON
    modem->indicate(kSvcNas, roam);
    EXPECT(waitFor([&] {
        auto s = core.serving();
        return s && s->roaming && *s->roaming;
    }));
    gReleaseServing.set_value();
    q.join();
    gHoldServing = false;
    {
        auto s = core.serving();
        EXPECT(s && s->roaming && *s->roaming);  // was: back to home (1 -> 0)
    }
    EXPECT(hoststub::logContains("serving system query answer dropped"));
    auto s2 = core.serving(true);  // a later query (no newer indication) is applied normally
    EXPECT(s2 && !(s2->roaming && *s2->roaming));

    // ---- F17: VOICE alone
    Message call(MsgType::Indication, voice::kAllCallStatusInd);
    call.raw(0x01, {1, 1, voice::kStateConversation, 0, voice::kDirMo, 3, 0, 0});
    modem->indicate(kSvcVoice, call);
    EXPECT(waitFor([] { return android::base::GetProperty("vendor.a6l.voice.active", "") == "1"; }));
    EXPECT_EQ(core.calls().size(), 1u);
    int reg = count(*modem, kSvcVoice, voice::kIndicationRegister);
    int getCalls = count(*modem, kSvcVoice, voice::kGetAllCallInfo);
    modem->removeService(kSvcVoice);
    EXPECT(waitFor([&] { return core.calls().empty(); }));
    EXPECT(waitFor([] { return android::base::GetProperty("vendor.a6l.voice.active", "") == "0"; }));
    EXPECT(core.ready());  // DMS/UIM/NAS healthy: no full re-init
    modem->addService(kSvcVoice);
    EXPECT(waitFor([&] { return count(*modem, kSvcVoice, voice::kIndicationRegister) == reg + 1; }));
    EXPECT(waitFor([&] { return count(*modem, kSvcVoice, voice::kGetAllCallInfo) >= getCalls + 1; }));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    EXPECT_EQ(count(*modem, kSvcVoice, voice::kIndicationRegister), reg + 1);  // once
    EXPECT(core.ready());
    // WMS alone
    int routes = count(*modem, kSvcWms, wms::kSetRoutes), evr = count(*modem, kSvcWms, wms::kSetEventReport);
    modem->removeService(kSvcWms);
    EXPECT(waitFor([&] { return !core.ctl().hasService(kSvcWms); }));
    modem->addService(kSvcWms);
    EXPECT(waitFor([&] { return count(*modem, kSvcWms, wms::kSetEventReport) == evr + 1; }));
    EXPECT(count(*modem, kSvcWms, wms::kSetRoutes) >= routes + 1);
    // a full modem restart still goes through initModem only (no extra VOICE registration from restoreService)
    int nasReg = count(*modem, kSvcNas, nas::kRegisterIndications);
    reg = count(*modem, kSvcVoice, voice::kIndicationRegister);
    int readyBefore = l->ready;
    for (auto s : kSvcs) modem->removeService(s);
    EXPECT(waitFor([&] { return !core.ready(); }));
    for (auto s : kSvcs) modem->addService(s);
    EXPECT(waitFor([&] { return core.ready() && l->ready > readyBefore; }, 20000));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    EXPECT_EQ(count(*modem, kSvcNas, nas::kRegisterIndications), nasReg + 1);
    EXPECT_EQ(count(*modem, kSvcVoice, voice::kIndicationRegister), reg + 1);

    // ---- F16: fatal receive error on the control client
    auto* ctl = gCtlTransport.load();
    EXPECT(ctl != nullptr);
    int opens = ctl ? ctl->opens.load() : 0;
    int lost = l->lost;
    readyBefore = l->ready;
    nasReg = count(*modem, kSvcNas, nas::kRegisterIndications);
    if (ctl) ctl->fail = true;
    EXPECT(waitFor([&] { return l->lost > lost; }));  // services reported down -> modem lost
    EXPECT(waitFor([&] { return core.ready() && l->ready > readyBefore; }, 20000));
    EXPECT(ctl && ctl->opens.load() == opens + 1);  // exactly one reopen
    EXPECT_EQ(count(*modem, kSvcNas, nas::kRegisterIndications), nasReg + 1);
    EXPECT(core.serving(true).has_value());

    printf("a6l-review5b-modemcore tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    std::_Exit(gFail ? 1 : 0);
}
