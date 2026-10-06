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
#include <a6lqmi/multisim.h>
#include <a6lqmi/rmnet.h>
#include <a6lqmi/services.h>
#include <a6lqmi/sms.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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

// data2 (25 Sep): DPM Open Port before WDA (the phone answered WDA INVALID_OPERATION without it).
static std::string hexOf(const std::vector<uint8_t>& v) { return hex(v); }

static void testDpmWdaEncoding() {
    dpm::HwDataPort p;  // defaults: embedded/1, rx 4, tx 5
    auto m = dpm::buildOpenPort({p});
    // TLV 0x11: count 1, ep_type 4, iface 1, rx_ep(consumer) 4, tx_ep(producer) 5 (all le32)
    EXPECT_EQ(hexOf(*m.get(0x11)), std::string("0104000000010000000400000005000000"));
    EXPECT(!m.has(0x10));
    EXPECT_EQ(m.msgId, 0x0020);
    // control port form (not used over QRTR, but keep the encoder right): name u8-prefixed
    dpm::CtlPort c;
    c.name = "DATA5_CNTL";
    auto m2 = dpm::buildOpenPort({}, {c});
    EXPECT_EQ(hexOf(*m2.get(0x10)), std::string("010A44415441355F434E544C0400000001000000"));
    EXPECT(!m2.has(0x11));
    wda::DataFormat f;
    auto w = wda::buildSetDataFormat(f);
    EXPECT_EQ(hexOf(*w.get(0x11)), std::string("02000000"));
    EXPECT_EQ(hexOf(*w.get(0x12)), std::string("05000000"));
    EXPECT_EQ(hexOf(*w.get(0x13)), std::string("05000000"));
    EXPECT_EQ(hexOf(*w.get(0x15)), std::string("20000000"));
    EXPECT_EQ(hexOf(*w.get(0x16)), std::string("00800000"));
    EXPECT_EQ(hexOf(*w.get(0x17)), std::string("0400000001000000"));
    if (getenv("A6L_DUMP_VECTORS")) {  // cross-checked with libqmi (docs/data2-20260925.md)
        printf("VEC dpm_open %s\n", hexOf(m.encode()).c_str());
        printf("VEC dpm_open_ctl %s\n", hexOf(m2.encode()).c_str());
        printf("VEC wda_set %s\n", hexOf(w.encode()).c_str());
        Message g = Message::request(wda::kGetDataFormat);
        g.raw(0x10, {4, 0, 0, 0, 1, 0, 0, 0});
        printf("VEC wda_get %s\n", hexOf(g.encode()).c_str());
    }
}

// Models the SDM660 modem: WDA Set/Get Data Format for ep 4/1 is INVALID_OPERATION until a DPM
// Open Port with that hardware data port (rx 4 / tx 5) was received.
struct DpmModel {
    std::atomic<bool> opened{false};
    std::atomic<int> dpmReqs{0};
    std::optional<Message> handle(uint32_t svc, const Message& req) {
        if (svc == kSvcDpm && req.msgId == dpm::kOpenPort) {
            dpmReqs++;
            auto* v = req.get(0x11);
            if (v && hex(*v) == "0104000000010000000400000005000000") {
                opened = true;
                return test::okResponse(req.msgId);
            }
            return test::errResponse(req.msgId, kErrInvalidArgument);
        }
        if (svc == kSvcWda && (req.msgId == wda::kSetDataFormat || req.msgId == wda::kGetDataFormat)) {
            if (!opened) return test::errResponse(req.msgId, kErrInvalidOperation);
            auto r = test::okResponse(req.msgId);
            r.u32(0x11, 2);
            r.u32(0x12, 5);
            r.u32(0x13, 5);
            r.u32(0x15, 10);
            r.u32(0x16, 8192);
            return r;
        }
        return modemHandler(svc, req);
    }
};

static radio::DataConfig dpmTestCfg() {
    radio::DataConfig cfg;
    cfg.requireNetdev = false;
    cfg.setDataFormat = true;
    cfg.parentIface = "a6l_no_such_if0";  // no sysfs endpoint ids -> 4/5 defaults
    return cfg;
}

