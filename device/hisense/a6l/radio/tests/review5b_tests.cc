// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio, r5 review pass2/deep fixes (28 Sep 2026, docs/hardware-review-pass2-20260928.md F14-F16,
// docs/hardware-review-deep-20260928.md F28), QMI/data layer over the fake modem:
//  F14 RTM_NEWADDR builder (byte-identical to iproute2 `ip addr add`, strace capture 28 Sep) and the data manager
//      installing every reported address on rmnet_data<N> before the link is reported up,
//  F15 transactional setup: bind mux rejection, settings failure/without address (bounded retry), no handle, link
//      failures -> accurate failure, WDS sessions stopped, link deleted; dual-stack partial success,
//  F16 fatal QRTR receive error: requests fail fast, services reported down, start() reopens,
//  F17 (data part) a leg's WDS service withdrawn -> loss callback,
//  F28 a delayed loss for a reused cid never removes the newer connection.
// Optional real-kernel check (network namespace + dummy link "d0"): A6L_NETNS_IF=d0 (see run-host-tests.sh).
#include "fake_modem.h"

#include <a6lqmi/client.h>
#include <a6lqmi/datacall.h>
#include <a6lqmi/log.h>
#include <a6lqmi/rmnet.h>
#include <a6lqmi/services.h>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace a6l;
using namespace a6l::qmi;
using a6l::test::FakeModem;
using a6l::test::errResponse;
using a6l::test::okResponse;
using namespace std::chrono_literals;

static int gFail = 0, gPass = 0;
#define EXPECT(c)                                                                            \
    do {                                                                                     \
        if (c) gPass++;                                                                      \
        else { gFail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); }      \
    } while (0)
#define EXPECT_EQ(a, b) EXPECT((a) == (b))

template <typename F>
static bool waitFor(F f, int ms = 3000) {
    for (int i = 0; i < ms / 10; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(10ms);
    }
    return f();
}

// ------------------------------------------------------------------ F14 builder
static void testNewAddrBuilder() {
    int fam = 0, prefix = 0;
    std::vector<uint8_t> a;
    EXPECT(rmnet::parseCidr("10.0.0.5/30", &fam, &a, &prefix));
    EXPECT(fam == AF_INET && prefix == 30 && a == (std::vector<uint8_t>{10, 0, 0, 5}));
    EXPECT(rmnet::parseCidr("2a01:cb8:1:2::1/64", &fam, &a, &prefix));
    EXPECT(fam == AF_INET6 && prefix == 64 && a.size() == 16 && a[0] == 0x2a && a[15] == 1);
    EXPECT(rmnet::parseCidr("10.1.2.3", &fam, &a, &prefix) && prefix == 32);
    EXPECT(!rmnet::parseCidr("10.1.2/24", &fam, &a, &prefix));
    EXPECT(!rmnet::parseCidr("10.1.2.3/33", &fam, &a, &prefix));
    EXPECT(!rmnet::parseCidr("10.1.2.3/", &fam, &a, &prefix));
    EXPECT(!rmnet::parseCidr("2001:db8::1/129", &fam, &a, &prefix));
    // iproute2 6.x `ip addr add 10.0.0.5/30 dev d0` (strace -xx, 28 Sep, ifindex 3, seq 0):
    // nlmsghdr{40, RTM_NEWADDR, REQUEST|ACK|EXCL|CREATE} ifaddrmsg{AF_INET,30,0,UNIVERSE,3} IFA_LOCAL IFA_ADDRESS
    const uint16_t iproute2Flags = NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE;
    std::vector<uint8_t> want4 = {0x28, 0, 0, 0, 0x14, 0, 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0x02, 0x1e, 0, 0, 3, 0, 0, 0,
                                  8, 0, 2, 0, 10, 0, 0, 5,
                                  8, 0, 1, 0, 10, 0, 0, 5};
    rmnet::parseCidr("10.0.0.5/30", &fam, &a, &prefix);
    EXPECT(rmnet::buildNewAddr(0, 3, fam, a, prefix, iproute2Flags) == want4);
    // `ip addr add 2a01:cb8:1:2::1/64 dev d0 nodad`: {64,...} ifaddrmsg{AF_INET6,64,IFA_F_NODAD,UNIVERSE,3}
    std::vector<uint8_t> v6 = {0x2a, 0x01, 0x0c, 0xb8, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1};
    std::vector<uint8_t> want6 = {0x40, 0, 0, 0, 0x14, 0, 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0, 0,
                                  0x0a, 0x40, 0x02, 0, 3, 0, 0, 0, 0x14, 0, 2, 0};
    want6.insert(want6.end(), v6.begin(), v6.end());
    want6.insert(want6.end(), {0x14, 0, 1, 0});
    want6.insert(want6.end(), v6.begin(), v6.end());
    rmnet::parseCidr("2a01:cb8:1:2::1/64", &fam, &a, &prefix);
    EXPECT(rmnet::buildNewAddr(0, 3, fam, a, prefix, iproute2Flags) == want6);
}

