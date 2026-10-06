// Exact HAL method bodies + production QMI code + in-process fake modem.
// The executor and Binder responses are host recording fixtures.
#include <a6lqmi/services.h>
#include <a6lqmi/log.h>
#include "fake_modem.h"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <deque>
#include <functional>
#include <thread>
#include <atomic>
namespace qmi = a6l::qmi;
namespace qv = a6l::qmi::voice;
namespace aidlVoice { enum class TtyMode { OFF=0, FULL=1, HCO=2, VCO=3 }; }
struct ScopedAStatus {};
ScopedAStatus ok() { return {}; }
enum class RadioError { INVALID_STATE=1 };
int noError(int) { return 0; }
int errorResponse(int, RadioError) { return 1; }
int errorResponse(int, int e) { return e; }
int toRadioError(const qmi::Result& r) { return r.ok() ? 0 : 1; }
struct Executor {
    std::deque<std::function<void()>> q;
    void post(std::function<void()> f) { q.push_back(std::move(f)); }
    void drain() { while (!q.empty()) { auto f=std::move(q.front()); q.pop_front(); f(); } }
};
struct Response {
    int error=-1; aidlVoice::TtyMode tty=aidlVoice::TtyMode::OFF;
    void setTtyModeResponse(int e) { error=e; }
    void getTtyModeResponse(int e, aidlVoice::TtyMode t) { error=e; tty=t; }
    void sendDtmfResponse(int e) { error=e; }
    void stopDtmfResponse(int e) { error=e; }
    void rejectCallResponse(int e) { error=e; }
};
struct Core {
    qmi::Client* client;
    qmi::Client& ctl() { return *client; }
    std::vector<qv::CallInfo> calls(bool) {
        std::vector<qv::CallInfo> out;
        auto r=qv::getAllCalls(*client, &out); assert(r.ok()); return out;
    }
};
struct A6lRadioVoice {
    Core core; Executor mExec; Response response;
    aidlVoice::TtyMode mTty=aidlVoice::TtyMode::OFF;
    Core& slotCore() { return core; }
    Response* respond() { return &response; }
    std::optional<uint8_t> findCall(std::initializer_list<uint8_t>);
    ScopedAStatus setTtyMode(int32_t, aidlVoice::TtyMode);
    ScopedAStatus getTtyMode(int32_t);
    ScopedAStatus sendDtmf(int32_t, const std::string&);
    ScopedAStatus stopDtmf(int32_t);
    ScopedAStatus rejectCall(int32_t);
};
#include "voice_methods.inc"
static size_t count(a6l::test::FakeModem& f, uint16_t id) {
    size_t n=0; for(auto& x:f.requests()) if(x.second.msgId==id) ++n; return n;
}
int main() {
    qmi::gLogLevel=0; a6l::test::FakeModem modem;
    std::atomic<bool> failStop{false}, tone{false}, heldReleased{false};
    std::atomic<uint8_t> state{qv::kStateConversation};
    modem.setHandler([&](uint32_t, const qmi::Message& r)->std::optional<qmi::Message> {
        if (r.msgId==qv::kGetAllCallInfo)
            return a6l::test::okResponse(r.msgId).raw(0x10, {1,7,state.load(),qv::kTypeVoice,qv::kDirMo,qv::kModeUmts,0,0});
        if (r.msgId==qv::kStartContDtmf) tone=true;
        if (r.msgId==qv::kStopContDtmf) {
            if (failStop) return a6l::test::errResponse(r.msgId,3);
            tone=false;
        }
        if (r.msgId==qv::kManageCalls && r.get(0x01)->at(0)==qv::kSupsReleaseHeldOrWaiting && state==qv::kStateHold)
            heldReleased=true;
        return a6l::test::okResponse(r.msgId);
    });
    modem.addService(qmi::kSvcVoice);
    qmi::Client client(modem.transport(),"round7");
    assert(client.start({qmi::kSvcVoice}));
    assert(client.waitForServices({qmi::kSvcVoice},2000).empty());
    A6lRadioVoice hal; hal.core.client=&client;
    auto before=modem.requests().size();
    for(auto t:{aidlVoice::TtyMode::FULL,aidlVoice::TtyMode::HCO,aidlVoice::TtyMode::VCO}) {
        hal.setTtyMode(1,t); hal.getTtyMode(2);
        assert(hal.response.error==0 && hal.response.tty==t);
    }
    assert(modem.requests().size()==before);
    puts("F55 TTY_FULL_HCO_VCO success=1 getter_echoes_request=1 modem_requests=0");

    failStop=true;
    hal.sendDtmf(3,"5"); hal.mExec.drain();
    assert(hal.response.error==0 && tone && count(modem,qv::kStopContDtmf)==1);
    failStop=false; std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(tone && count(modem,qv::kStopContDtmf)==1);
    puts("F56 DTMF_stop_rejected HAL_success=1 fake_continuous_tone=1 cleanup_retries=0");
    hal.stopDtmf(4); hal.mExec.drain();
    assert(hal.response.error==0 && !tone);
    puts("F56 explicit_stop_positive_control tone=0");

    // A waiting call disappears after Reject is queued, leaving an unrelated held call.
    state=qv::kStateWaiting; hal.rejectCall(5);
    state=qv::kStateHold; hal.mExec.drain();
    assert(hal.response.error==0 && heldReleased && count(modem,qv::kManageCalls)==1);
    auto all=modem.requests();
    for(auto& x:all) if(x.second.msgId==qv::kManageCalls) {
        assert(x.second.get(0x01)->at(0)==qv::kSupsReleaseHeldOrWaiting && !x.second.get(0x10));
    }
    puts("F57 reject_with_only_held_call success=1 broad_release_request=1 no_call_id=1 fake_held_released=1");
    heldReleased=false; state=qv::kStateIncoming;
    hal.rejectCall(6); hal.mExec.drain();
    assert(hal.response.error==0 && count(modem,qv::kEndCall)==1 && !heldReleased);
    puts("F57 incoming_call_positive_control targeted_END_CALL=1");
    client.stop();
}