static void testDpmBeforeWda() {
    {   // 1. phone behaviour reproduced: no DPM -> INVALID_OPERATION
        FakeModem modem;
        DpmModel model;
        modem.addService(kSvcWds);
        modem.addService(kSvcWda);
        modem.addService(kSvcDpm);
        modem.setHandler([&](uint32_t s, const Message& r) { return model.handle(s, r); });
        Client ctl(modem.transport(), "ctl");
        EXPECT(ctl.start({kSvcWda}));
        EXPECT(ctl.waitForServices({kSvcWda}, 1000).empty());
        auto cfg = dpmTestCfg();
        cfg.dpmOpenPort = false;
        radio::DataCallManager dm(cfg, [&](const char* tag) {
            return std::make_unique<Client>(modem.transport(), tag);
        }, &ctl);
        EXPECT(!dm.prepareDataPath());
        EXPECT(dm.formatReport().find("INVALID_OPERATION") != std::string::npos);
        EXPECT_EQ(model.dpmReqs.load(), 0);
        ctl.stop();
    }
    {   // 2. default: DPM open port first, then WDA succeeds, then the call comes up
        FakeModem modem;
        DpmModel model;
        modem.addService(kSvcWds);
        modem.addService(kSvcWda);
        modem.addService(kSvcDpm);
        modem.setHandler([&](uint32_t s, const Message& r) { return model.handle(s, r); });
        Client ctl(modem.transport(), "ctl");
        EXPECT(ctl.start({kSvcWda}));
        EXPECT(ctl.waitForServices({kSvcWda}, 1000).empty());
        radio::DataCallManager dm(dpmTestCfg(), [&](const char* tag) {
            return std::make_unique<Client>(modem.transport(), tag);
        }, &ctl);
        EXPECT(dm.prepareDataPath());
        EXPECT(dm.prepareDataPath());  // idempotent
        EXPECT_EQ(model.dpmReqs.load(), 1);
        auto rep = dm.formatReport();
        EXPECT(rep.find("dpm: open port hw ep=4/1 rx_ep(consumer)=4 tx_ep(producer)=5: ok") != std::string::npos);
        EXPECT(rep.find("wda: format set llp=2 ul=5 dl=5 dl_max=10/8192") != std::string::npos);
        // order on the wire: DPM open < WDA set < WDS bind mux
        int iDpm = -1, iWda = -1, iBind = -1, i = 0;
        radio::DataRequest rq;
        rq.apn = "orange";
        rq.protocol = radio::Protocol::V4;
        auto o = dm.setup(rq);
        EXPECT(o.ok);
        uint8_t muxSeen = 0;
        for (auto& [svc, m] : modem.requests()) {
            if (svc == kSvcDpm && m.msgId == dpm::kOpenPort && iDpm < 0) iDpm = i;
            if (svc == kSvcWda && m.msgId == wda::kSetDataFormat && iWda < 0) iWda = i;
            if (svc == kSvcWds && m.msgId == wds::kBindMuxDataPort && iBind < 0) {
                iBind = i;
                EXPECT_EQ(hex(*m.get(0x10)), std::string("0400000001000000"));
                muxSeen = (*m.get(0x11))[0];
            }
            i++;
        }
        EXPECT(iDpm >= 0 && iWda > iDpm && iBind > iWda);
        EXPECT_EQ(muxSeen, 1);
        // modem restart: DPM + WDA are sent again
        dm.modemReset();
        model.opened = false;
        EXPECT(dm.prepareDataPath());
        EXPECT_EQ(model.dpmReqs.load(), 2);
        dm.deactivateAll();
        ctl.stop();
    }
    {   // 3. no DPM service: reported, WDA still tried (and fails in this model)
        FakeModem modem;
        DpmModel model;
        modem.addService(kSvcWda);
        modem.setHandler([&](uint32_t s, const Message& r) { return model.handle(s, r); });
        Client ctl(modem.transport(), "ctl");
        EXPECT(ctl.start({kSvcWda}));
        EXPECT(ctl.waitForServices({kSvcWda}, 1000).empty());
        radio::DataCallManager dm(dpmTestCfg(), [&](const char* tag) {
            return std::make_unique<Client>(modem.transport(), tag);
        }, &ctl);
        EXPECT(!dm.prepareDataPath());
        auto rep = dm.formatReport();
        EXPECT(rep.find("dpm: service 0x2f not found") != std::string::npos);
        EXPECT(rep.find("(DPM port not open)") != std::string::npos);
        ctl.stop();
    }
}