// Real kernel (optional): run inside `unshare -rn` with a dummy link.
static void testNetnsAddAddress() {
    const char* ifn = getenv("A6L_NETNS_IF");
    if (!ifn) return;
    EXPECT_EQ(rmnet::addAddress(ifn, "10.0.0.5/30"), 0);
    EXPECT_EQ(rmnet::addAddress(ifn, "10.0.0.5/30"), 0);  // NLM_F_REPLACE: idempotent
    EXPECT_EQ(rmnet::addAddress(ifn, "2a01:cb8:1:2::1/64"), 0);
    EXPECT(rmnet::addAddress("a6l_no_such_if0", "10.0.0.5/30") < 0);
    bool v4 = false, v6 = false;
    ifaddrs* ifa = nullptr;
    if (getifaddrs(&ifa) == 0) {
        for (auto* p = ifa; p; p = p->ifa_next) {
            if (!p->ifa_addr || strcmp(p->ifa_name, ifn)) continue;
            char b[64] = {};
            if (p->ifa_addr->sa_family == AF_INET) {
                inet_ntop(AF_INET, &reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr, b, sizeof b);
                auto* m = reinterpret_cast<sockaddr_in*>(p->ifa_netmask);
                if (!strcmp(b, "10.0.0.5") && m && ntohl(m->sin_addr.s_addr) == 0xFFFFFFFC) v4 = true;
            } else if (p->ifa_addr->sa_family == AF_INET6) {
                inet_ntop(AF_INET6, &reinterpret_cast<sockaddr_in6*>(p->ifa_addr)->sin6_addr, b, sizeof b);
                if (!strcmp(b, "2a01:cb8:1:2::1")) v6 = true;
            }
        }
        freeifaddrs(ifa);
    }
    EXPECT(v4);
    EXPECT(v6);
    printf("a6l-review5b netns: kernel accepted the RTM_NEWADDR (v4=%d v6=%d)\n", v4, v6);
}

// ------------------------------------------------------------------ fake link layer + modem
struct FakeLink : radio::LinkOps {
    std::mutex m;
    std::vector<std::string> ops;
    std::string failOn;  // op prefix that fails, e.g. "up rmnet_data0", "addr 2a01"
    bool existing = false;
    int run(const std::string& op) {
        std::lock_guard<std::mutex> g(m);
        ops.push_back(op);
        return !failOn.empty() && op.rfind(failOn, 0) == 0 ? -1 : 0;
    }
    bool exists(const std::string& n) override { return n == "rmnet_ipa0" || existing; }
    int createLink(const std::string&, const std::string& n, uint16_t, uint32_t) override { return run("create " + n); }
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
    void clear() {
        std::lock_guard<std::mutex> g(m);
        ops.clear();
    }
};

struct Faults {
    std::atomic<bool> bindMux{false}, settingsErr{false}, noHandle{false}, v6Start{false};
    std::atomic<int> noAddrTimes{0};  // settings answers without an address
};
static Faults gF;

static std::optional<Message> wdsHandler(uint32_t svc, const Message& req) {
    if (svc != kSvcWds) return okResponse(req.msgId);
    if (req.msgId == wds::kBindMuxDataPort && gF.bindMux) return errResponse(req.msgId, kErrInvalidOperation);
    if (req.msgId == wds::kStartNetwork) {
        auto* fam = req.get(0x19);
        bool v6 = fam && (*fam)[0] == 6;
        if (v6 && gF.v6Start) return errResponse(req.msgId, kErrCallFailed);
        auto r = okResponse(req.msgId);
        if (!gF.noHandle) r.u32(0x01, v6 ? 0x2222 : 0x1111);
        return r;
    }
    if (req.msgId == wds::kGetCurrentSettings) {
        if (gF.settingsErr) return errResponse(req.msgId, kErrInternal);
        auto r = okResponse(req.msgId);
        r.u32(0x15, 0x0A0B0C0D);
        if (gF.noAddrTimes > 0) {
            gF.noAddrTimes--;
            return r;
        }
        r.u32(0x1E, 0x0A000005);
        r.u32(0x20, 0x0A000006);
        r.u32(0x21, 0xFFFFFFFC);
        r.u32(0x29, 1430);
        std::vector<uint8_t> a6{0x2a, 0x01, 0x0c, 0xb8, 0, 1, 0, 2, 0, 0, 0, 0, 0, 0, 0, 1, 64};
        r.raw(0x25, a6);
        return r;
    }
    return okResponse(req.msgId);
}

