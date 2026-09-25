// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): offline unit tests for the QMI/QRTR layer.
// Vectors: libqmi src/libqmi-glib/test/test-generated.c (real modem captures, QMUX header
// stripped: the 7-byte QMI service header is identical on QRTR) and aospm/qrild notes (SDM845
// modem over QRTR, qmicli --verbose). Build: see tests/run-host-tests.sh (g++ or clang++, no deps).
#include "fake_modem.h"
#include "vectors.h"

#include <a6lqmi/datacall.h>
#include <a6lqmi/log.h>
#include <a6lqmi/message.h>
#include <a6lqmi/rmnet.h>
#include <a6lqmi/services.h>
#include <a6lqmi/sms.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;

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
#define EXPECT_EQ(a, b)                                                            \
    do {                                                                           \
        auto _a = (a);                                                             \
        auto _b = (b);                                                             \
        if (_a == _b) {                                                            \
            gPass++;                                                               \
        } else {                                                                   \
            gFail++;                                                               \
            fprintf(stderr, "FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
        }                                                                          \
    } while (0)

static std::vector<uint8_t> H(const char* s) { return *unhex(s); }
// strip the 6-byte QMUX header (01 len len flags svc cid) of libqmi/qmicli captures
static Message fromQmux(const std::vector<uint8_t>& q) {
    auto m = Message::decode(q.data() + 6, q.size() - 6);
    if (!m) {
        fprintf(stderr, "vector does not decode\n");
        abort();
    }
    return *m;
}

static void testCodec() {
    Message m = Message::request(0x0020);
    m.txn = 7;
    m.u8(0x10, 1).u16(0x11, 0x1234).u32(0x12, 0xA0B0C0D0).strNoLen(0x01, "123");
    auto b = m.encode();
    EXPECT_EQ(hex(b), std::string("00070020001600010300313233100100011102003412120400D0C0B0A0"));
    auto d = Message::decode(b);
    EXPECT(d.has_value());
    EXPECT_EQ(d->txn, 7);
    EXPECT_EQ(d->msgId, 0x20);
    EXPECT_EQ(Reader(d->get(0x12)).u32(), 0xA0B0C0D0u);
    // truncated TLV must be rejected
    b.pop_back();
    EXPECT(!Message::decode(b).has_value());
    // reader bounds
    std::vector<uint8_t> v{1, 2};
    Reader r(v);
    EXPECT_EQ(r.u16(), 0x0201);
    r.u8();
    EXPECT(!r.good());
}

static void testDmsIdsReal() {
    // libqmi test_generated_dms_get_ids response
    auto m = fromQmux(H(kDmsGetIds));
    auto ids = dms::parseIds(m);
    EXPECT(m.resultOk());
    EXPECT_EQ(ids->imei, std::string("359225050039973"));
    EXPECT_EQ(ids->esn, std::string("80997874"));
    EXPECT_EQ(ids->meid, std::string("35922505003997"));
    EXPECT_EQ(ids->imeisv, std::string("B"));
}

static void testServingSystem() {
    // libqmi test_generated_nas_get_serving_system response (Iliad, FR)
    auto m = fromQmux(H(kNasGetServingSystem));
    auto s = nas::parseServingSystem(m, false);
    EXPECT(s.has_value());
    EXPECT_EQ(s->regState, nas::kRegistered);
    EXPECT_EQ(s->csAttach, nas::kAttached);
    EXPECT_EQ(s->psAttach, nas::kAttached);
    EXPECT_EQ(s->primaryRat(), nas::kRifUmts);
    EXPECT(s->roaming.has_value() && !*s->roaming);
    EXPECT_EQ(s->dataCaps.size(), 3u);
    EXPECT_EQ(s->mcc, 222);
    EXPECT_EQ(s->mnc, 50);
    EXPECT_EQ(s->mccStr(), std::string("222"));
    EXPECT_EQ(s->mncStr(), std::string("50"));
    EXPECT_EQ(s->description, std::string("Iliad"));
    EXPECT(s->lac && *s->lac == 0x5FB4);
    EXPECT(s->cid && *s->cid == 0x01135ACFu);
    EXPECT(!s->mnc3Digits);
}

static void testCardStatus() {
    // aospm/qrild notes/uim_get_card_status.txt (SDM845 over QRTR): card 0 USIM ready, card 1 no ATR
    auto m = fromQmux(H(kUimGetCardStatus));
    auto* t = m.get(0x10);
    EXPECT(t != nullptr);
    auto cs = uim::parseCardStatus(*t);
    EXPECT(cs.has_value());
    EXPECT_EQ(cs->indexGwPrimary, 0x0000);
    EXPECT_EQ(cs->cards.size(), 2u);
    EXPECT_EQ(cs->cards[0].state, uim::kCardPresent);
    EXPECT_EQ(cs->cards[1].state, uim::kCardError);
    EXPECT_EQ(cs->cards[1].error, 3);
    auto* app = cs->primaryGwApp();
    EXPECT(app != nullptr);
    EXPECT_EQ(app->type, uim::kAppUsim);
    EXPECT_EQ(app->state, uim::kAppStateReady);
    EXPECT_EQ(app->pin1State, uim::kPinDisabled);
    EXPECT_EQ(app->pin1Retries, 3);
    EXPECT_EQ(app->puk1Retries, 10);
    EXPECT_EQ(hex(app->aid), std::string("A0000000871002FF44FF128900000100"));
}

static void testFileAttributes() {
    // aospm/qrild notes/dev-notes.md: UIM Get File Attributes of EF 6F62 (QMUX header stripped)
    auto m = fromQmux(H(kUimGetFileAttributes6F62));
    auto a = uim::parseFileAttributes(m);
    EXPECT(a.has_value());
    EXPECT_EQ(a->fileSize, 10);
    EXPECT_EQ(a->fileId, 0x6F62);
    EXPECT_EQ(a->fileType, uim::kFileTransparent);
    EXPECT(a->hasSw && a->sw1 == 0x90 && a->sw2 == 0x00);
    EXPECT_EQ(a->raw.size(), 30u);
    auto g = uim::toGsmGetResponse(*a);
    EXPECT_EQ(hex(g), std::string("0000000A6F62040000000001020000"));
}

static void testEfDecoders() {
    EXPECT_EQ(uim::decodeImsi(H("082980511032547698")), std::string("208150123456789"));
    // ICCID 8933150319000000000F (19 digits + F)
    EXPECT_EQ(uim::decodeIccid(H("983351301900000000F0")), std::string("8933150391000000000"));
}

static void testFilePathEncoding() {
    auto m = uim::buildReadTransparent(uim::Session{uim::kSessionPrimaryGw, {}},
                                       uim::FilePath{0x6F07, {0x3F00, 0x7FFF}}, 0, 0);
    EXPECT_EQ(hex(*m.get(0x01)), std::string("0000"));
    EXPECT_EQ(hex(*m.get(0x02)), std::string("076F04003FFF7F"));
    EXPECT_EQ(hex(*m.get(0x03)), std::string("00000000"));
}

static void testDescriptionAndIp() {
    EXPECT_EQ(decodeNetworkDescription(std::string("\x49\x76\x3A\x4C\x06", 5)), std::string("Iliad"));
    EXPECT_EQ(decodeNetworkDescription("Orange F"), std::string("Orange F"));
    EXPECT_EQ(ipv4ToString(0x0A000001), std::string("10.0.0.1"));
    EXPECT_EQ(maskToPrefix(0xFFFFFFFC), 30);
    uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    EXPECT_EQ(ipv6ToString(v6), std::string("2001:db8::1"));
}

static void testRmnetBuilder() {
    // aospm/qrild notes/netlink.md: iproute2 `ip link add link rmnet_ipa0 name rmnet_data0 type
    // rmnet mux_id 1 egress-chksumv4 ingress-chksumv4 ingress-deaggregation` (rmnet_ipa0 ifindex 3)
    auto want = H(kIproute2NewLinkRmnet);
    auto got = rmnet::buildNewLink(0, 3, "rmnet_data0", 1, 0x0d);
    // we add NLM_F_ACK (0x04) to get the kernel verdict; iproute2 capture did not
    EXPECT_EQ(got[6], 0x05);
    got[6] &= ~0x04;
    EXPECT_EQ(hex(got), hex(want));
}

static void testSms() {
    auto p = sms::buildSubmit("+33612345678", "hello");
    EXPECT(p.has_value());
    EXPECT_EQ(hex(*p), std::string("0001000B913316325476F8000005E8329BFD06"));
    EXPECT(!sms::buildSubmit("+331", "caf\xc3\xa9").has_value());
    // classic SMS-DELIVER example: from +31641600986 "How are you?"
    auto d = sms::parseDeliver(H("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07"));
    EXPECT(d.has_value());
    EXPECT_EQ(d->originator, std::string("+31641600986"));
    EXPECT_EQ(d->text, std::string("How are you?"));
    EXPECT_EQ(d->smsc, std::string("+31624000000"));
    sms::SmscForm f = sms::SmscForm::Auto;
    EXPECT(sms::parseDeliver(H("07911326040000F0040B911346610089F60000208062917314080CC8F71D14969741F977FD07"),
                             sms::SmscForm::Auto, &f).has_value());
    EXPECT(f == sms::SmscForm::Prefixed);

    // Real A6L captures 24 Sep 2026: WMS transfer route delivers the bare TPDU (no SMSC prefix)
    const char* loop = "040B913366336322F80000629042418444800D411B13C47EBFE1207A794E07";
    auto a = sms::parseDeliver(H(loop), sms::SmscForm::Auto, &f);
    EXPECT(a.has_value());
    EXPECT(f == sms::SmscForm::Bare);
    EXPECT_EQ(a->text, std::string("A6L loop test"));
    EXPECT_EQ(a->originator, std::string("+33663336228"));
    EXPECT_EQ(a->smsc, std::string(""));
    EXPECT_EQ(a->timestamp, std::string("260924144844"));
    EXPECT(!a->statusReport);
    auto b = sms::parseDeliver(H("040B913366336322F80000629042416403800A411B13442FCFE9A018"));
    EXPECT(b.has_value());
    EXPECT_EQ(b->text, std::string("A6L test 1"));
    EXPECT_EQ(b->originator, std::string("+33663336228"));
    // HAL normalisation for Android newSms(): 00 prepended once, idempotent
    auto pre = sms::toSmscPrefixed(H(loop));
    EXPECT_EQ(hex(pre), std::string("00") + loop);
    EXPECT_EQ(hex(sms::toSmscPrefixed(pre)), hex(pre));
    auto c = sms::parseDeliver(pre, sms::SmscForm::Auto, &f);
    EXPECT(c.has_value());
    EXPECT(f == sms::SmscForm::Prefixed);
    EXPECT_EQ(c->text, std::string("A6L loop test"));
    // same message with a real SMSC prefix (+33689004000, Orange FR)
    auto e = sms::parseDeliver(H((std::string("07913386094000F0") + loop).c_str()));
    EXPECT(e.has_value());
    EXPECT_EQ(e->smsc, std::string("+33689004000"));
    EXPECT_EQ(e->text, std::string("A6L loop test"));
    // bare status report: MR 05, RA +33663336228, SCTS, DT, ST 00
    auto sr = sms::parseDeliver(H("06050B913366336322F862904241844480629042418454800" "0"));
    EXPECT(sr.has_value());
    EXPECT(sr->statusReport);
    EXPECT_EQ(sr->originator, std::string("+33663336228"));
    EXPECT(sms::detectForm(H("06050B913366336322F8629042418444806290424184548000")) == sms::SmscForm::Bare);
    // forced forms still work
    EXPECT(sms::parseDeliver(H(loop), sms::SmscForm::Bare).has_value());
    EXPECT(!sms::parseDeliver(H("FF")).has_value());
}

static void testWmsAndVoiceParsers() {
    Message ind(MsgType::Indication, wms::kSetEventReport);
    std::vector<uint8_t> tr{0x00, 0x78, 0x56, 0x34, 0x12, 0x06, 0x03, 0x00, 0xAA, 0xBB, 0xCC};
    ind.raw(0x11, tr);
    auto e = wms::parseEventReport(ind);
    EXPECT(e.hasTransfer);
    EXPECT_EQ(e.txn, 0x12345678u);
    EXPECT_EQ(e.format, wms::kFormatGwPp);
    EXPECT_EQ(hex(e.data), std::string("AABBCC"));

    Message c(MsgType::Indication, voice::kAllCallStatusInd);
    c.raw(0x01, {2, 1, voice::kStateConversation, 0, voice::kDirMo, 3, 0, 0,
                 2, voice::kStateIncoming, 0, voice::kDirMt, 3, 0, 0});
    c.raw(0x10, {1, 2, 0, 4, '1', '2', '3', '4'});
    auto calls = voice::parseCalls(c, true);
    EXPECT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].number, std::string("1234"));
    EXPECT(!calls[0].hasNumber);
}

