// Offline only: real QMI/data manager, fake modem and fake link operations.
#include <a6lqmi/client.h>
#include <a6lqmi/datacall.h>
#include <a6lqmi/rmnet.h>
#include <a6lqmi/log.h>
#include "fake_modem.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace a6l;
using namespace a6l::qmi;
using namespace std::chrono_literals;
static int linkCalls, upCalls, mtuCalls;
static bool failUp;
namespace a6l::rmnet {
bool exists(const std::string&) { return true; }
int createLink(const std::string&, const std::string&, uint16_t, uint32_t) { ++linkCalls; return 0; }
int deleteLink(const std::string&) { return 0; }
int setUp(const std::string&, bool) { ++upCalls; return failUp ? -1 : 0; }
int setMtu(const std::string&, int) { ++mtuCalls; return 0; }
}
struct FaultTransport : Transport {
    std::unique_ptr<Transport> inner;
    std::atomic<bool> fail{false}, failed{false};
    int opens = 0;
    explicit FaultTransport(std::unique_ptr<Transport> t) : inner(std::move(t)) {}
    bool open() override { ++opens; return inner->open(); }
    void close() override { inner->close(); }
    uint32_t localNode() const override { return inner->localNode(); }
    uint32_t localPort() const override { return inner->localPort(); }
    bool send(const Addr& a, const std::vector<uint8_t>& b) override { return inner->send(a,b); }
    int recv(Addr* a, std::vector<uint8_t>* b, int ms) override {
        if (fail) { failed = true; return -1; }
        return inner->recv(a,b,ms);
    }
};
int main() {
    gLogLevel = 0;
    {
        test::FakeModem modem;
        modem.addService(kSvcDms);
        modem.setHandler([](uint32_t, const Message& r) { return test::okResponse(r.msgId); });
        auto transport = std::make_unique<FaultTransport>(modem.transport());
        auto* fault = transport.get();
        Client client(std::move(transport),"fatal-read-review");
        std::atomic<int> down{0};
        client.onServiceChange([&](uint32_t, bool up) { if (!up) ++down; });
        assert(client.start({kSvcDms}));
        assert(client.waitForServices({kSvcDms},1000).empty());
        fault->fail = true;
        for (int i=0; i<100 && !fault->failed; ++i) std::this_thread::sleep_for(10ms);
        assert(fault->failed);
        std::this_thread::sleep_for(30ms);
        auto r = client.request(kSvcDms,Message::request(dms::kGetOperatingMode),80);
        bool startAgain = client.start({kSvcDms});
        printf("FATAL_QRTR_READ cached_service=%d down_events=%d request_timeout=%d start_again=%d opens=%d\n",
               client.hasService(kSvcDms),down.load(),r.status==Result::Timeout,startAgain,fault->opens);
        assert(client.hasService(kSvcDms) && down==0 && r.status==Result::Timeout && startAgain && fault->opens==1);
        client.stop();
    }
    for (int mode=0; mode<4; ++mode) {
        test::FakeModem modem;
        modem.addService(kSvcWds);
        modem.setHandler([mode](uint32_t, const Message& req) -> std::optional<Message> {
            if (mode==1 && req.msgId==wds::kGetCurrentSettings)
                return test::errResponse(req.msgId,kErrInternal);
            if (mode==2 && req.msgId==wds::kBindMuxDataPort)
                return test::errResponse(req.msgId,kErrInternal);
            auto r=test::okResponse(req.msgId);
            if (req.msgId==wds::kStartNetwork) r.u32(0x01,0x1111);
            if (req.msgId==wds::kGetCurrentSettings) {
                r.u32(0x1e,0x0a000005).u32(0x20,0x0a000006).u32(0x21,0xfffffffc).u32(0x29,1430);
            }
            return r;
        });
        radio::DataConfig cfg;
        cfg.requireNetdev=true; cfg.setDataFormat=false;
        radio::DataCallManager manager(cfg,[&](const char* tag) {
            return std::make_unique<Client>(modem.transport(),tag);
        },nullptr);
        radio::DataRequest req;
        req.apn="offline.test"; req.protocol=radio::Protocol::V4;
        linkCalls=upCalls=mtuCalls=0; failUp=mode==3;
        auto result=manager.setup(req);
        int starts=0;
        for(auto& [s,m]:modem.requests()) if(m.msgId==wds::kStartNetwork) ++starts;
        printf("DATA mode=%d ok=%d addresses=%zu create=%d up=%d mtu=%d start_requests=%d\n",
               mode,result.ok,result.call.addresses.size(),linkCalls,upCalls,mtuCalls,starts);
        assert(result.ok && starts==1 && linkCalls==1 && upCalls==2);
        assert((mode==1) == result.call.addresses.empty());
    }
    puts("REPRODUCED QMI fatal-read stall and data false-success cases; normal data path traced.");
}