static int count(FakeModem& m, uint16_t id) {
    int n = 0;
    for (auto& [s, msg] : m.requests())
        if (s == kSvcWds && msg.msgId == id) n++;
    return n;
}

struct Rig {
    FakeModem modem;
    std::shared_ptr<FakeLink> link = std::make_shared<FakeLink>();
    std::unique_ptr<radio::DataCallManager> dm;
    std::mutex lm;
    std::vector<std::pair<int, uint64_t>> lost;
    Rig() {
        modem.addService(kSvcWds);
        modem.setHandler(wdsHandler);
        radio::DataConfig cfg;
        cfg.setDataFormat = false;
        cfg.settingsRetryMs = 20;
        dm = std::make_unique<radio::DataCallManager>(
                cfg, [this](const char* tag) { return std::make_unique<Client>(modem.transport(), tag); }, nullptr);
        dm->setLinkOps(link);
        dm->onLost([this](int cid, uint64_t gen) {
            std::lock_guard<std::mutex> g(lm);
            lost.push_back({cid, gen});
        });
    }
    size_t lostCount() {
        std::lock_guard<std::mutex> g(lm);
        return lost.size();
    }
};

static radio::DataRequest req(radio::Protocol p = radio::Protocol::V4V6) {
    radio::DataRequest r;
    r.apn = "free";
    r.protocol = p;
    return r;
}

static void resetFaults() {
    gF.bindMux = gF.settingsErr = gF.noHandle = gF.v6Start = false;
    gF.noAddrTimes = 0;
}

// ------------------------------------------------------------------ F14 + control
static void testSetupInstallsAddresses() {
    resetFaults();
    Rig r;
    auto o = r.dm->setup(req());
    EXPECT(o.ok);
    EXPECT(o.call.v4 && o.call.v6);
    // create -> parent up -> MTU -> every reported address -> link up (addresses before Android sees the call)
    EXPECT_EQ(r.link->joined(), std::string("create rmnet_data0,up rmnet_ipa0,mtu rmnet_data0 1430,"
                                            "addr 10.0.0.5/30 rmnet_data0,addr 2a01:cb8:1:2::1/64 rmnet_data0,"
                                            "up rmnet_data0"));
    EXPECT_EQ(o.call.addresses.size(), 2u);
    EXPECT(o.call.generation > 0);
    r.link->clear();
    EXPECT(r.dm->deactivate(o.call.cid));
    EXPECT_EQ(r.link->joined(), std::string("delete rmnet_data0"));
    EXPECT_EQ(count(r.modem, wds::kStopNetwork), 2);
    // a stale netdev with our name (HAL restart) is deleted before the link is created again
    r.link->existing = true;
    r.link->clear();
    auto o2 = r.dm->setup(req(radio::Protocol::V4));
    EXPECT(o2.ok);
    EXPECT(r.link->joined().rfind("delete rmnet_data0,create rmnet_data0", 0) == 0);
}