// ---------------------------------------------------------------- client over fake modem
static std::optional<Message> modemHandler(uint32_t svc, const Message& req) {
    using test::okResponse;
    if (svc == kSvcDms && req.msgId == dms::kGetIds) {
        auto r = okResponse(req.msgId);
        r.strNoLen(0x11, "867400022047199");
        return r;
    }
    if (svc == kSvcDms && req.msgId == dms::kSetOperatingMode) return okResponse(req.msgId);
    if (svc == kSvcWds && req.msgId == wds::kStartNetwork) {
        auto* fam = req.get(0x19);
        auto r = okResponse(req.msgId);
        r.u32(0x01, fam && (*fam)[0] == 6 ? 0x2222 : 0x1111);
        return r;
    }
    if (svc == kSvcWds && req.msgId == wds::kGetCurrentSettings) {
        auto r = okResponse(req.msgId);
        Reader mask(req.get(0x10));
        uint32_t mk = mask.u32();
        if (mk & wds::kReqDnsAddress) {
            r.u32(0x15, 0x0A0B0C0D);  // 10.11.12.13
            r.u32(0x16, 0x08080808);
            std::vector<uint8_t> d6{0x20, 0x01, 0x48, 0x60, 0x48, 0x60, 0, 0, 0, 0, 0, 0, 0, 0, 0x88, 0x88};
            r.raw(0x27, d6);
        }
        r.u32(0x1E, 0x0A000005);
        r.u32(0x20, 0x0A000006);
        r.u32(0x21, 0xFFFFFFFC);
        r.u32(0x29, 1430);
        std::vector<uint8_t> a6{0x2a, 0x01, 0x0c, 0xb8, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 64};
        r.raw(0x25, a6);
        r.strNoLen(0x14, "free");
        return r;
    }
    if (svc == kSvcWds) return okResponse(req.msgId);  // bind mux, set family, stop
    if (svc == kSvcNas && req.msgId == nas::kGetSignalInfo) return std::nullopt;  // never answers
    return test::errResponse(req.msgId, kErrInvalidQmiCommand);
}

