// Complete production ModemCore + QMI. Reuse only the existing fake setup helpers.
#define main original_fixture_main
#include "modemcore_tests.cc"
#undef main
#include <cassert>
int main() {
    gLogLevel=0;
    android::base::SetProperty("ro.vendor.a6l.ril.slots","1");
    gS.cardTlv=cardStatusTlv(0,0xffff,{{uim::kCardPresent,0,uim::kAppStateReady}});
    auto* modem=new FakeModem(); modem->setHandler(handler);
    ModemCore::setTransportFactory([modem]{return modem->transport();});
    for(auto svc:kSvcs) modem->addService(svc);
    auto& core=ModemCore::get(1); core.start();
    assert(waitFor([&]{return core.ready();}));
    Message active(MsgType::Indication,voice::kAllCallStatusInd);
    active.raw(0x01,{1,7,voice::kStateConversation,voice::kTypeVoice,voice::kDirMo,voice::kModeUmts,0,0});
    modem->indicate(kSvcVoice,active);
    assert(waitFor([&]{return core.calls(false).size()==1;}));
    modem->removeService(kSvcVoice);
    assert(waitFor([&]{return core.calls(false).empty() && hoststub::logContains("VOICE service gone");}));
    assert(core.lastCallFailCause()==16);
    puts("F58 active_call_lost_with_VOICE_service last_call_fail_cause=16(NORMAL_CLEARING)");
    // Existing ModemCore is process-lifetime with no stop/join API.
    fflush(stdout); _Exit(0);
}
