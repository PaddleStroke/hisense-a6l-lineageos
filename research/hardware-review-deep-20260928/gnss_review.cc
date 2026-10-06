#include "loc_client.h"
#include "fake_modem.h"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <thread>
using namespace a6l;
using namespace std::chrono_literals;
struct Listener:EngineListener {
    std::atomic<int> done{0};
    void onXtraResult(bool ok,const std::string&) override {done=ok?1:2;}
};
static qmi::Message ind(uint16_t id) {qmi::Message m;m.type=qmi::kIndication;m.msgId=id;return m;}
int main() {
    for(int mode=0;mode<3;mode++) {
        auto fake=std::make_shared<FakeModem>(); Listener l;
        std::atomic<int> parts{0};
        fake->onRequest=[mode,&parts](FakeModem& f,const qmi::Message& r) {
            auto m=ind(r.msgId);
            if(r.msgId==loc::kGetPredictedOrbitsSource || r.msgId==loc::kGetPredictedOrbitsValidity) {
                m.add(1,qmi::Writer().u32(0));f.inject(m);
            } else if(r.msgId==loc::kInjectPredictedOrbits) {
                ++parts;
                if(mode==2) return; // WaitInd blocks worker; stop requested while upload is pending.
                if(mode==0) {m.add(1,qmi::Writer().u32(0));m.add(0x10,qmi::Writer().u16(99));}
                // mode 1: empty/malformed indication, lacking even mandatory status.
                f.inject(m);
            }
        };
        EngineConfig cfg; GnssEngine e([fake]{return std::make_unique<FakeModemTransport>(fake);},&l,cfg,nullptr);
        e.begin();assert(fake->waitFor([&]{return fake->count(loc::kSetNmeaTypes)==1;},2000));
        e.setActive(true);assert(fake->waitFor([&]{return e.sessionRunning();},2000));
        std::vector<uint8_t> data(2500);for(size_t i=0;i<data.size();i++)data[i]=uint8_t(i*7+1);
        e.injectXtra(data);
        if(mode<2) {
            assert(fake->waitFor([&]{return l.done!=0;},2000));
            printf("XTRA_ACK mode=%s success=%d parts=%d\n",mode==0?"wrong-part":"missing-status",l.done==1,parts.load());
            assert(l.done==1 && parts==3);
        } else {
            assert(fake->waitFor([&]{return parts>=1;},2000));
            e.setActive(false);std::this_thread::sleep_for(1200ms);
            printf("GNSS_STOP_DURING_UPLOAD stop_requests=%zu session_still_running=%d elapsed_ms=1200\n",fake->count(loc::kStop),e.sessionRunning());
            assert(fake->count(loc::kStop)==0 && e.sessionRunning());
        }
        e.end();
    }
}
