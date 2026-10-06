// Review-only: unchanged HAL methods included from a temporary source snapshot.
// Binder objects are small response-recording stubs; QMI uses the real client/builders
// over the project's in-process FakeModem. No phone, SIM, or network operations.
#include <a6lqmi/services.h>
#include <a6lqmi/multisim.h>
#include <a6lqmi/log.h>
#include <android-base/logging.h>
#include "fake_modem.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace qmi = a6l::qmi;
namespace uim = a6l::qmi::uim;
namespace nas = a6l::qmi::nas;
namespace multisim = a6l::qmi::multisim;
using a6l::test::FakeModem;
struct ScopedAStatus { static ScopedAStatus ok(){return {};} };
static auto ok=&ScopedAStatus::ok;
enum class RadioError { NONE, INVALID_ARGUMENTS, INVALID_SIM_STATE, SIM_ABSENT,
 PASSWORD_INCORRECT, REQUEST_NOT_SUPPORTED, GENERIC_FAILURE };
static int noError(int){return 0;}
static int errorResponse(int,RadioError e){return int(e);}
static RadioError qmiToRadioError(const qmi::Result&){return RadioError::GENERIC_FAILURE;}
static RadioError toRadioError(const qmi::Result& r){return qmiToRadioError(r);}
enum class AccessNetwork { GERAN, UTRAN, EUTRAN, UNKNOWN };
#include "raf.inc"
struct Response {
 int error=-1,value=-1;
 void setNumOfLiveModemsResponse(int e){error=e;}
 void getNumOfLiveModemsResponse(int e,int n){error=e;value=n;}
 void enableUiccApplicationsResponse(int e){error=e;}
 void areUiccApplicationsEnabledResponse(int e,bool n){error=e;value=n;}
 void supplyIccPinForAppResponse(int e,int n){error=e;value=n;}
 void supplyIccPukForAppResponse(int e,int n){error=e;value=n;}
 void changeIccPinForAppResponse(int e,int n){error=e;value=n;}
 void setFacilityLockForAppResponse(int e,int n){error=e;value=n;}
 void getFacilityLockForAppResponse(int e,int n){error=e;value=n;}
 void setAllowedNetworkTypesBitmapResponse(int e){error=e;}
 void getAllowedNetworkTypesBitmapResponse(int e,int n){error=e;value=n;}
 void setNetworkSelectionModeManualResponse(int e){error=e;}
};
struct ModemCore {
 enum class PinVerdict {Send,NoCard,NotProvisioned,AlreadyReady,Blocked,NotPinState,BadFormat,RepeatedWrong};
 struct PinCheck {PinVerdict verdict=PinVerdict::NoCard;int retries=-1;std::string why;};
 struct SlotSim {std::optional<uim::Card> cardInfo;int gwAppIndex=0;bool provisioned=true;
  std::vector<uint8_t> gwAid={0xa0};uint8_t cardSession=6,provSession=0,nonProvSession=4;};
 qmi::Client* client=nullptr;uim::CardStatus cs;
 static int slotCount(){return 2;}
 qmi::Client& ctl(){return *client;}
 SlotSim slotSim(bool=false){SlotSim s;s.cardInfo=cs.cards.at(0);return s;}
 static PinCheck checkPin(const uim::CardStatus&,int,const std::string&);
 PinCheck pinPreflight(const std::string& pin){return checkPin(cs,0,pin);}
 void notePinResult(const std::string&,bool){}
 void post(std::function<void()>){ /* deferred refresh irrelevant to recorded request */ }
 void cardStatus(bool){}
};
struct A6lRadioConfig {Response r;Response* respond(){return &r;}
 ScopedAStatus setNumOfLiveModems(int32_t,int8_t);ScopedAStatus getNumOfLiveModems(int32_t);};
struct RadioSim {Response r;bool mAreUiccApplicationsEnabled=true;Response* respond(){return &r;}
 ScopedAStatus enableUiccApplications(int32_t,bool);ScopedAStatus areUiccApplicationsEnabled(int32_t);};
