// Actual ModemCore with existing fake-modem helpers; never connects to a phone.
#define main original_test_main
#include "modemcore_tests.cc"
#undef main
#include <cassert>
#include <future>
static FakeModem* modem;
static std::atomic<int> delivered{0}, dispatched{0}, deletedBeforeDelivery{0}, ackedBeforeTransferDelivery{0};
struct ReviewListener : ModemCore::Listener {
    void onNewSms(const std::vector<uint8_t>&,bool) override {
        int n=delivered.fetch_add(1);
        if(n==0) {
            deletedBeforeDelivery=count(*modem,kSvcWms,wms::kDelete);
            assert(ModemCore::get(1).ackLastSms(false,0x16,0xD3));
            ackedBeforeTransferDelivery=count(*modem,kSvcWms,wms::kSendAck);
        }
    }
};
static Message transfer(uint8_t format,uint32_t txn) {
    Message m(MsgType::Indication,wms::kSetEventReport);
    std::vector<uint8_t> data={0,uint8_t(txn),uint8_t(txn>>8),uint8_t(txn>>16),uint8_t(txn>>24),format,2,0,4,0};
    m.raw(0x11,data); return m;
}
int main() {
    gLogLevel=0; android::base::SetProperty("ro.vendor.a6l.ril.slots","1");
    gS.cardTlv=cardStatusTlv(0,0xffff,{{uim::kCardPresent,0,uim::kAppStateReady}});
    modem=new FakeModem();
    modem->setHandler([](uint32_t svc,const Message& req)->std::optional<Message>{
        if(svc==kSvcWms && req.msgId==wms::kRawRead)
            return test::okResponse(req.msgId).raw(0x01,{0,wms::kFormatGwPp,2,0,4,0});
        return handler(svc,req);
    });
    ModemCore::setTransportFactory([]{return modem->transport();});
    for(auto s:kSvcs)modem->addService(s);
    auto& core=ModemCore::get(1); core.start(); assert(waitFor([&]{return core.ready();}));
    core.ctl().onIndication(kSvcWms,wms::kSetEventReport,[](const Message&){++dispatched;});
    // Broadcast payload follows a separate route; no listener/API exists to deliver it.
    modem->indicate(kSvcWms,transfer(wms::kFormatGwBc,11));
    assert(waitFor([]{return dispatched==1;}));
    auto barrier=std::make_shared<std::promise<void>>(); auto b=barrier->get_future();
    core.post([barrier]{barrier->set_value();}); assert(b.wait_for(std::chrono::seconds(2))==std::future_status::ready);
    int cbNacks=count(*modem,kSvcWms,wms::kSendAck);
    printf("CELL_BROADCAST ignored_log=%d negative_acks=%d\n",hoststub::logContains("ignoring MT SMS in format 7"),cbNacks);
    assert(hoststub::logContains("ignoring MT SMS in format 7"));

    // Block the worker while the dispatcher receives stored A followed by transfer B.
    auto gate=std::make_shared<std::promise<void>>(); auto gf=gate->get_future().share();
    std::atomic<bool> blocked{false};
    core.post([&blocked,gf]{blocked=true;gf.wait();}); assert(waitFor([&]{return blocked.load();}));
    core.addListener(new ReviewListener());
    Message stored(MsgType::Indication,wms::kSetEventReport); stored.raw(0x10,{1,7,0,0,0});
    modem->indicate(kSvcWms,stored); modem->indicate(kSvcWms,transfer(wms::kFormatGwPp,0x2222));
    assert(waitFor([]{return dispatched==3;})); gate->set_value();
    assert(waitFor([]{return delivered==2;}));
    auto a=last(*modem,kSvcWms,wms::kSendAck); assert(a && a->get(1));
    auto bytes=*a->get(1); uint32_t txn=bytes[0]|bytes[1]<<8|bytes[2]<<16|bytes[3]<<24;
    printf("STORED_SMS deletes_before_delivery=%d first_receipt_rejected=1\n",deletedBeforeDelivery.load());
    printf("SMS_ACK_ORDER first_delivered=stored_A ack_target=0x%x transfer_B_not_yet_delivered=1 ack_delta=%d\n",txn,ackedBeforeTransferDelivery.load()-cbNacks);
    assert(deletedBeforeDelivery==1 && ackedBeforeTransferDelivery==cbNacks+1 && txn==0x2222);
    fflush(stdout); _Exit(0); // Existing ModemCore fixture has process-lifetime threads.
}
