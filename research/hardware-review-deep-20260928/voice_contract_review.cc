// Compile the two unmodified method bodies extracted by reproduce.py.
// Binder/QMI dependencies are stubs; qv::dial only records requests, never sends them.
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <cstdio>
#include <cassert>
struct ScopedAStatus {static ScopedAStatus ok(){return {};}};
static auto ok=&ScopedAStatus::ok;
struct Log {template<class T> Log& operator<<(const T&){return *this;}};
#define LOG(level) Log{}
namespace aidlVoice {
struct Dial {std::string address;int clir=0;};
enum class EmergencyCallRouting {UNKNOWN,NORMAL,EMERGENCY};
}
static std::string toString(aidlVoice::EmergencyCallRouting){return "route";}
enum class RadioError {RADIO_NOT_AVAILABLE};
static int errorResponse(int,RadioError){return 1;}
static int noError(int){return 0;}
struct Result {bool success=true;bool ok()const{return success;}std::string describe()const{return "fake";}};
static RadioError toRadioError(Result){return RadioError::RADIO_NOT_AVAILABLE;}
namespace qmi::nas {static std::string radioIfList(int){return "fake";}}
struct Call {std::string number;bool emergency;};
static std::vector<Call> requests;
static bool rejectFirst;
namespace qv {
static Result dial(int,const std::string& n,bool emergency,uint8_t*) {
    requests.push_back({n,emergency}); return {!(rejectFirst && requests.size()==1)};
}}
struct Core {
    bool bound(){return true;}int ctl(){return 0;}int sub(){return 0;}
    std::optional<int> serving(){return 1;}
};
struct Response {void dialResponse(int){}void emergencyDialResponse(int){}};
struct Executor {template<class F>void post(F f){f();}};
struct A6lRadioVoice {
    Executor mExec;Core c;Response r;Core& slotCore(){return c;}Response* respond(){return &r;}
    ScopedAStatus dial(int32_t,const aidlVoice::Dial&);
    ScopedAStatus emergencyDial(int32_t,const aidlVoice::Dial&,int32_t,const std::vector<std::string>&,
      aidlVoice::EmergencyCallRouting,bool,bool);
};
#include "voice_methods.inc"
int main() {
    A6lRadioVoice voice;aidlVoice::Dial d{"offline-placeholder",1};
    voice.dial(1,d);
    assert(requests.size()==1 && requests[0].number==d.address && !requests[0].emergency);
    printf("DIAL_CLIR requested=restrict qmi_arguments=number,emergency_only sent=1\n");
    requests.clear();voice.emergencyDial(2,d,0,{},aidlVoice::EmergencyCallRouting::EMERGENCY,true,true);
    assert(requests.size()==1 && requests[0].emergency);
    printf("EMERGENCY_TEST isTesting=1 modem_dial_requests=%zu emergency=%d\n",requests.size(),requests[0].emergency);
    requests.clear();voice.emergencyDial(3,d,0,{},aidlVoice::EmergencyCallRouting::NORMAL,false,false);
    assert(requests.size()==1 && requests[0].emergency);
    printf("EMERGENCY_ROUTING requested=normal first_request_emergency=%d\n",requests[0].emergency);
    requests.clear();rejectFirst=true;
    voice.emergencyDial(4,d,0,{},aidlVoice::EmergencyCallRouting::EMERGENCY,true,false);
    assert(requests.size()==2 && !requests[1].emergency);
    printf("EMERGENCY_FALLBACK requested=emergency failed_first=1 second_request_emergency=%d\n",requests[1].emergency);
}
