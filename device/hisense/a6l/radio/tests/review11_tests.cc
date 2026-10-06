// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 review round 11 (28 Sep 2026, docs/hardware-review-round11-20260928.md): host tests over the
// real hal/ModemCore.cpp and the fake modem (no phone, no SIM request, no clock change).
//  F17 follow-up: a VOICE/WMS registration the modem rejects stays pending and is retried (bounded exponential
//      backoff) at startup and after an isolated service return, without another withdrawal; unsupported steps are
//      not retried; withdrawal during backoff and a modem restart hand the service back to the loss/re-init path;
//      healthy services are not re-registered.
//  F65: NITZ keeps missing time zone / DST unknown (formatNitz), never "+0" / ",0"; no NITZ without a time zone.
// Build/run: tests/run-host-tests.sh (review11 binary).
#define main modemcore_tests_main
#include "modemcore_tests.cc"
#undef main

#include "../hal/TimePolicy.h"

#include <future>
#include <unistd.h>

using android::hardware::radio::a6l::formatNitz;

static std::atomic<uint16_t> gVoiceRegErr{0}, gWmsEventErr{0}, gWmsRoutesErr{0};
static std::atomic<int> gVoiceAccepted{0}, gWmsEventAccepted{0};

static std::optional<Message> r11Handler(uint32_t svc, const Message& req) {
    if (svc == kSvcVoice && req.msgId == voice::kIndicationRegister) {
        if (uint16_t e = gVoiceRegErr) return test::errResponse(req.msgId, e);
        gVoiceAccepted++;
    }
    if (svc == kSvcWms && req.msgId == wms::kSetEventReport) {
        if (uint16_t e = gWmsEventErr) return test::errResponse(req.msgId, e);
        gWmsEventAccepted++;
    }
    if (svc == kSvcWms && req.msgId == wms::kSetRoutes) {
        if (uint16_t e = gWmsRoutesErr) return test::errResponse(req.msgId, e);
    }
    return handler(svc, req);
}

struct TL : L {
    std::mutex tm;
    std::vector<std::string> times;
    void onNitz(const std::string& n, int64_t) override {
        std::lock_guard<std::mutex> g(tm);
        times.push_back(n);
    }
    size_t size() {
        std::lock_guard<std::mutex> g(tm);
        return times.size();
    }
};