static void testClient() {
    FakeModem modem;
    modem.addService(kSvcDms);
    modem.addService(kSvcNas);
    modem.setHandler(modemHandler);
    Client c(modem.transport(), "test");
    std::atomic<int> svcUp{0}, svcDown{0}, inds{0};
    c.onServiceChange([&](uint32_t, bool up) { (up ? svcUp : svcDown)++; });
    c.onIndication(kSvcNas, nas::kGetServingSystem, [&](const Message&) { inds++; });
    EXPECT(c.start({kSvcDms, kSvcNas, kSvcWds}));
    EXPECT(c.waitForServices({kSvcDms, kSvcNas}, 1000).empty());
    auto missing = c.waitForServices({kSvcWds}, 200);
    EXPECT_EQ(missing.size(), 1u);
    dms::Ids ids;
    auto r = dms::getIds(c, &ids);
    EXPECT(r.ok());
    EXPECT_EQ(ids.imei, std::string("867400022047199"));
    // unknown message -> QMI failure with the modem's error code
    std::string rev;
    r = dms::getRevision(c, &rev);
    EXPECT_EQ(r.status, Result::QmiFailure);
    EXPECT_EQ(r.qmiError, kErrInvalidQmiCommand);
    // timeout path
    auto t0 = std::chrono::steady_clock::now();
    r = c.request(kSvcNas, Message::request(nas::kGetSignalInfo), 300);
    EXPECT_EQ(r.status, Result::Timeout);
    EXPECT(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(280));
    // indication dispatch
    Message ind(MsgType::Indication, nas::kGetServingSystem);
    ind.raw(0x01, {1, 1, 1, 2, 1, 8});
    modem.indicate(kSvcNas, ind);
    // late service arrival + removal
    modem.addService(kSvcWds);
    EXPECT(c.waitForServices({kSvcWds}, 1000).empty());
    modem.removeService(kSvcWds);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT(!c.hasService(kSvcWds));
    r = c.request(kSvcWds, Message::request(wds::kGetCurrentSettings), 300);
    EXPECT_EQ(r.status, Result::NoService);
    EXPECT(inds.load() == 1);
    EXPECT(svcUp.load() >= 3);
    EXPECT(svcDown.load() == 1);
    c.stop();
}

