// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 review fixes (28 Sep 2026, docs/hardware-review-20260928.md):
//  F6  call mute client of a6l-q6voiced (hal/VoiceMute.cpp) against a fake daemon socket: reply parsing, error classes
//      (no tx-mute kernel control, DSP refusal, daemon absent, no reply),
//  F9  data roaming permission decision (hal/DataPolicy.h),
//  F12 WMS Set Broadcast Config wire format, range normalisation and modem refusal propagation (fake modem).
#include "fake_modem.h"

#include "DataPolicy.h"
#include "VoiceMute.h"

#include <a6lqmi/services.h>

#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

using namespace a6l::qmi;
using a6l::test::FakeModem;
using a6l::test::errResponse;
using a6l::test::okResponse;
namespace hal = android::hardware::radio::a6l;

static int gFail = 0, gPass = 0;
#define EXPECT_HEX(a, b)                                                                     \
    do {                                                                                     \
        std::string _a = (a);                                                                \
        if (_a == (b)) gPass++;                                                              \
        else { gFail++; fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, _a.c_str(), b); } \
    } while (0)
#define EXPECT(c)                                                                            \
    do {                                                                                     \
        if (c) gPass++;                                                                      \
        else { gFail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); }        \
    } while (0)

// ------------------------------------------------------------------ F6
struct FakeDaemon {
    std::string path;
    int fd = -1;
    std::thread t;
    std::string lastCmd;
    FakeDaemon(const std::string& p, std::string reply, bool silent = false) : path(p) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
        unlink(path.c_str());
        if (bind(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) || listen(fd, 1)) perror("fake daemon");
        t = std::thread([this, reply, silent] {
            int c = accept(fd, nullptr, nullptr);
            if (c < 0) return;
            char b[64] = {};
            ssize_t n = read(c, b, sizeof(b) - 1);
            if (n > 0) lastCmd.assign(b, static_cast<size_t>(n));
            if (silent) usleep(400 * 1000);
            else if (write(c, reply.data(), reply.size()) < 0) perror("fake daemon write");
            close(c);
        });
    }
    ~FakeDaemon() {
        t.join();
        close(fd);
        unlink(path.c_str());
    }
};

static void testVoiceMute() {
    using K = hal::VoiceMuteReply;
    auto p = hal::parseQ6voicedReply("OK mute=1");
    EXPECT(p.ok() && p.mute);
    p = hal::parseQ6voicedReply("OK mute=0");
    EXPECT(p.ok() && !p.mute);
    EXPECT(hal::parseQ6voicedReply("ERR 2 No such file or directory").kind == K::Unsupported);
    EXPECT(hal::parseQ6voicedReply("ERR 5 Input/output error").kind == K::DspError);
    EXPECT(hal::parseQ6voicedReply("ERR 110 Connection timed out").kind == K::DspError);
    EXPECT(hal::parseQ6voicedReply("ERR 19 No such device").kind == K::DspError);
    EXPECT(hal::parseQ6voicedReply("ERR 22 unknown command").kind == K::BadRequest);
    EXPECT(hal::parseQ6voicedReply("OK mute=7").kind == K::Protocol);
    EXPECT(hal::parseQ6voicedReply("").kind == K::Protocol);

    const std::string path = "/tmp/a6l-review5-q6voiced.sock";
    {
        FakeDaemon d(path, "OK mute=1\n");
        auto r = hal::setVoiceTxMute(true, path.c_str());
        EXPECT(r.ok() && r.mute);
        EXPECT(d.lastCmd == "mute 1\n");
    }
    {
        FakeDaemon d(path, "ERR 5 Input/output error\n");  // DSP refused VSS_IVOLUME_CMD_MUTE_V2
        auto r = hal::setVoiceTxMute(false, path.c_str());
        EXPECT(!r.ok() && r.kind == K::DspError && r.err == EIO);
        EXPECT(d.lastCmd == "mute 0\n");
    }
    {
        FakeDaemon d(path, "ERR 2 No such file or directory\n");  // kernel without the tx-mute patch
        EXPECT(hal::setVoiceTxMute(true, path.c_str()).kind == K::Unsupported);
    }
    {
        FakeDaemon d(path, "OK mute=0\n");
        auto r = hal::getVoiceTxMute(path.c_str());
        EXPECT(r.ok() && !r.mute && d.lastCmd == "getmute\n");
    }
    {
        FakeDaemon d(path, "", true);  // accepts, never answers
        auto r = hal::q6voicedRequest("mute 1", path.c_str(), 150);
        EXPECT(!r.ok() && r.kind == K::Protocol && r.err == ETIMEDOUT);
    }
    auto none = hal::setVoiceTxMute(true, "/tmp/a6l-review5-no-daemon.sock");
    EXPECT(!none.ok() && none.kind == K::NoDaemon);
}