static void drain(ModemCore& core) {
    auto p = std::make_shared<std::promise<void>>();
    auto f = p->get_future();
    core.post([p] { p->set_value(); });
    EXPECT(f.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
}
static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static void testFormatNitz() {
    auto f = [](std::optional<int> tz, std::optional<int> dst) { return formatNitz(2026, 9, 28, 12, 0, 0, tz, dst); };
    EXPECT(!f(std::nullopt, std::nullopt));                       // reviewer case 1: no "+0,0"
    EXPECT(!f(std::nullopt, 1));                                  // DST without a zone: still no offset
    EXPECT_EQ(*f(8, std::nullopt), std::string("26/09/28,12:00:00+8"));  // reviewer case 2: DST omitted (unknown)
    EXPECT_EQ(*f(8, 1), std::string("26/09/28,12:00:00+8,1"));           // positive control
    EXPECT_EQ(*f(0, 0), std::string("26/09/28,12:00:00+0,0"));           // explicitly supplied zeros stay zeros
    EXPECT_EQ(*f(0, std::nullopt), std::string("26/09/28,12:00:00+0"));
    EXPECT_EQ(*f(-20, std::nullopt), std::string("26/09/28,12:00:00-20"));  // UTC-5
    EXPECT_EQ(*f(22, 0), std::string("26/09/28,12:00:00+22,0"));           // UTC+5:30 (fractional hour)
    EXPECT_EQ(*f(-14, 2), std::string("26/09/28,12:00:00-14,2"));
    EXPECT_EQ(*f(56, 0), std::string("26/09/28,12:00:00+56,0"));           // UTC+14 bound
    EXPECT_EQ(*f(-48, 0), std::string("26/09/28,12:00:00-48,0"));          // UTC-12 bound
    EXPECT(!f(57, 0) && !f(-49, 0) && !f(100, 0) && !f(-128, 0));           // malformed zone -> no NITZ
    EXPECT_EQ(*f(8, 3), std::string("26/09/28,12:00:00+8"));                // invalid DST -> unknown, not a value
    EXPECT_EQ(*f(8, -1), std::string("26/09/28,12:00:00+8"));
    EXPECT(!formatNitz(2026, 13, 28, 12, 0, 0, 8, 0) && !formatNitz(2026, 9, 0, 12, 0, 0, 8, 0) &&
           !formatNitz(2026, 9, 28, 24, 0, 0, 8, 0));  // malformed date
}

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    testFormatNitz();

    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    gS.cardTlv = cardStatusTlv(0x0000, 0xFFFF, {{uim::kCardPresent, 0, uim::kAppStateReady}});
    auto* modem = new FakeModem();  // leaked: core threads outlive main
    modem->setHandler(r11Handler);
    ModemCore::setTransportFactory([modem] { return modem->transport(); });
    for (auto s : kSvcs) modem->addService(s);
    auto& core = ModemCore::get(1);
    auto* l = new TL();
    core.addListener(l);
    auto pendingHas = [&](uint32_t svc) { return core.pendingServiceSetup().count(svc) > 0; };
    auto voiceReq = [&] { return count(*modem, kSvcVoice, voice::kIndicationRegister); };
    auto evReq = [&] { return count(*modem, kSvcWms, wms::kSetEventReport); };
    auto routesReq = [&] { return count(*modem, kSvcWms, wms::kSetRoutes); };

    // ---- reviewer case 1: startup, VOICE registration + WMS event report rejected (transient QMI error)
    gVoiceRegErr = kErrInternal;
    gWmsEventErr = kErrInternal;
    core.start();
    EXPECT(waitFor([&] { return core.ready() && l->ready >= 1; }));
    drain(core);
    EXPECT(pendingHas(kSvcVoice) && pendingHas(kSvcWms));
    EXPECT(gVoiceAccepted == 0 && gWmsEventAccepted == 0);
    gVoiceRegErr = 0;
    gWmsEventErr = 0;
    EXPECT(waitFor([&] { return gVoiceAccepted == 1 && gWmsEventAccepted == 1; }, 4000));  // no withdrawal needed
    EXPECT(waitFor([&] { return core.pendingServiceSetup().empty(); }, 2000));
    EXPECT(core.ready());
    // healthy services are not re-registered
    int v0 = voiceReq(), e0 = evReq(), r0 = routesReq();
    sleepMs(2500);
    drain(core);
    EXPECT(voiceReq() == v0 && evReq() == e0 && routesReq() == r0);

    auto withdraw = [&](std::initializer_list<uint32_t> svcs) {
        for (auto s : svcs) modem->removeService(s);
        EXPECT(waitFor([&] {
            for (auto s : svcs)
                if (core.ctl().hasService(s)) return false;
            return true;
        }));
        drain(core);
    };
    auto publish = [&](std::initializer_list<uint32_t> svcs) {
        for (auto s : svcs) modem->addService(s);
    };

    // ---- healthy isolated return: positive control, exactly one registration each
    withdraw({kSvcVoice, kSvcWms});
    publish({kSvcVoice, kSvcWms});
    EXPECT(waitFor([&] { return gVoiceAccepted == 2 && gWmsEventAccepted == 2; }));
    drain(core);
    EXPECT(core.pendingServiceSetup().empty());

    // ---- reviewer case 2: isolated return with rejected registrations, then the fault clears
    gVoiceRegErr = kErrInternal;
    gWmsEventErr = kErrInternal;
    v0 = voiceReq();
    e0 = evReq();
    withdraw({kSvcVoice, kSvcWms});
    publish({kSvcVoice, kSvcWms});
    EXPECT(waitFor([&] { return voiceReq() > v0 && evReq() > e0; }));
    drain(core);
    EXPECT(pendingHas(kSvcVoice) && pendingHas(kSvcWms) && core.ready());
    gVoiceRegErr = 0;
    gWmsEventErr = 0;
    EXPECT(waitFor([&] { return gVoiceAccepted == 3 && gWmsEventAccepted == 3; }, 4000));
    EXPECT(waitFor([&] { return core.pendingServiceSetup().empty(); }, 2000));

    // ---- persistent failure: bounded exponential backoff (1 s, 2 s, 4 s ...), not a tight loop, never forgotten
    gVoiceRegErr = kErrInternal;
    v0 = voiceReq();
    withdraw({kSvcVoice});
    publish({kSvcVoice});
    EXPECT(waitFor([&] { return voiceReq() > v0; }));
    sleepMs(6500);  // attempts at ~0, 1, 3, 7 s
    int attempts = voiceReq() - v0;
    EXPECT(attempts >= 2 && attempts <= 4);
    EXPECT(pendingHas(kSvcVoice));
    // withdrawal during backoff: the loss path owns it, no request while it is absent
    withdraw({kSvcVoice});
    EXPECT(!pendingHas(kSvcVoice));
    int vGone = voiceReq();
    sleepMs(2500);
    EXPECT(voiceReq() == vGone);
    gVoiceRegErr = 0;
    publish({kSvcVoice});
    EXPECT(waitFor([&] { return gVoiceAccepted == 4; }));
    EXPECT(waitFor([&] { return core.pendingServiceSetup().empty(); }, 2000));

    // ---- WMS routes rejected (both the configured route and the store fallback): retried too
    gWmsRoutesErr = kErrInternal;
    r0 = routesReq();
    int evBefore = gWmsEventAccepted;
    withdraw({kSvcWms});
    publish({kSvcWms});
    EXPECT(waitFor([&] { return routesReq() >= r0 + 2; }));  // transfer-only then store-and-notify fallback
    drain(core);
    EXPECT(pendingHas(kSvcWms));
    int rFail = routesReq();
    gWmsRoutesErr = 0;
    EXPECT(waitFor([&] { return !pendingHas(kSvcWms); }, 4000));
    EXPECT(routesReq() > rFail);
    sleepMs(300);
    EXPECT_EQ(int(gWmsEventAccepted), evBefore + 1);  // event report accepted once; only the failed step retried

    // ---- unsupported step (NOT_SUPPORTED): optional, logged, not retried
    gWmsEventErr = kErrNotSupported;
    e0 = evReq();
    withdraw({kSvcWms});
    publish({kSvcWms});
    EXPECT(waitFor([&] { return evReq() > e0; }));
    drain(core);
    EXPECT(!pendingHas(kSvcWms));
    sleepMs(2500);
    EXPECT(evReq() == e0 + 1);
    gWmsEventErr = 0;

    // ---- modem restart while a retry is pending: re-init owns it, the new instance is registered once
    gVoiceRegErr = kErrInternal;
    v0 = voiceReq();
    withdraw({kSvcVoice});
    publish({kSvcVoice});
    EXPECT(waitFor([&] { return voiceReq() > v0 && pendingHas(kSvcVoice); }));
    int lostBefore = l->lost, readyBefore = l->ready;
    for (auto s : kSvcs) modem->removeService(s);
    EXPECT(waitFor([&] { return l->lost > lostBefore && !core.ready(); }));
    gVoiceRegErr = 0;
    int acceptedBefore = gVoiceAccepted;
    for (auto s : kSvcs) modem->addService(s);
    EXPECT(waitFor([&] { return core.ready() && l->ready > readyBefore; }, 20000));
    EXPECT(waitFor([&] { return core.pendingServiceSetup().empty(); }, 3000));
    sleepMs(1500);
    EXPECT_EQ(int(gVoiceAccepted), acceptedBefore + 1);

    // ---- F65 through the real NAS indication path
    auto timeInd = [&](std::optional<int8_t> tz, std::optional<uint8_t> dst) {
        Message m(MsgType::Indication, nas::kNetworkTimeInd);
        m.raw(0x01, {0xEA, 0x07, 9, 28, 12, 0, 0, 0});  // 2026-09-28 12:00:00 UTC
        if (tz) m.u8(0x10, static_cast<uint8_t>(*tz));
        if (dst) m.u8(0x11, *dst);
        modem->indicate(kSvcNas, m);
    };
    timeInd(std::nullopt, std::nullopt);  // reviewer case: was "+0,0"
    timeInd(8, std::nullopt);             // was "+8,0"
    timeInd(8, 1);                        // positive control
    timeInd(-20, 0);                      // negative offset, explicit zero DST
    timeInd(std::nullopt, 1);             // DST without zone: dropped
    EXPECT(waitFor([&] { return l->size() >= 3; }));
    sleepMs(300);
    drain(core);
    {
        std::lock_guard<std::mutex> g(l->tm);
        EXPECT_EQ(l->times.size(), size_t(3));
        if (l->times.size() == 3) {
            EXPECT_EQ(l->times[0], std::string("26/09/28,12:00:00+8"));
            EXPECT_EQ(l->times[1], std::string("26/09/28,12:00:00+8,1"));
            EXPECT_EQ(l->times[2], std::string("26/09/28,12:00:00-20,0"));
        }
    }

    printf("a6l-review11 (round 11) tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
