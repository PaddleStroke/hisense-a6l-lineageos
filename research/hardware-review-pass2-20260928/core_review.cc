// Reuse the existing fake card/modem/listener helpers; invoke only our review cases.
#define main existing_modemcore_tests_main
#include "modemcore_tests.cc"
#undef main
#include <cassert>
static std::atomic<bool> rejectPower{false};
int main() {
    gLogLevel=0;
    android::base::SetProperty("ro.vendor.a6l.ril.slots","1");
    gS.cardTlv=cardStatusTlv(0,0xffff,{{uim::kCardPresent,0,uim::kAppStateReady}});
    auto* modem=new FakeModem();
    modem->setHandler([](uint32_t svc,const Message& req) -> std::optional<Message> {
        if(rejectPower && svc==kSvcDms && req.msgId==dms::kSetOperatingMode)
            return test::errResponse(req.msgId,kErrInternal);
        return handler(svc,req);
    });
    ModemCore::setTransportFactory([modem]{return modem->transport();});
    for(auto s:kSvcs) modem->addService(s);
    auto& core=ModemCore::get(1);
    auto* listener=new L(); core.addListener(listener); core.start();
    assert(waitFor([&]{return core.ready() && listener->ready>=1;}));
    assert(core.setRadioPower(true));
    assert(core.radioOn());
    rejectPower=true;
    bool accepted=core.setRadioPower(false);
    int mode;
    {std::lock_guard<std::mutex> lock(gS.lock); mode=gS.opMode;}
    printf("FAILED_RADIO_OFF accepted=%d reported_on=%d actual_modem_online=%d\n",
           accepted,core.radioOn(),mode==dms::kOnline);
    assert(!accepted && !core.radioOn() && mode==dms::kOnline);
    rejectPower=false;
    assert(core.setRadioPower(true));

    Message call(MsgType::Indication,voice::kAllCallStatusInd);
    call.raw(0x01,{1,1,voice::kStateConversation,0,voice::kDirMo,3,0,0});
    modem->indicate(kSvcVoice,call);
    assert(waitFor([&]{return android::base::GetProperty("vendor.a6l.voice.active","")=="1";}));
    int before=count(*modem,kSvcVoice,voice::kIndicationRegister);
    modem->removeService(kSvcVoice);
    assert(waitFor([&]{return !core.ctl().hasService(kSvcVoice);}));
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    printf("VOICE_SERVICE_LOST ready=%d voice_active=%s cached_calls=%zu\n",core.ready(),
           android::base::GetProperty("vendor.a6l.voice.active","").c_str(),core.calls().size());
    assert(core.ready() && android::base::GetProperty("vendor.a6l.voice.active","")=="1");
    modem->addService(kSvcVoice);
    assert(waitFor([&]{return core.ctl().hasService(kSvcVoice);}));
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    int after=count(*modem,kSvcVoice,voice::kIndicationRegister);
    printf("VOICE_SERVICE_RETURN register_before=%d register_after=%d\n",before,after);
    assert(before==after);
    puts("REPRODUCED failed power-off state mismatch and isolated VOICE recovery failure.");
    fflush(stdout);
    // ModemCore intentionally has process-lifetime threads and no stop API, as in existing tests.
    _Exit(0);
}