// ------------------------------------------------------------------ F15
static void testSetupFailures() {
    {   // settings failure (was: ok=1 with zero addresses): the started session is stopped, no link
        resetFaults();
        gF.settingsErr = true;
        Rig r;
        auto o = r.dm->setup(req(radio::Protocol::V4));
        EXPECT(!o.ok);
        EXPECT(o.detail.find("settings:qmi-error") != std::string::npos);
        EXPECT_EQ(count(r.modem, wds::kGetCurrentSettings), 3);  // bounded retry
        EXPECT_EQ(count(r.modem, wds::kStopNetwork), 1);
        EXPECT(r.link->joined().empty());
        EXPECT(r.dm->list().empty());
        gF.settingsErr = false;  // fault cleared: data comes up
        EXPECT(r.dm->setup(req(radio::Protocol::V4)).ok);
    }
    {   // settings without an address at first: bounded retry succeeds
        resetFaults();
        gF.noAddrTimes = 2;
        Rig r;
        auto o = r.dm->setup(req(radio::Protocol::V4));
        EXPECT(o.ok);
        EXPECT_EQ(count(r.modem, wds::kGetCurrentSettings), 3);
        EXPECT_EQ(o.call.addresses.size(), 1u);
    }
    {   // mux bind rejected (was: START still sent, ok=1): no START
        resetFaults();
        gF.bindMux = true;
        Rig r;
        auto o = r.dm->setup(req());
        EXPECT(!o.ok);
        EXPECT(o.detail.find("bind-mux") != std::string::npos);
        EXPECT_EQ(count(r.modem, wds::kStartNetwork), 0);
        EXPECT(r.dm->list().empty());
    }
    {   // START without a packet data handle
        resetFaults();
        gF.noHandle = true;
        Rig r;
        auto o = r.dm->setup(req(radio::Protocol::V4));
        EXPECT(!o.ok);
        EXPECT(o.detail.find("no-handle") != std::string::npos);
    }
    {   // link up failure (was: ok=1): both sessions stopped, link deleted
        resetFaults();
        Rig r;
        r.link->failOn = "up rmnet_data0";
        auto o = r.dm->setup(req());
        EXPECT(!o.ok);
        EXPECT_EQ(o.failCause, 0x1001);
        EXPECT(o.detail.find("link-up-failed") != std::string::npos);
        EXPECT_EQ(count(r.modem, wds::kStopNetwork), 2);
        EXPECT(r.link->joined().find("delete rmnet_data0") != std::string::npos);
        EXPECT(r.dm->list().empty());
        r.link->failOn.clear();
        EXPECT(r.dm->setup(req()).ok);
    }
    {   // parent up failure and IPv4 address failure on a v4-only call
        resetFaults();
        Rig r;
        r.link->failOn = "up rmnet_ipa0";
        EXPECT(!r.dm->setup(req(radio::Protocol::V4)).ok);
        r.link->failOn = "addr 10.";
        auto o = r.dm->setup(req(radio::Protocol::V4));
        EXPECT(!o.ok);
        EXPECT(o.detail.find("v4:address-failed") != std::string::npos);
        EXPECT(r.dm->list().empty());
    }
    {   // dual stack: IPv6 address cannot be installed -> the call continues IPv4-only, v6 session stopped
        resetFaults();
        Rig r;
        r.link->failOn = "addr 2a01";
        auto o = r.dm->setup(req());
        EXPECT(o.ok);
        EXPECT(o.call.v4 && !o.call.v6);
        EXPECT_EQ(o.call.addresses.size(), 1u);
        bool v6left = false;
        for (auto* v : {&o.call.addresses, &o.call.gateways, &o.call.dnses})
            for (auto& x : *v)
                if (x.find(':') != std::string::npos) v6left = true;
        EXPECT(!v6left);
        EXPECT_EQ(o.call.mtuV6, 0);
        EXPECT_EQ(count(r.modem, wds::kStopNetwork), 1);
    }
    {   // dual stack: v6 START rejected -> IPv4-only call (legitimate single-family fallback)
        resetFaults();
        gF.v6Start = true;
        Rig r;
        auto o = r.dm->setup(req());
        EXPECT(o.ok);
        EXPECT(o.call.v4 && !o.call.v6);
    }
    resetFaults();
}

