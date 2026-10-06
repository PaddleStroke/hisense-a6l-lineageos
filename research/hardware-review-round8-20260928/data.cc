// Offline: production DataCallManager, client, encoders; fake modem and link operations.
#include <a6lqmi/datacall.h>
#include <a6lqmi/log.h>
#include "fake_modem.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
using namespace a6l::qmi;
using namespace a6l::radio;
using namespace a6l::test;
using namespace std::chrono_literals;
template<class F> bool waitFor(F f) {
    for(int i=0;i<200;i++){if(f())return true;std::this_thread::sleep_for(5ms);} return f();
}
struct Links : LinkOps {
    std::vector<std::string> added;
    bool exists(const std::string& n) override { return n=="rmnet_ipa0"; }
    int createLink(const std::string&,const std::string&,uint16_t,uint32_t) override {return 0;}
    int deleteLink(const std::string&) override {return 0;}
    int setUp(const std::string&,bool) override {return 0;}
    int setMtu(const std::string&,int) override {return 0;}
    int addAddress(const std::string&,const std::string& a) override {added.push_back(a);return 0;}
};
void run(bool earlyDisconnect) {
    FakeModem modem;
    std::atomic<int> settings{0}, observed{0}, lost{0};
    std::atomic<bool> changed{false};
    // This early observer is only a dispatch barrier. It never handles or repairs the event.
    modem.setHandler([&](uint32_t, const Message& req)->std::optional<Message>{
        auto r=okResponse(req.msgId);
        if(req.msgId==wds::kStartNetwork) r.u32(0x01,0x1234);
        if(req.msgId==wds::kGetCurrentSettings) {
            ++settings;
            if(earlyDisconnect && settings==1) {
                Message ind(MsgType::Indication,wds::kPacketServiceStatus);
                ind.raw(0x01,{wds::kConnDisconnected,0}).u8(0x12,wds::kFamilyV4);
                modem.indicate(kSvcWds,ind);
                assert(waitFor([&]{return observed.load()==1;}));
                // The in-flight settings reply still carries the old lease.
            }
            r.u32(0x1e,changed?0x0a000009:0x0a000005).u32(0x21,0xfffffffc);
            r.u32(0x15,changed?0x09090909:0x08080808).u32(0x29,changed?1280:1430);
        }
        return r;
    });
    modem.addService(kSvcWds);
    DataConfig cfg; cfg.setDataFormat=false; cfg.settingsTries=1;
    auto links=std::make_shared<Links>();
    DataCallManager dm(cfg,[&](const char* tag){
        auto c=std::make_unique<Client>(modem.transport(),tag);
        c->onIndication(kSvcWds,wds::kPacketServiceStatus,[&](const Message&){++observed;});
        return c;
    },nullptr);
    dm.setLinkOps(links);
    dm.onLost([&](int,uint64_t){++lost;});
    DataRequest rq; rq.protocol=Protocol::V4; rq.apn="offline-review";
    auto o=dm.setup(rq);
    assert(o.ok && o.call.addresses==std::vector<std::string>{"10.0.0.5/30"});
    if(earlyDisconnect) {
        std::this_thread::sleep_for(100ms);
        assert(observed==1 && lost==0 && dm.list().size()==1);
        puts("F60 disconnect_during_settings consumed=1 setup_success=1 cached_calls=1 loss_callbacks=0");
        // Positive control: once installed, the production listener does deliver a loss.
        Message ind(MsgType::Indication,wds::kPacketServiceStatus);
        ind.raw(0x01,{wds::kConnDisconnected,0}); modem.indicate(kSvcWds,ind);
        assert(waitFor([&]{return lost.load()==1;}));
        puts("F60 post_setup_disconnect_positive_control loss_callbacks=1");
    } else {
        changed=true;
        Message ind(MsgType::Indication,wds::kPacketServiceStatus);
        ind.raw(0x01,{wds::kConnConnected,1}).u8(0x12,wds::kFamilyV4);
        assert(wds::parsePacketStatus(ind).reconfig);
        modem.indicate(kSvcWds,ind);
        assert(waitFor([&]{return observed.load()==1;}));
        std::this_thread::sleep_for(300ms);
        auto calls=dm.list();
        assert(settings==1 && lost==0 && calls.size()==1);
        assert(calls[0].addresses==o.call.addresses && calls[0].dnses==o.call.dnses && calls[0].mtuV4==1430);
        assert(links->added==o.call.addresses);
        puts("F59 connected_reconfig=1 settings_queries_after_event=0 cached_IP=10.0.0.5/30 cached_DNS=8.8.8.8 cached_MTU=1430");
        // The fake modem really does publish the new settings, when asked.
        Client verify(modem.transport(),"verify"); assert(verify.start({kSvcWds}));
        assert(verify.waitForServices({kSvcWds},1000).empty());
        wds::Settings s; assert(wds::getCurrentSettings(verify,&s).ok());
        assert(s.ipv4==0x0a000009 && s.dns4a==0x09090909 && s.mtu==1280);
        puts("F59 fresh_query_positive_control IP=10.0.0.9 DNS=9.9.9.9 MTU=1280");
        verify.stop();
    }
    dm.deactivateAll();
}
int main(){gLogLevel=0;run(false);run(true);}