static void testDataCallModemOnly() {
    FakeModem modem;
    modem.addService(kSvcWds);
    modem.addService(kSvcWda);
    modem.setHandler(modemHandler);
    radio::DataConfig cfg;
    cfg.requireNetdev = false;  // host has no rmnet_ipa0
    cfg.setDataFormat = false;
    radio::DataCallManager dm(cfg, [&](const char* tag) {
        return std::make_unique<Client>(modem.transport(), tag);
    }, nullptr);
    radio::DataRequest rq;
    rq.apn = "free";
    rq.protocol = radio::Protocol::V4V6;
    auto o = dm.setup(rq);
    EXPECT(o.ok);
    EXPECT_EQ(o.call.ifname, std::string("rmnet_data0"));
    EXPECT_EQ(o.call.cid, 1);
    bool dnsOk = false, dns6Ok = false, addrOk = false;
    for (auto& d : o.call.dnses) {
        if (d == "10.11.12.13") dnsOk = true;
        if (d == "2001:4860:4860::8888") dns6Ok = true;
    }
    for (auto& a : o.call.addresses)
        if (a == "10.0.0.5/30") addrOk = true;
    EXPECT(dnsOk);
    EXPECT(dns6Ok);
    EXPECT(addrOk);
    EXPECT_EQ(o.call.mtuV4, 1430);
    EXPECT_EQ(o.call.gateways[0], std::string("10.0.0.6"));
    // the request must have asked for DNS (the qrild bug)
    bool askedDns = false;
    for (auto& [svc, m] : modem.requests())
        if (svc == kSvcWds && m.msgId == wds::kGetCurrentSettings) {
            Reader r(m.get(0x10));
            askedDns = (r.u32() & wds::kReqDnsAddress) != 0;
        }
    EXPECT(askedDns);
    EXPECT_EQ(dm.list().size(), 1u);
    EXPECT(dm.deactivate(1));
    EXPECT_EQ(dm.list().size(), 0u);
    int stops = 0;
    for (auto& [svc, m] : modem.requests())
        if (svc == kSvcWds && m.msgId == wds::kStopNetwork) stops++;
    EXPECT_EQ(stops, 2);
}

static void testDataCallNoNetdev() {
    FakeModem modem;
    modem.setHandler(modemHandler);
    radio::DataConfig cfg;
    cfg.parentIface = "a6l_no_such_if0";
    radio::DataCallManager dm(cfg, [&](const char* tag) {
        return std::make_unique<Client>(modem.transport(), tag);
    }, nullptr);
    auto o = dm.setup(radio::DataRequest{});
    EXPECT(!o.ok);
    EXPECT_EQ(o.failCause, 0x1001);
}

int main(int argc, char** argv) {
    gLogLevel = (argc > 1 && !strcmp(argv[1], "-v")) ? 4 : 0;
    testCodec();
    testDmsIdsReal();
    testServingSystem();
    testCardStatus();
    testFileAttributes();
    testEfDecoders();
    testFilePathEncoding();
    testDescriptionAndIp();
    testRmnetBuilder();
    testSms();
    testWmsAndVoiceParsers();
    testClient();
    testDataCallModemOnly();
    testDataCallNoNetdev();
    printf("a6l-qmi tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