// ------------------------------------------------------------------ F28 + F17 (data)
static void testLossGeneration() {
    resetFaults();
    Rig r;
    auto a = r.dm->setup(req(radio::Protocol::V4));
    EXPECT(a.ok);
    Message lost(MsgType::Indication, wds::kPacketServiceStatus);
    lost.raw(0x01, {wds::kConnDisconnected, 0});
    r.modem.indicate(kSvcWds, lost);
    EXPECT(waitFor([&] { return r.lostCount() >= 1; }));
    std::pair<int, uint64_t> old;
    {
        std::lock_guard<std::mutex> g(r.lm);
        old = r.lost[0];
    }
    EXPECT_EQ(old.first, a.call.cid);
    EXPECT_EQ(old.second, a.call.generation);
    // Android tears A down and sets up B before the queued loss runs: B reuses the cid
    EXPECT(r.dm->deactivate(a.call.cid));
    auto b = r.dm->setup(req(radio::Protocol::V4));
    EXPECT(b.ok && b.call.cid == old.first && b.call.generation != old.second);
    EXPECT(!r.dm->deactivateIfCurrent(old.first, old.second));  // the old loss: ignored
    EXPECT_EQ(r.dm->list().size(), 1u);
    EXPECT(r.dm->deactivateIfCurrent(b.call.cid, b.call.generation));  // B's own loss still works
    EXPECT(r.dm->list().empty());

    // F17 (data): WDS withdrawn alone -> every leg of the call reports its loss with the call's generation
    auto c = r.dm->setup(req());
    EXPECT(c.ok);
    size_t before = r.lostCount();
    r.modem.removeService(kSvcWds);
    EXPECT(waitFor([&] { return r.lostCount() >= before + 2; }));
    {
        std::lock_guard<std::mutex> g(r.lm);
        EXPECT(r.lost.back().first == c.call.cid && r.lost.back().second == c.call.generation);
    }
    EXPECT(r.dm->deactivateIfCurrent(c.call.cid, c.call.generation));
    EXPECT(!r.dm->deactivateIfCurrent(c.call.cid, c.call.generation));  // second leg's report: no-op
}

// ------------------------------------------------------------------ F16
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

static void testFatalRecv() {
    FakeModem modem;
    modem.addService(kSvcDms);
    std::atomic<bool> hold{false};
    modem.setHandler([&](uint32_t, const Message& r) -> std::optional<Message> {
        if (hold) return std::nullopt;  // outstanding request when the transport dies
        if (r.msgId == dms::kGetOperatingMode) return okResponse(r.msgId).u8(0x01, 0);
        return okResponse(r.msgId);
    });
    auto t = std::make_unique<FaultTransport>(modem.transport());
    auto* ft = t.get();
    Client c(std::move(t), "fatal");
    std::atomic<int> down{0}, up{0};
    c.onServiceChange([&](uint32_t, bool u) { (u ? up : down)++; });
    EXPECT(c.start({kSvcDms}));
    EXPECT(c.waitForServices({kSvcDms}, 1000).empty());
    // a request outstanding while the reader hits the fatal error fails promptly (not after its 5 s timeout)
    hold = true;
    Result pending;
    std::thread th([&] { pending = c.request(kSvcDms, Message::request(dms::kGetOperatingMode), 5000); });
    std::this_thread::sleep_for(100ms);
    auto t0 = std::chrono::steady_clock::now();
    ft->fail = true;
    th.join();
    EXPECT(std::chrono::steady_clock::now() - t0 < 1s);
    EXPECT_EQ(pending.status, Result::TransportError);
    EXPECT(c.failed());
    EXPECT(!c.hasService(kSvcDms));
    EXPECT(waitFor([&] { return down.load() == 1; }));
    auto r = c.request(kSvcDms, Message::request(dms::kGetOperatingMode), 5000);
    EXPECT_EQ(r.status, Result::TransportError);  // fails fast
    EXPECT(c.waitForServices({kSvcDms}, 2000).size() == 1u);  // returns early, no 2 s wait needed
    // recovery: start() reopens (one new transport open), services and requests come back
    hold = false;
    EXPECT(c.start({kSvcDms}));
    EXPECT_EQ(ft->opens.load(), 2);
    EXPECT(!c.failed());
    EXPECT(c.waitForServices({kSvcDms}, 1000).empty());
    uint8_t mode = 9;
    EXPECT(dms::getOperatingMode(c, &mode).ok() && mode == 0);
    EXPECT(c.start({kSvcDms}));  // healthy: no reopen
    EXPECT_EQ(ft->opens.load(), 2);
    // repeated failure / restart, then plain stop
    ft->fail = true;
    EXPECT(waitFor([&] { return c.failed(); }));
    EXPECT(c.start({kSvcDms}));
    EXPECT_EQ(ft->opens.load(), 3);
    EXPECT(c.waitForServices({kSvcDms}, 1000).empty());
    c.stop();
    c.stop();
}

int main(int argc, char** argv) {
    gLogLevel = (argc > 1 && !strcmp(argv[1], "-v")) ? 4 : 0;
    testNewAddrBuilder();
    testNetnsAddAddress();
    testSetupInstallsAddresses();
    testSetupFailures();
    testLossGeneration();
    testFatalRecv();
    printf("a6l-review5b tests: %d passed, %d failed\n", gPass, gFail);
    return gFail ? 1 : 0;
}
