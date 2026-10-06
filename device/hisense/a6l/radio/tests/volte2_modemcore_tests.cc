// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent volte2, 25 Sep 2026): IMSA in the real hal/ModemCore.cpp over the fake
// modem: IMSA published late (after the core is ready) -> register indications + status query,
// registration / service indications -> cache + onImsChanged, IMSA withdrawn -> absent.
#include "../hal/ModemCore.h"
#include "fake_modem.h"

#include <a6lqmi/ims.h>
#include <a6lqmi/log.h>
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

static int gFail = 0, gPass = 0;
#define EXPECT(c)                                                                            \
    do {                                                                                     \
        if (c) gPass++;                                                                      \
        else { gFail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); }       \
    } while (0)

static std::atomic<uint32_t> gImsaStatus{imsa::kRegistering};

static std::optional<Message> handler(uint32_t svc, const Message& req) {
    auto ok = [&] { return test::okResponse(req.msgId); };
    switch (svc) {
        case kSvcDms:
            if (req.msgId == dms::kGetIds) return ok().strNoLen(0x11, "865947040141299");
            if (req.msgId == dms::kGetRevision) return ok().strNoLen(0x01, "MPSS.AT.3.1");
            if (req.msgId == dms::kGetOperatingMode) return ok().u8(0x01, dms::kOnline);
            return ok();
        case kSvcUim:
            if (req.msgId == uim::kGetCardStatus) return test::errResponse(req.msgId, kErrDeviceNotReady);
            return ok();
        case kSvcNas:
            if (req.msgId == nas::kGetServingSystem) return ok().raw(0x01, {1, 1, 1, 2, 1, nas::kRifLte});
            return ok();
        case kSvcImsa:
            if (req.msgId == imsa::kGetRegStatus) return ok().u8(0x10, 0).u32(0x12, gImsaStatus.load());
            if (req.msgId == imsa::kGetServicesStatus) return ok().u32(0x11, imsa::kSvcUnavailable);
            return ok();
        default:
            return ok();
    }
}

struct L : ModemCore::Listener {
    std::atomic<int> ready{0}, ims{0};
    void onModemReady() override { ready++; }
    void onImsChanged() override { ims++; }
};

template <typename F>
static bool waitFor(F f, int ms = 8000) {
    for (int i = 0; i < ms / 20; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return f();
}

int main(int argc, char** argv) {
    gLogLevel = 0;
    hoststub::logEcho() = argc > 1 && !strcmp(argv[1], "-v");
    android::base::SetProperty("ro.vendor.a6l.ril.slots", "1");
    auto* modem = new FakeModem();  // leaked: core threads outlive main
    modem->setHandler(handler);
    ModemCore::setTransportFactory([modem] { return modem->transport(); });
    for (auto s : {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda}) modem->addService(s);
    auto& c = ModemCore::get(1);
    auto* l = new L();
    c.addListener(l);
    c.start();
    EXPECT(waitFor([&] { return c.ready() && l->ready >= 1; }));
    EXPECT(!c.imsState().present);  // 25 Sep phone: IMSA not published

    // the modem IMS task starts (after online / after our IMSDCM answered) -> IMSA published
    modem->addService(kSvcImsa);
    EXPECT(waitFor([&] { return c.imsState().present && l->ims >= 1; }));
    auto st = c.imsState();
    EXPECT(st.reg.status && *st.reg.status == imsa::kRegistering);
    EXPECT(!st.registered());
    int regs = 0, binds = 0, bindFirst = -1;
    for (auto& [s, m] : modem->requests()) {
        if (s == kSvcImsa && m.msgId == imsa::kRegisterIndications && m.has(0x10) && m.has(0x11)) regs++;
        // volte5: bind to the primary subscription (0x0033 {0x10 u32 0}) before anything else on IMSA
        if (s == kSvcImsa && m.msgId == imsa::kBindSubscription && m.get(0x10) && Reader(*m.get(0x10)).u32() == 0) binds++;
        if (s == kSvcImsa && bindFirst < 0) bindFirst = m.msgId == imsa::kBindSubscription ? 1 : 0;
    }
    EXPECT(regs == 1);
    EXPECT(binds == 1);
    EXPECT(bindFirst == 1);

    Message ind(MsgType::Indication, imsa::kRegStatusInd);
    ind.u8(0x01, 1);
    ind.u32(0x11, imsa::kRegistered);
    ind.u32(0x13, imsa::kTechWwan);
    int before = l->ims;
    modem->indicate(kSvcImsa, ind);
    EXPECT(waitFor([&] { return c.imsState().registered() && l->ims > before; }));
    Message si(MsgType::Indication, imsa::kServicesStatusInd);
    si.u32(0x11, imsa::kSvcAvailable);
    si.u32(0x14, imsa::kTechWwan);
    si.u32(0x10, imsa::kSvcAvailable);
    modem->indicate(kSvcImsa, si);
    EXPECT(waitFor([&] { return c.imsState().voiceOverIms(); }));

    // IMSA withdrawn (modem IMS task stopped / SSR) -> absent, not registered
    before = l->ims;
    modem->removeService(kSvcImsa);
    EXPECT(waitFor([&] { return !c.imsState().present && l->ims > before; }));
    EXPECT(!c.imsState().registered());
    EXPECT(c.ready());  // IMSA loss is not a modem loss

    printf("a6l-volte2-modemcore tests: %d passed, %d failed\n", gPass, gFail);
    fflush(stdout);
    _exit(gFail ? 1 : 0);
}
