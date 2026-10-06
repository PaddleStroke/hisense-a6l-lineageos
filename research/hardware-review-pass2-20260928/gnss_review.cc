#include "loc_client.h"
#include "fake_modem.h"
#include "android_map.h"
#include <cassert>
#include <cstdio>
#include <thread>
using namespace a6l;
using namespace std::chrono_literals;
int main() {
    for(int mode=0;mode<2;++mode) {
        auto fake=std::make_shared<FakeModem>();
        uint16_t failedId=mode==0?loc::kRegEvents:loc::kStart;
        fake->errors[failedId]=3;
        EngineConfig cfg; cfg.reconnectMs=50;
        GnssEngine engine([fake]{return std::make_unique<FakeModemTransport>(fake);},nullptr,cfg,nullptr);
        engine.begin();
        assert(fake->waitFor([&]{return fake->count(loc::kSetNmeaTypes)>=1;},2000));
        engine.setActive(true);
        assert(fake->waitFor([&]{return fake->count(loc::kStart)>=1;},2000));
        std::this_thread::sleep_for(100ms);
        {std::lock_guard<std::mutex> lock(fake->mu);fake->errors.clear();}
        size_t before=fake->count(failedId);
        std::this_thread::sleep_for(1300ms);
        printf("GNSS_TRANSIENT mode=%d attempts_before=%zu attempts_after=%zu session=%d desired_on=1\n",
               mode,before,fake->count(failedId),engine.sessionRunning());
        assert(before==1 && fake->count(failedId)==1);
        assert(engine.sessionRunning()==(mode==0));
        engine.end();
    }
    loc::Fix fix;
    fix.hasLatLon=true; fix.latitude=45; fix.longitude=5;
    fix.hasAltMsl=true; fix.altMsl=123; fix.hasAltEllipsoid=false;
    auto location=toAndroidLocation(fix,1000);
    printf("MSL_ONLY_FIX android_has_altitude=%d reported_ellipsoid_altitude=%.1f\n",
           !!(location.flags&kHasAltitude),location.altitudeMeters);
    assert((location.flags&kHasAltitude) && location.altitudeMeters==123);
    puts("REPRODUCED GNSS retry failures and MSL-as-ellipsoid mapping.");
}
