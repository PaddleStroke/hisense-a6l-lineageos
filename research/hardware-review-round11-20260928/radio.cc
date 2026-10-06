// Offline production ModemCore + QMI over the repository's in-process fake transport.
#define main unused_existing_modemcore_main
#include "modemcore_tests.cc"
#undef main
#include <fstream>
#include <future>

static std::atomic<bool> rejectRegistrations{true};
static std::atomic<int> voiceAccepted{0}, smsAccepted{0};
struct TimeListener : L {
    std::mutex timeLock;
    std::vector<std::string> times;
    void onNitz(const std::string& n, int64_t) override {
        std::lock_guard<std::mutex> g(timeLock);
        times.push_back(n);
    }
    size_t size(){std::lock_guard<std::mutex> g(timeLock);return times.size();}
};
static void drain(ModemCore& core) {
    auto p=std::make_shared<std::promise<void>>();auto f=p->get_future();
    core.post([p]{p->set_value();});
    EXPECT(f.wait_for(std::chrono::seconds(3))==std::future_status::ready);
}
int main(int argc,char** argv) {
    if(argc!=2)return 2;
    gLogLevel=0;
    android::base::SetProperty("ro.vendor.a6l.ril.slots","1");
    gS.cardTlv=cardStatusTlv(0x0000,0xFFFF,{{uim::kCardPresent,0,uim::kAppStateReady}});
    auto* modem=new FakeModem();
    modem->setHandler([](uint32_t svc,const Message& req)->std::optional<Message>{
        bool voice=svc==kSvcVoice && req.msgId==voice::kIndicationRegister;
        bool sms=svc==kSvcWms && req.msgId==wms::kSetEventReport;
        if(voice||sms){
            if(rejectRegistrations)return test::errResponse(req.msgId,kErrInternal);
            if(voice)++voiceAccepted;else ++smsAccepted;
        }
        return handler(svc,req);
    });
    ModemCore::setTransportFactory([modem]{return modem->transport();});
    for(auto s:kSvcs)modem->addService(s);
    auto& core=ModemCore::get(1);auto* listener=new TimeListener();core.addListener(listener);core.start();
    EXPECT(waitFor([&]{return core.ready()&&listener->ready>=1;}));
    drain(core);
    EXPECT(voiceAccepted==0 && smsAccepted==0);
    const int startupVoice=count(*modem,kSvcVoice,voice::kIndicationRegister);
    const int startupSms=count(*modem,kSvcWms,wms::kSetEventReport);
    rejectRegistrations=false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));drain(core);
    EXPECT(count(*modem,kSvcVoice,voice::kIndicationRegister)==startupVoice);
    EXPECT(count(*modem,kSvcWms,wms::kSetEventReport)==startupSms);
    EXPECT(voiceAccepted==0 && smsAccepted==0 && core.ready());
    printf("F17 startup ready=%d voice_attempts=%d sms_attempts=%d successes_after_fault_cleared=%d/%d\n",
           core.ready(),startupVoice,startupSms,int(voiceAccepted),int(smsAccepted));

    auto cycle=[&]{
        modem->removeService(kSvcVoice);modem->removeService(kSvcWms);
        EXPECT(waitFor([&]{return !core.ctl().hasService(kSvcVoice)&&!core.ctl().hasService(kSvcWms);}));
        drain(core);
        modem->addService(kSvcVoice);modem->addService(kSvcWms);
    };
    // Positive control: successful isolated restoration really enables the registrations.
    cycle();EXPECT(waitFor([]{return voiceAccepted==1&&smsAccepted==1;}));drain(core);
    puts("F17 healthy_service_return_positive_control registrations_accepted=1/1");

    rejectRegistrations=true;
    const int beforeVoice=count(*modem,kSvcVoice,voice::kIndicationRegister);
    const int beforeSms=count(*modem,kSvcWms,wms::kSetEventReport);
    cycle();
    EXPECT(waitFor([&]{return count(*modem,kSvcVoice,voice::kIndicationRegister)>beforeVoice &&
                           count(*modem,kSvcWms,wms::kSetEventReport)>beforeSms;}));drain(core);
    rejectRegistrations=false;
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));drain(core);
    EXPECT(count(*modem,kSvcVoice,voice::kIndicationRegister)==beforeVoice+1);
    EXPECT(count(*modem,kSvcWms,wms::kSetEventReport)==beforeSms+1);
    EXPECT(voiceAccepted==1&&smsAccepted==1&&core.ready());
    printf("F17 rejected_service_return retries_after_fault_cleared=0/0 ready=%d\n",core.ready());
    cycle();EXPECT(waitFor([]{return voiceAccepted==2&&smsAccepted==2;}));drain(core);
    puts("F17 second_service_cycle_positive_control registrations_accepted=2/2");

    auto time=[&](bool tz,bool dst){
        Message m(MsgType::Indication,nas::kNetworkTimeInd);
        m.raw(0x01,{0xEA,0x07,9,28,12,0,0,0});if(tz)m.u8(0x10,8);if(dst)m.u8(0x11,1);
        size_t before=listener->size();modem->indicate(kSvcNas,m);
        EXPECT(waitFor([&]{return listener->size()==before+1;}));
    };
    time(false,false);time(true,false);time(true,true);drain(core);
    {
        std::lock_guard<std::mutex> g(listener->timeLock);
        EXPECT(listener->times.size()==3);
        if(listener->times.size()==3){
            EXPECT(listener->times[0]=="26/09/28,12:00:00+0,0");
            EXPECT(listener->times[1]=="26/09/28,12:00:00+8,0");
            EXPECT(listener->times[2]=="26/09/28,12:00:00+8,1");
            std::ofstream out(argv[1]);
            for(auto& s:listener->times){out<<s<<'\n';printf("F65 nitz=%s\n",s.c_str());}
        }
    }
    printf("ROUND11_RADIO defect assertions passed=%d failed=%d\n",gPass,gFail);fflush(stdout);
    // ModemCore has process-lifetime detached loops and no stop API, as in the repository tests.
    std::_Exit(gFail?1:0);
}