// ------------------------------------------------------------------ F9
static void testRoaming() {
    using hal::RoamingVerdict;
    nas::ServingSystem home;
    home.regState = nas::kRegistered;
    home.roaming = false;
    nas::ServingSystem roam = home;
    roam.roaming = true;
    nas::ServingSystem noInd = home;
    noInd.roaming.reset();
    EXPECT(hal::checkDataRoaming(home, false, false) == RoamingVerdict::Allow);
    EXPECT(hal::checkDataRoaming(home, true, false) == RoamingVerdict::Allow);
    EXPECT(hal::checkDataRoaming(roam, true, false) == RoamingVerdict::Allow);
    EXPECT(hal::checkDataRoaming(roam, false, false) == RoamingVerdict::RejectRoaming);
    EXPECT(hal::checkDataRoaming(roam, false, true) == RoamingVerdict::Allow);  // emergency PDN
    EXPECT(hal::checkDataRoaming(noInd, false, false) == RoamingVerdict::Allow);
    EXPECT(hal::checkDataRoaming(std::nullopt, false, false) == RoamingVerdict::Allow);
    EXPECT(hal::servingIsRoaming(roam) && !hal::servingIsRoaming(home) && !hal::servingIsRoaming(std::nullopt));
    EXPECT(hal::kFailCauseDataRoamingSettingsDisabled == 0x810);
}

// ------------------------------------------------------------------ F12
static void testBroadcast() {
    auto m = wms::buildSetBroadcastConfig({{4352, 4354, true}, {4370, 4370, false}});
    m.txn = 1;
    // request, txn 1, msg 0x003D, TLV 0x01 {01}, TLV 0x10 {02, 0x1100..0x1102 sel, 0x1112..0x1112 unsel}
    EXPECT_HEX(hex(m.encode()), "0001003D00120001010001100B000200110211011211121100");

    std::vector<wms::BroadcastRange> out;
    EXPECT(wms::normalizeBroadcastRanges({{4371, 4372, true}, {4352, 4354, true}, {4355, 4360, true}, {50, 50, false},
                                          {4353, 4353, true}},
                                         &out));
    EXPECT((out == std::vector<wms::BroadcastRange>{{4352, 4360, true}, {4371, 4372, true}, {50, 50, false}}));
    std::vector<wms::BroadcastRange> many;
    for (uint16_t i = 0; i < 60; i++) many.push_back({static_cast<uint16_t>(i * 10), static_cast<uint16_t>(i * 10), true});
    EXPECT(!wms::normalizeBroadcastRanges(many, &out));
    EXPECT(wms::normalizeBroadcastRanges(std::vector<wms::BroadcastRange>(many.begin(), many.begin() + 50), &out));
    EXPECT(out.size() == 50);

    FakeModem fm;
    fm.addService(kSvcWms);
    std::atomic<int> mode{0};
    fm.setHandler([&](uint32_t, const Message& r) -> std::optional<Message> {
        if (mode == 1) return errResponse(r.msgId, kErrInvalidArgument);
        if (mode == 2) return errResponse(r.msgId, kErrDeviceUnsupported);
        return okResponse(r.msgId);
    });
    Client c(fm.transport(), "t");
    EXPECT(c.start({kSvcWms}));
    EXPECT(c.waitForServices({kSvcWms}, 2000).empty());
    EXPECT(wms::setBroadcastConfig(c, {{4352, 4356, true}}).ok());
    mode = 1;  // modem refusal: returned, not swallowed
    auto r = wms::setBroadcastConfig(c, {{4352, 4356, true}});
    EXPECT(!r.ok() && r.status == Result::QmiFailure && r.qmiError == kErrInvalidArgument);
    mode = 2;
    r = wms::setBroadcastActivation(c, true);
    EXPECT(!r.ok() && r.qmiError == kErrDeviceUnsupported);
    size_t before = fm.requests().size();
    r = wms::setBroadcastConfig(c, many);  // > 50: refused locally, nothing sent
    EXPECT(!r.ok() && r.qmiError == kErrInvalidArgument && fm.requests().size() == before);
    int cfg = 0, act = 0;
    for (auto& [svc, q] : fm.requests()) {
        if (svc != kSvcWms) continue;
        if (q.msgId == wms::kSetBroadcastConfig) {
            cfg++;
            auto* t = q.get(0x01);
            EXPECT(t && t->size() == 1 && (*t)[0] == 1);
        }
        if (q.msgId == wms::kSetBroadcastActivation) act++;
    }
    EXPECT(cfg == 2 && act == 1);
    c.stop();
}

int main() {
    testVoiceMute();
    testRoaming();
    testBroadcast();
    printf("a6l-review5 tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