// ---------------------------------------------------------------- ril3 (25 Sep 2026)
// Build an SMS-DELIVER TPDU carrying the UD of an SMS-SUBMIT (to round-trip the encoder).
static std::vector<uint8_t> submitToDeliver(const std::vector<uint8_t>& sub) {
    // sub: 00 | FO | MR | DA-len | DA-toa | DA digits | PID | DCS | UDL | UD
    size_t i = 1;
    uint8_t fo = sub[i++];
    i++;  // MR
    uint8_t n = sub[i++];
    i += 1 + (n + 1) / 2;
    uint8_t pid = sub[i++], dcs = sub[i++];
    std::vector<uint8_t> d = {static_cast<uint8_t>(0x04 | (fo & 0x40)), 0x0B, 0x91, 0x33, 0x66, 0x33, 0x36, 0x22, 0xF8,
                              pid, dcs, 0x62, 0x90, 0x52, 0x11, 0x00, 0x00, 0x80};
    d.insert(d.end(), sub.begin() + i, sub.end());  // UDL + UD
    return d;
}

static void testSmsEncoding() {
    // GSM7 single part: same bytes as the v1 ASCII encoder
    auto p = sms::buildSubmitParts("+33612345678", "hello");
    EXPECT(p.has_value());
    EXPECT(p->coding == sms::Coding::Gsm7);
    EXPECT_EQ(p->pdus.size(), 1u);
    EXPECT_EQ(hex(p->pdus[0]), std::string("0001000B913316325476F8000005E8329BFD06"));
    // French accents in the GSM7 alphabet (é è à ù) + extension table (€ [ ]) stay GSM7
    auto g = sms::utf8ToGsm7("\xc3\xa9t\xc3\xa9 \xe2\x82\xac[x]");
    EXPECT(g.has_value());
    EXPECT_EQ(hex(*g), std::string("05740520") + "1B65" + "1B3C" + "78" + "1B3E");
    // ç is not in GSM7 -> UCS2 (UTF-16BE), 70 units max in one part
    p = sms::buildSubmitParts("0612345678", "gar\xc3\xa7on \xf0\x9f\x98\x80");
    EXPECT(p.has_value());
    EXPECT(p->coding == sms::Coding::Ucs2);
    EXPECT_EQ(p->pdus.size(), 1u);
    EXPECT_EQ(p->units, 9u);  // 7 BMP chars + 1 surrogate pair (2 units)
    EXPECT_EQ(hex(p->pdus[0]), std::string("0001000A81602143658700081200670061007200E7006F006E0020D83DDE00"));
    auto rt = sms::parseDeliver(submitToDeliver(p->pdus[0]), sms::SmscForm::Bare);
    EXPECT(rt.has_value());
    EXPECT_EQ(rt->text, std::string("gar\xc3\xa7on \xf0\x9f\x98\x80"));
    // multipart GSM7: 200 chars -> 153 + 47, UDH 05 00 03 ref total seq, TP-UDHI set, SRR on request
    std::string longText;
    for (int i = 0; i < 200; i++) longText += static_cast<char>('a' + i % 26);
    sms::SubmitOptions so;
    so.concatRef = 0x42;
    so.statusReport = true;
    p = sms::buildSubmitParts("+33612345678", longText, so);
    EXPECT(p.has_value());
    EXPECT_EQ(p->pdus.size(), 2u);
    EXPECT_EQ(p->pdus[0][1], 0x61);  // SUBMIT | SRR | UDHI
    std::string joined;
    for (size_t k = 0; k < p->pdus.size(); k++) {
        auto d = sms::parseDeliver(submitToDeliver(p->pdus[k]), sms::SmscForm::Bare);
        EXPECT(d.has_value());
        if (!d) continue;
        EXPECT_EQ(d->concatRef, 0x42u);
        EXPECT_EQ(d->concatTotal, 2u);
        EXPECT_EQ(d->concatSeq, k + 1);
        joined += d->text;
    }
    EXPECT_EQ(joined, longText);
    // UDL of part 1 = 7 header septets + 153
    EXPECT_EQ(p->pdus[0][4 + 1 + 6 + 2], 160);
    // an escape pair is never split across parts: 152 'a' + '€' (septets 152/153) + 10 'b'
    std::string esc(152, 'a');
    esc += "\xe2\x82\xac" "bbbbbbbbbb";
    p = sms::buildSubmitParts("+33612345678", esc);
    EXPECT(p.has_value() && p->pdus.size() == 2);
    if (p && p->pdus.size() == 2) {
        auto d1 = sms::parseDeliver(submitToDeliver(p->pdus[0]), sms::SmscForm::Bare);
        auto d2 = sms::parseDeliver(submitToDeliver(p->pdus[1]), sms::SmscForm::Bare);
        EXPECT(d1 && d2 && d1->text + d2->text == esc);
        EXPECT(d2 && d2->text == "\xe2\x82\xac" "bbbbbbbbbb");
    }
    // multipart UCS2: 100 'ç' -> 67 + 33
    std::string u;
    for (int i = 0; i < 100; i++) u += "\xc3\xa7";
    p = sms::buildSubmitParts("+33612345678", u);
    EXPECT(p.has_value() && p->pdus.size() == 2 && p->coding == sms::Coding::Ucs2);
    if (p && p->pdus.size() == 2) {
        auto d1 = sms::parseDeliver(submitToDeliver(p->pdus[0]), sms::SmscForm::Bare);
        EXPECT(d1 && d1->concatTotal == 2 && d1->text.size() == 67 * 2);
    }
    // limits and bad input
    EXPECT(!sms::buildSubmitParts("+33x", "hi").has_value());
    EXPECT(!sms::buildSubmitParts("+33612345678", "").has_value());
    EXPECT(!sms::buildSubmitParts("+33612345678", "\xc3").has_value());  // truncated UTF-8
    sms::SubmitOptions two;
    two.maxParts = 2;
    EXPECT(!sms::buildSubmitParts("+33612345678", std::string(400, 'x'), two).has_value());
    // delivery report fields (status report TP-MR / TP-ST)
    auto sr = sms::parseDeliver(H("06050B913366336322F8629042418444806290424184548000"));
    EXPECT(sr && sr->statusReport && sr->srMessageRef == 5 && sr->srStatus == 0);
}

