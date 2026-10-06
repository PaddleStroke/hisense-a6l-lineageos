// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte3, 26 Sep 2026): offline tests for the VoLTE step 3/4 helpers: call domain
// (IMS vs CS) from the call info, typed dial, VoLTE indication register, audio RAT change, NAS subscription info
// (VSIDs, TLV layout from the stock IDL) and NAS IMS voice support. The fake modem answers like the stock IDL.
#include "fake_modem.h"

#include <a6lqmi/log.h>
#include <a6lqmi/services.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using a6l::test::okResponse;
using a6l::test::errResponse;

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

static voice::CallInfo call(uint8_t type, uint8_t mode, uint8_t state = voice::kStateConversation) {
    voice::CallInfo c;
    c.id = 1;
    c.state = state;
    c.type = type;
    c.mode = mode;
    return c;
}

static void testDomain() {
    using namespace voice;
    EXPECT_EQ(std::string(callDomain(call(kTypeVoice, kModeUmts))), std::string("cs"));  // CSFB call (25 Sep)
    EXPECT_EQ(std::string(callDomain(call(kTypeVoice, kModeGsm))), std::string("cs"));
    EXPECT_EQ(std::string(callDomain(call(kTypeVoiceIp, kModeLte))), std::string("ims"));
    EXPECT_EQ(std::string(callDomain(call(kTypeVoice, kModeLte))), std::string("ims"));  // modem reports VOICE on LTE
    EXPECT_EQ(std::string(callDomain(call(kTypeVoiceIp, kModeWlan))), std::string("ims-wlan"));
    EXPECT_EQ(std::string(callDomain(call(0x0B, kModeLte))), std::string("ims"));  // emergency over IMS
    EXPECT_EQ(std::string(callDomain(call(kTypeVoice, kModeNoSrv, kStateOrigination))), std::string("unknown"));
    EXPECT(isImsCall(call(kTypeVoiceIp, kModeNoSrv)));
    EXPECT(!isImsCall(call(kTypeEmergency, kModeUmts)));
    EXPECT_EQ(std::string(callTypeName(0x02)), std::string("voice-ip"));
    EXPECT_EQ(std::string(callModeName(kModeLte)), std::string("lte"));

    // all call status indication with an MT IMS call: parsed like any other call (HAL reports it the same way)
    Message ind(MsgType::Indication, kAllCallStatusInd);
    ind.raw(0x01, {1, 2, kStateIncoming, kTypeVoiceIp, kDirMt, kModeLte, 0, 0});  // n, id, state, type, dir, mode, mpty, als
    ind.raw(0x10, {1, 2, 0, 3, '1', '2', '3'});                                  // n, id, pi, len, number
    auto calls = parseCalls(*Message::decode(ind.encode()), true);
    EXPECT_EQ(calls.size(), size_t(1));
    if (!calls.empty()) {
        EXPECT_EQ(calls[0].state, uint8_t(kStateIncoming));
        EXPECT_EQ(calls[0].direction, uint8_t(kDirMt));
        EXPECT(isImsCall(calls[0]));
        EXPECT_EQ(calls[0].id, uint8_t(2));
        EXPECT_EQ(calls[0].number, std::string("123"));
        EXPECT_EQ(std::string(callDomain(calls[0])), std::string("ims"));
    }

    Message ar(MsgType::Indication, kAudioRatChangeInd);
    ar.u32(0x10, 1);
    ar.u8(0x11, kModeLte);
    auto a = parseAudioRatChange(*Message::decode(ar.encode()));
    EXPECT(a.sessionInfo && *a.sessionInfo == 1u);
    EXPECT(a.rat && *a.rat == kModeLte);
    auto empty = parseAudioRatChange(Message(MsgType::Indication, kAudioRatChangeInd));
    EXPECT(!empty.sessionInfo && !empty.rat);
}

static void testSubscriptionInfo() {
    Message r = okResponse(nas::kGetSubscriptionInfo);
    r.u8(0x10, 1);
    r.u8(0x11, 1);
    r.u8(0x12, 1);
    r.u32(0x13, 0x11C05000);
    r.u32(0x14, 0x10C02000);
    r.u32(0x15, 0x10002000);
    auto s = nas::parseSubscriptionInfo(*Message::decode(r.encode()));
    EXPECT(s.voiceVsid && *s.voiceVsid == 0x11C05000u);
    EXPECT(s.lteVoiceVsid && *s.lteVoiceVsid == 0x10C02000u);
    EXPECT(s.wlanVoiceVsid && *s.wlanVoiceVsid == 0x10002000u);
    EXPECT(s.active && *s.active == 1);
    std::string sum = s.summary();
    EXPECT(sum.find("cs_vsid=0x11C05000(VoiceMMode1)") != std::string::npos);
    EXPECT(sum.find("lte_vsid=0x10C02000(VoLTE)") != std::string::npos);
    EXPECT(sum.find("wlan_vsid=0x10002000(VoWLAN)") != std::string::npos);
    // short / missing TLVs are ignored
    Message b(MsgType::Indication, nas::kSubscriptionInfoInd);
    b.raw(0x14, {0x00, 0x50});
    auto s2 = nas::parseSubscriptionInfo(b);
    EXPECT(!s2.lteVoiceVsid && !s2.voiceVsid);
    EXPECT(s2.summary().find("lte_vsid=absent") != std::string::npos);
    EXPECT_EQ(std::string(nas::vsidName(0x10C01000)), std::string("CS-Voice"));
    EXPECT_EQ(std::string(nas::vsidName(0x12345678)), std::string("unknown"));
}

