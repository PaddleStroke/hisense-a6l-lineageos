// Production QMI wire codec -> position parser -> Android location mapper, offline.
#include "qmi.h"
#include "loc_v02.h"
#include "android_map.h"
#include <cassert>
#include <cstdio>
using namespace a6l;
static AndroidLocation map(int confidence, bool uncertainties=true) {
    qmi::Message m;
    m.type=qmi::kIndication;m.msgId=loc::kIndPosition;
    m.add(0x01,qmi::Writer().u32(loc::kStatusSuccess));
    m.add(0x02,qmi::Writer().u8(1));
    m.add(0x10,qmi::Writer().f64(48));m.add(0x11,qmi::Writer().f64(2));
    m.add(0x1A,qmi::Writer().f32(80));
    m.add(0x25,qmi::Writer().u64(1800000000000ULL));
    if(uncertainties){
        m.add(0x12,qmi::Writer().f32(10));
        m.add(0x1C,qmi::Writer().f32(20));
    }
    if(confidence>=0){
        m.add(0x16,qmi::Writer().u8(confidence));
        m.add(0x1D,qmi::Writer().u8(confidence));
    }
    auto bytes=qmi::encode(m);qmi::Message wire;std::string error;
    assert(qmi::decode(bytes.data(),bytes.size(),&wire,&error));
    if(confidence>=0){uint8_t h=0,v=0;assert(wire.getU8(0x16,&h)&&wire.getU8(0x1D,&v));assert(h==confidence&&v==confidence);}
    loc::Fix f;assert(loc::parsePosition(wire,&f));
    assert(f.status==loc::kStatusSuccess&&f.hasLatLon&&f.latitude==48&&f.longitude==2);
    return toAndroidLocation(f,0);
}
int main(){
    for(int c:{39,50,68,95}){
        auto a=map(c);
        assert((a.flags&kHasHorizontalAccuracy)&&(a.flags&kHasVerticalAccuracy));
        assert(a.horizontalAccuracyMeters==10&&a.verticalAccuracyMeters==20);
        printf("F66 input_confidence_pct=%d Android_horizontal_m=%.1f Android_vertical_m=%.1f flags=%d\n",
               c,a.horizontalAccuracyMeters,a.verticalAccuracyMeters,a.flags);
    }
    auto absent=map(-1);
    assert((absent.flags&kHasHorizontalAccuracy)&&(absent.flags&kHasVerticalAccuracy));
    printf("F66 absent_confidence Android_horizontal_m=%.1f Android_vertical_m=%.1f\n",
           absent.horizontalAccuracyMeters,absent.verticalAccuracyMeters);
    auto noAccuracy=map(68,false);
    assert(!(noAccuracy.flags&kHasHorizontalAccuracy)&&!(noAccuracy.flags&kHasVerticalAccuracy));
    puts("F66 no_uncertainty_positive_control accuracy_flags_absent=1");
    puts("ROUND12_ACCURACY_REPRODUCTION_PASS (defect present, not fixed)");
}