static void testWmsAck() {
    // ERR_84 on 25 Sep = QMI_ERR_ACK_NOT_SENT (0x54): named now
    EXPECT_EQ(errorName(84), std::string("ACK_NOT_SENT"));
    EXPECT_EQ(std::string(wms::ackFailureCauseName(1)), std::string("network-released-link"));
    EXPECT_EQ(wms::messageProtocolFor(wms::kFormatGwPp), 1);
    EXPECT_EQ(wms::messageProtocolFor(wms::kFormatCdma), 0);
    Message ev(MsgType::Indication, wms::kSetEventReport);
    ev.raw(0x11, {1, 1, 0, 0, 0, 6, 1, 0, 0x04}).u8(0x16, 1);
    auto e = wms::parseEventReport(ev);
    EXPECT(e.hasTransfer);
    EXPECT(!e.needsAck());  // ack indicator 1 = DO_NOT_SEND
    EXPECT(e.smsOnIms && *e.smsOnIms);
    Message st(MsgType::Indication, wms::kSetEventReport);
    st.raw(0x10, {1, 3, 0, 0, 0});
    EXPECT(!wms::parseEventReport(st).needsAck());  // stored message: the modem acked it
    // Send Ack over the fake modem: request layout + failure cause from TLV 0x10
    FakeModem modem;
    modem.addService(kSvcWms);
    modem.setHandler([](uint32_t, const Message& r) -> std::optional<Message> {
        if (r.msgId == wms::kSendAck) return test::errResponse(r.msgId, kErrAckNotSent).u8(0x10, 0);
        return test::okResponse(r.msgId);
    });
    Client c(modem.transport(), "ack");
    EXPECT(c.start({kSvcWms}));
    EXPECT(c.waitForServices({kSvcWms}, 1000).empty());
    wms::AckOptions ao;
    ao.smsOnIms = true;
    int cause = -2;
    auto r = wms::sendAck(c, 0x01020304, true, 0, 0, ao, &cause);
    EXPECT_EQ(r.qmiError, kErrAckNotSent);
    EXPECT_EQ(cause, 0);
    auto reqs = modem.requests();
    EXPECT(!reqs.empty());
    if (!reqs.empty()) {
        auto& m = reqs.back().second;
        EXPECT(m.get(0x01) && hex(*m.get(0x01)) == "040302010101");
        EXPECT(m.get(0x12) && hex(*m.get(0x12)) == "01");
        EXPECT(!m.get(0x11));
    }
    c.stop();
}