static void testOverFakeModem() {
    FakeModem fm;
    fm.addService(kSvcVoice);
    fm.addService(kSvcNas);
    std::atomic<int> dialType{-1}, indRegOk{0};
    std::string number;
    std::mutex nl;
    fm.setHandler([&](uint32_t svc, const Message& m) -> std::optional<Message> {
        if (svc == kSvcVoice && m.msgId == voice::kDialCall) {
            auto* t = m.get(0x10);
            dialType = t && t->size() == 1 ? (*t)[0] : -2;
            if (auto* n = m.get(0x01)) {
                std::lock_guard<std::mutex> l(nl);
                number.assign(n->begin(), n->end());
            }
            Message r = okResponse(m.msgId);
            r.u8(0x10, 3);
            return r;
        }
        if (svc == kSvcVoice && m.msgId == voice::kIndicationRegister) {
            bool ok = true;
            for (uint8_t t : {0x12, 0x13, 0x14, 0x15, 0x23}) {
                auto* v = m.get(t);
                ok = ok && v && v->size() == 1 && (*v)[0] == 1;
            }
            if (ok && m.tlvs.size() == 5) indRegOk++;
            return okResponse(m.msgId);
        }
        if (svc == kSvcNas && m.msgId == nas::kGetSubscriptionInfo) {
            Message r = okResponse(m.msgId);
            r.u32(0x13, 0x11C05000);
            r.u32(0x14, 0x11C05000);
            return r;
        }
        if (svc == kSvcNas && m.msgId == nas::kGetSystemInfo) {
            Message r = okResponse(m.msgId);
            r.u8(0x29, 1);
            r.u32(0x2A, 1);
            return r;
        }
        return errResponse(m.msgId, kErrNotSupported);
    });
    Client c(fm.transport(), "volte3");
    EXPECT(c.start({kSvcVoice, kSvcNas}));
    EXPECT(c.waitForServices({kSvcVoice, kSvcNas}, 2000).empty());
    uint8_t id = 0;
    EXPECT(voice::dialTyped(c, "+33600000000", voice::kTypeVoice, &id).ok());
    EXPECT_EQ(id, uint8_t(3));
    EXPECT_EQ(dialType.load(), 0);
    {
        std::lock_guard<std::mutex> l(nl);
        EXPECT_EQ(number, std::string("+33600000000"));
    }
    EXPECT(voice::dialTyped(c, "+33600000000", voice::kTypeVoiceIp, &id).ok());
    EXPECT_EQ(dialType.load(), 2);
    EXPECT(voice::indicationRegisterVolte(c).ok());
    EXPECT_EQ(indRegOk.load(), 1);
    nas::SubscriptionInfo si;
    EXPECT(nas::getSubscriptionInfo(c, &si).ok());
    EXPECT(si.lteVoiceVsid && *si.lteVoiceVsid == 0x11C05000u);
    EXPECT(!si.wlanVoiceVsid);
    nas::ImsVoiceSupport iv;
    EXPECT(nas::getImsVoiceSupport(c, &iv).ok());
    EXPECT(iv.lteImsVoice && *iv.lteImsVoice == 1);
    EXPECT(iv.lteVoiceDomain && *iv.lteVoiceDomain == 1u);
    // audio RAT change indication reaches a handler
    std::atomic<int> rat{-1};
    c.onIndication(kSvcVoice, voice::kAudioRatChangeInd, [&](const Message& m) {
        auto a = voice::parseAudioRatChange(m);
        rat = a.rat ? *a.rat : -2;
    });
    Message ind(MsgType::Indication, voice::kAudioRatChangeInd);
    ind.u32(0x10, 0);
    ind.u8(0x11, voice::kModeLte);
    fm.indicate(kSvcVoice, ind);
    for (int i = 0; i < 50 && rat.load() < 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_EQ(rat.load(), int(voice::kModeLte));
    c.stop();
}

int main(int argc, char** argv) {
    gLogLevel = (argc > 1 && !strcmp(argv[1], "-v")) ? 4 : 0;
    for (int round = 0; round < 3; round++) {
        testDomain();
        testSubscriptionInfo();
        testOverFakeModem();
    }
    printf("a6l-volte3 tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