struct A6lRadioSim {Response r;ModemCore c;Response* respond(){return &r;}ModemCore& slotCore(){return c;}
 uim::Session sessionFor(const std::string&);
 ScopedAStatus supplyIccPinForApp(int32_t,const std::string&,const std::string&);
 ScopedAStatus supplyIccPukForApp(int32_t,const std::string&,const std::string&,const std::string&);
 ScopedAStatus changeIccPinForApp(int32_t,const std::string&,const std::string&,const std::string&);
 ScopedAStatus setFacilityLockForApp(int32_t,const std::string&,bool,const std::string&,int32_t,const std::string&);
 ScopedAStatus getFacilityLockForApp(int32_t,const std::string&,const std::string&,int32_t,const std::string&);
};
struct A6lRadioNetwork {Response r;ModemCore c;int32_t mAllowedBitmap=0;
 Response* respond(){return &r;}ModemCore& slotCore(){return c;}
 ScopedAStatus setAllowedNetworkTypesBitmap(int32_t,int32_t);
 ScopedAStatus getAllowedNetworkTypesBitmap(int32_t);
 ScopedAStatus setNetworkSelectionModeManual(int32_t,const std::string&,AccessNetwork);
};
#define LOG_CALL LOG(VERBOSE)
#define LOG_CALL_IGNORED LOG(VERBOSE)
#include "methods.inc"
static qmi::Message last(FakeModem& m,uint16_t id){
 auto all=m.requests();for(auto i=all.rbegin();i!=all.rend();++i)if(i->second.msgId==id)return i->second;
 assert(false);return {};
}
int main(){
 qmi::gLogLevel=0;FakeModem modem;
 modem.setHandler([](uint32_t,const qmi::Message& m){return a6l::test::okResponse(m.msgId);});
 modem.addService(qmi::kSvcNas);modem.addService(qmi::kSvcUim);
 qmi::Client client(modem.transport(),"round5");
 assert(client.start({qmi::kSvcNas,qmi::kSvcUim}));
 assert(client.waitForServices({qmi::kSvcNas,qmi::kSvcUim},2000).empty());
 auto n=modem.requests().size();
 A6lRadioConfig cfg;cfg.setNumOfLiveModems(1,1);assert(cfg.r.error==0);
 cfg.getNumOfLiveModems(2);assert(cfg.r.value==2);
 RadioSim base;base.enableUiccApplications(3,false);base.areUiccApplicationsEnabled(4);
 assert(base.r.error==0 && base.r.value==0 && modem.requests().size()==n);
 printf("F42 requested_live_modems=1 success=1 readback=2; disable_UICC success=1 reported_enabled=0 QMI_requests=0\n");

 A6lRadioSim sim;sim.c.client=&client;
 uim::App app;app.type=uim::kAppUsim;app.state=uim::kAppStatePin;app.aid={0xa0};
 app.upinReplacesPin1=true;app.pin1State=uim::kPinDisabled;app.pin1Retries=0;
 uim::Card card;card.state=uim::kCardPresent;card.upinState=uim::kPinEnabledNotVerified;card.upinRetries=3;
 card.apps.push_back(app);sim.c.cs.indexGwPrimary=0;sim.c.cs.cards.push_back(card);
 auto guard=sim.c.pinPreflight("1234");assert(guard.verdict==ModemCore::PinVerdict::Send && guard.retries==3);
 sim.supplyIccPinForApp(5,"1234","a0");
 assert(last(modem,uim::kVerifyPin).get(0x02)->at(0)==uim::kPin1);
 sim.supplyIccPukForApp(6,"12345678","1234","a0");
 assert(last(modem,uim::kUnblockPin).get(0x02)->at(0)==uim::kPin1);
 sim.changeIccPinForApp(7,"1234","5678","a0");
 assert(last(modem,uim::kChangePin).get(0x02)->at(0)==uim::kPin1);
 sim.setFacilityLockForApp(8,"SC",true,"1234",0,"a0");
 assert(last(modem,uim::kSetPinProtection).get(0x02)->at(0)==uim::kPin1);
 printf("F44 upin_replaces_pin1=1 UPIN_attempts=3 guard=Send; verify/unblock/change/protect PIN_ID=1 expected=3\n");

 A6lRadioNetwork net;net.c.client=&client;
 for(int32_t bm : {0,static_cast<int32_t>(RadioAccessFamily::NR)}){
  net.setAllowedNetworkTypesBitmap(9,bm);assert(net.r.error==0);
  auto req=last(modem,nas::kSetSystemSelectionPreference);
  uint16_t mode=qmi::Reader(req.get(0x11)).u16();
  assert(mode==(nas::kModeGsm|nas::kModeUmts|nas::kModeLte));
  printf("F45 requested_bitmap=0x%x applied_GSM_UMTS_LTE=0x%x success=1\n",bm,mode);
 }
 net.getAllowedNetworkTypesBitmap(10);assert(net.r.value==static_cast<int32_t>(RadioAccessFamily::NR));
 printf("F45 readback_claims_NR_only=1\n");
 net.setNetworkSelectionModeManual(11,"00101",AccessNetwork::EUTRAN);
 auto a=last(modem,nas::kInitiateNetworkRegister);
 net.setNetworkSelectionModeManual(12,"001001",AccessNetwork::EUTRAN);
 auto b=last(modem,nas::kInitiateNetworkRegister);
 assert(a.tlvs==b.tlvs && !b.has(0x12));
 printf("F46 test_PLMNs=00101,001001 identical_QMI_TLVs=1 MNC_width_TLV_missing=1\n");

 n=modem.requests().size();sim.getFacilityLockForApp(13,"FD","",0,"a0");
 assert(sim.r.error==0 && sim.r.value==0 && modem.requests().size()==n);
 printf("F48 FDN_query success=1 returned_disabled=1 QMI_requests=0\n");
 client.stop();
}