static void testMultisimViews() {
    // A6L 25 Sep: card 0 USIM (primary), card 1 present-error (empty slot) -- SDM845 capture shape
    auto m = fromQmux(H(kUimGetCardStatus));
    auto cs = uim::parseCardStatus(*m.get(0x10));
    EXPECT(cs.has_value());
    auto v0 = multisim::viewFor(*cs, 0);
    auto v1 = multisim::viewFor(*cs, 1);
    EXPECT(v0.provisioned && v0.card == 0 && v0.gwAppIndex == 0 && v0.present());
    EXPECT_EQ(v0.cardSession, multisim::kSessionCardSlot1);
    EXPECT(!v1.provisioned && v1.card == 1 && !v1.present() && v1.gwApp == nullptr);
    EXPECT_EQ(v1.cardSession, multisim::kSessionCardSlot2);
    EXPECT_EQ(v1.provSession, multisim::kSessionSecondaryGw);
    EXPECT(!multisim::provisioningCandidate(*cs, 1).has_value());
    // v1 single-SIM equivalence: slot 1 = primaryCard()/primaryGwApp()
    EXPECT(v0.cardPtr == cs->primaryCard());
    EXPECT(v0.gwApp == cs->primaryGwApp());
    // SIM only in physical slot 2, provisioned as primary: logical slot 1 -> card 1
    uim::CardStatus s2 = *cs;
    std::swap(s2.cards[0], s2.cards[1]);
    s2.indexGwPrimary = 0x0100;
    v0 = multisim::viewFor(s2, 0);
    v1 = multisim::viewFor(s2, 1);
    EXPECT(v0.card == 1 && v0.provisioned && v0.cardSession == multisim::kSessionCardSlot2);
    EXPECT(v1.card == 0 && !v1.provisioned);
    // two SIMs, second not provisioned yet -> candidate {uim slot 2, aid}
    uim::CardStatus s3 = *cs;
    s3.cards[1] = s3.cards[0];
    s3.cards[1].apps[0].state = uim::kAppStateDetected;
    auto cand = multisim::provisioningCandidate(s3, 1);
    EXPECT(cand.has_value() && cand->first == 2 && hex(cand->second) == "A0000000871002FF44FF128900000100");
    s3.indexGwSecondary = 0x0100;
    EXPECT(!multisim::provisioningCandidate(s3, 1).has_value());
    EXPECT(multisim::viewFor(s3, 1).provisioned);
    // messages (IDL layouts from stock libqmiservices.so)
    auto cp = multisim::buildChangeProvisioning(multisim::kSessionSecondaryGw, true, 2, {0xA0, 0x01});
    EXPECT_EQ(cp.msgId, 0x0038);
    EXPECT(hex(*cp.get(0x01)) == "0201" && hex(*cp.get(0x10)) == "0202A001");
    auto dds = multisim::buildSetDefaultDataSub(1);
    EXPECT(dds.msgId == 0x004B && hex(*dds.get(0x12)) == "01" && dds.tlvs.size() == 1);
    Message dsb(MsgType::Response, 0x005C);
    dsb.raw(0x10, {4}).raw(0x11, {0}).raw(0x12, {0}).raw(0x13, {1}).raw(0x14, {0}).u64(0x15, 3);
    auto p = multisim::parseDualStandbyPref(dsb);
    EXPECT(p.defaultDataSubs && *p.defaultDataSubs == 1 && p.activeSubsMask && *p.activeSubsMask == 3);
    // bindAll over the fake modem: sub 0 sends nothing, sub 1 sends the 4 binds
    FakeModem modem;
    for (auto s : {kSvcNas, kSvcWms, kSvcDms, kSvcVoice}) modem.addService(s);
    modem.setHandler([](uint32_t, const Message& r) -> std::optional<Message> { return test::okResponse(r.msgId); });
    Client c(modem.transport(), "bind");
    EXPECT(c.start({kSvcNas, kSvcWms, kSvcDms, kSvcVoice}));
    EXPECT(c.waitForServices({kSvcNas, kSvcWms, kSvcDms, kSvcVoice}, 1000).empty());
    EXPECT(multisim::bindAll(c, 0, true).ok());
    EXPECT(modem.requests().empty());
    std::string log;
    EXPECT(multisim::bindAll(c, 1, true, &log).ok());
    auto reqs = modem.requests();
    EXPECT_EQ(reqs.size(), 4u);
    for (auto& [svc, msg] : reqs) {
        if (svc == kSvcDms) EXPECT(msg.msgId == 0x0054 && hex(*msg.get(0x01)) == "02000000");
        if (svc == kSvcNas) EXPECT(msg.msgId == 0x0045 && hex(*msg.get(0x01)) == "01");
        if (svc == kSvcWms) EXPECT(msg.msgId == 0x004C && hex(*msg.get(0x01)) == "01");
        if (svc == kSvcVoice) EXPECT(msg.msgId == 0x0044 && hex(*msg.get(0x01)) == "01");
    }
    c.stop();
    // power vote (airplane mode)
    multisim::PowerVote pv;
    EXPECT(!pv.any());
    EXPECT(pv.vote(0, true));
    EXPECT(pv.vote(1, false));   // slot 2 off, slot 1 on: modem online
    EXPECT(!pv.vote(0, false));  // both off: low power
    EXPECT(pv.vote(1, true));
    pv.reset();
    EXPECT(pv.wants(0) && pv.wants(1) && !pv.any());
    EXPECT_EQ(multisim::slotCountFromConfig("dsds"), 2);
    EXPECT_EQ(multisim::slotCountFromConfig("ssss"), 1);
    EXPECT_EQ(multisim::slotCountFromConfig("dsds", 1), 1);
    EXPECT_EQ(multisim::slotCountFromConfig("", 2), 2);
}

