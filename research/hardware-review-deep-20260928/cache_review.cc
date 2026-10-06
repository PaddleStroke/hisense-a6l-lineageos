#define main original_test_main
#include "modemcore_tests.cc"
#undef main
#include <cassert>
#include <future>
static std::atomic<bool> holdQuery{false}, queryEntered{false};
static std::promise<void> releaseQuery;
static auto queryGate=releaseQuery.get_future().share();
int main() {
    gLogLevel=0;android::base::SetProperty("ro.vendor.a6l.ril.slots","1");
    gS.cardTlv=cardStatusTlv(0,0xffff,{{uim::kCardPresent,0,uim::kAppStateReady}});
    auto* modem=new FakeModem();
    modem->setHandler([](uint32_t svc,const Message& req)->std::optional<Message>{
        if(svc==kSvcNas && req.msgId==nas::kGetServingSystem) {
            auto old=test::okResponse(req.msgId).raw(0x01,{1,1,1,2,1,nas::kRifLte}).u8(0x10,1); // home
            if(holdQuery){queryEntered=true;queryGate.wait();}
            return old;
        }
        return handler(svc,req);
    });
    ModemCore::setTransportFactory([modem]{return modem->transport();});
    for(auto s:kSvcs)modem->addService(s);
    auto& core=ModemCore::get(1);core.start();assert(waitFor([&]{return core.ready();}));
    holdQuery=true;
    std::thread reader([&]{core.serving(true);});assert(waitFor([]{return queryEntered.load();}));
    Message changed(MsgType::Indication,nas::kGetServingSystem);
    changed.raw(0x01,{1,1,1,2,1,nas::kRifLte}).u8(0x10,0); // roaming
    modem->indicate(kSvcNas,changed);
    assert(waitFor([&]{auto s=core.serving();return s && s->roaming && *s->roaming;}));
    bool newerRoam=*core.serving()->roaming;
    releaseQuery.set_value();reader.join();
    bool after=*core.serving()->roaming;
    printf("CACHE_ORDER new_indication_roaming=%d after_older_query_response=%d\n",newerRoam,after);
    assert(newerRoam && !after);
    fflush(stdout);_Exit(0);
}
