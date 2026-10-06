// Production GNSS engine/QMI/mapping/NMEA, in-process fake transport only.
#include "loc_client.h"
#include "fake_modem.h"
#include "android_map.h"
#include "nmea.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>
using namespace a6l;
using namespace std::chrono_literals;
static int64_t ticks(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
static std::vector<std::string> fields(const std::string& s){std::vector<std::string> out;std::istringstream in(s);std::string x;while(std::getline(in,x,','))out.push_back(x);return out;}
int main(){
    auto modem=std::make_shared<FakeModem>();
    std::atomic<bool> blocked{false},release{false};
    std::atomic<uint64_t> wireUtc{0};std::atomic<uint32_t> wireUnc{0};std::atomic<int64_t> sentAt{0};
    modem->onRequest=[&](FakeModem&,const qmi::Message& r){
        if(r.msgId==loc::kInjectPosition){blocked=true;while(!release.load())std::this_thread::sleep_for(1ms);}
        if(r.msgId==loc::kInjectUtcTime){uint64_t u;uint32_t c;assert(r.getU64(0x01,&u)&&r.getU32(0x02,&c));wireUtc=u;wireUnc=c;sentAt=ticks();}
    };
    EngineConfig cfg;cfg.reconnectMs=20;
    GnssEngine engine([modem]{return std::make_unique<FakeModemTransport>(modem);},nullptr,cfg,{});
    engine.begin();assert(modem->waitFor([&]{return engine.serviceUp();},2000));
    engine.injectLocation(48.0,2.0,10.0f);
    assert(modem->waitFor([&]{return blocked.load();},2000));
    constexpr uint64_t utc=1800000000000ULL;
    int64_t queuedAt=ticks();engine.injectTime(utc,1);
    std::this_thread::sleep_for(350ms);release=true;
    assert(modem->waitFor([&]{return sentAt.load()!=0;},2000));
    int64_t delay=sentAt-queuedAt;
    assert(delay>=300 && wireUtc==utc && wireUnc==1);
    printf("F64 queue_delay_ms=%lld UTC_advanced_ms=%lld advertised_uncertainty_ms=%u\n",(long long)delay,(long long)(wireUtc-utc),unsigned(wireUnc));
    // Positive control: a newer supplied time really is encoded on the next request.
    engine.injectTime(utc+10000,7);
    assert(modem->waitFor([&]{return wireUtc==utc+10000;},2000));
    assert(wireUnc==7);puts("F64 later_request_positive_control supplied_UTC_and_uncertainty_encoded=1");
    engine.end();

    loc::Fix f;f.hasLatLon=true;f.latitude=48;f.longitude=2;f.hasUtc=true;f.utcMs=utc;
    f.hasAltEllipsoid=true;f.altEllipsoid=80;
    auto a=toAndroidLocation(f,0);auto g=fields(nmea::gga(f));
    assert((a.flags&kHasAltitude) && a.altitudeMeters==80);
    assert(g[9]=="80.0" && g[11]=="0.0");
    puts("F22_NMEA ellipsoid_only=80 GGA_MSL=80.0 GGA_geoid=0.0");
    f.hasAltEllipsoid=false;g=fields(nmea::gga(f));
    assert(g[9]=="0.0" && g[11]=="0.0");
    puts("F22_NMEA no_altitude GGA_MSL=0.0 GGA_geoid=0.0");
    f.hasAltEllipsoid=true;f.hasAltMsl=true;f.altMsl=35;g=fields(nmea::gga(f));
    assert(g[9]=="35.0" && g[11]=="45.0");
    puts("F22_NMEA both_datums_positive_control GGA_MSL=35.0 GGA_geoid=45.0");
}