static void testSsrClient() {
    // Modem restart as seen by one client: every service DEL_SERVER, requests fail fast with
    // NoService (no 5 s timeouts), then NEW_SERVER on new ports and requests work again.
    FakeModem modem;
    for (auto s : {kSvcDms, kSvcNas}) modem.addService(s);
    modem.setHandler([](uint32_t, const Message& r) -> std::optional<Message> {
        if (r.msgId == dms::kGetOperatingMode) return test::okResponse(r.msgId).u8(0x01, 0);
        return test::okResponse(r.msgId);
    });
    Client c(modem.transport(), "ssr");
    std::atomic<int> down{0}, up{0};
    c.onServiceChange([&](uint32_t, bool u) { (u ? up : down)++; });
    EXPECT(c.start({kSvcDms, kSvcNas}));
    EXPECT(c.waitForServices({kSvcDms, kSvcNas}, 1000).empty());
    uint8_t mode = 9;
    EXPECT(dms::getOperatingMode(c, &mode).ok() && mode == 0);
    for (int round = 0; round < 3; round++) {
        modem.removeService(kSvcDms);
        modem.removeService(kSvcNas);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto t0 = std::chrono::steady_clock::now();
        auto r = dms::getOperatingMode(c, &mode);
        EXPECT_EQ(r.status, Result::NoService);
        EXPECT(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
        modem.addService(kSvcDms);
        modem.addService(kSvcNas);
        EXPECT(c.waitForServices({kSvcDms, kSvcNas}, 1000).empty());
        mode = 9;
        EXPECT(dms::getOperatingMode(c, &mode).ok() && mode == 0);
    }
    EXPECT(down.load() >= 6);
    c.stop();
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
    testDpmWdaEncoding();
    testDpmBeforeWda();
    testSmsEncoding();
    testWmsAck();
    testMultisimViews();
    testSsrClient();
    printf("a6l-qmi tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
