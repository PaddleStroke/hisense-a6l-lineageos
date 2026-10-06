#include <a6lqmi/datacall.h>
#include <a6lqmi/services.h>
#include "fake_modem.h"
#include <cassert>
#include <cstdio>
#include <mutex>
#include <thread>
using namespace a6l; using namespace a6l::qmi;
int main() {
    test::FakeModem modem;modem.addService(kSvcWds);
    modem.setHandler([](uint32_t,const Message& req)->std::optional<Message>{
        auto r=test::okResponse(req.msgId);
        if(req.msgId==wds::kStartNetwork)r.u32(1,0x1111);
        if(req.msgId==wds::kGetCurrentSettings)r.u32(0x1e,0x0a000005).u32(0x21,0xfffffffc);
        return r;
    });
    radio::DataConfig cfg;cfg.requireNetdev=false;cfg.setDataFormat=false;
    radio::DataCallManager manager(cfg,[&](const char* tag){return std::make_unique<Client>(modem.transport(),tag);},nullptr);
    std::mutex mu;std::vector<int> deferred;
    manager.onLost([&](int cid){std::lock_guard<std::mutex> g(mu);deferred.push_back(cid);});
    radio::DataRequest r;r.apn="offline.test";r.protocol=radio::Protocol::V4;
    auto a=manager.setup(r);assert(a.ok);
    Message lost(MsgType::Indication,wds::kPacketServiceStatus);lost.raw(1,{wds::kConnDisconnected,0});
    modem.indicate(kSvcWds,lost);
    int oldCid=-1;
    for(int i=0;i<200;i++){{std::lock_guard<std::mutex> g(mu);if(!deferred.empty()){oldCid=deferred[0];break;}}std::this_thread::sleep_for(std::chrono::milliseconds(5));}
    assert(oldCid==a.call.cid);assert(manager.deactivate(a.call.cid));
    auto b=manager.setup(r);assert(b.ok && b.call.cid==oldCid);
    // Same operation used by A6lRadioData::onDataCallLost after its queued callback runs.
    assert(manager.deactivate(oldCid));
    printf("DATA_STALE_LOSS old_cid=%d new_cid=%d remaining_after_old_callback=%zu\n",oldCid,b.call.cid,manager.list().size());
    assert(manager.list().empty());
}
