// Full production ModemCore + QMI over an in-process fake modem.
#define main original_test_main
#include "modemcore_tests.cc"
#undef main
#include <cassert>
int main(){
 gLogLevel=0;android::base::SetProperty("ro.vendor.a6l.ril.slots","2");
 // Primary subscription provisioned from physical card 2; secondary from card 1.
 gS.cardTlv=cardStatusTlv(0x0100,0x0000,{{uim::kCardPresent,0,uim::kAppStateReady},{uim::kCardPresent,0,uim::kAppStateReady}});
 auto* modem=new FakeModem();
 modem->setHandler([](uint32_t svc,const Message& req)->std::optional<Message>{
  if(svc==kSvcUim && req.msgId==uim::kReadTransparent){
   auto session=req.get(0x01)->at(0);
   uint8_t digit=session==uim::kSessionCardSlot1 ? 0x11 : 0x22;
   return test::okResponse(req.msgId).raw(0x11,{2,0,digit,digit});
  }
  return handler(svc,req);
 });
 ModemCore::setTransportFactory([modem]{return modem->transport();});
 for(auto svc:kSvcs)modem->addService(svc);
 auto& first=ModemCore::get(1);auto& second=ModemCore::get(2);first.start();second.start();
 assert(waitFor([&]{return first.ready()&&second.ready();}));
 auto p=first.slotSim();auto s=second.slotSim();
 auto a=first.iccid(true),b=second.iccid(true);
 assert(p.card==1 && p.cardSession==uim::kSessionCardSlot1+1);
 assert(s.card==0 && a=="1111" && b=="1111");
 printf("F43 primary_physical_card=2 primary_ICCID=%s expected=2222 secondary_physical_card=1 secondary_ICCID=%s\n",a.c_str(),b.c_str());
 fflush(stdout);_Exit(0);
}
