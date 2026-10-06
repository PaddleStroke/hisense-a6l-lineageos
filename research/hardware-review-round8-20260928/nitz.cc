// Exact HAL forwarding method. Virtual time avoids a real suspend or clock change.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>
namespace android { int64_t elapsedRealtime(){return 14000;} }
enum class RadioIndicationType {UNSOLICITED};
struct Indication {
    int64_t received=-1,age=-1;
    void nitzTimeReceived(RadioIndicationType,const std::string&,int64_t r,int64_t a){received=r;age=a;}
};
struct A6lRadioNetwork {
    Indication out;
    Indication* indicate(){return &out;}
    void onNitz(const std::string&,int64_t);
};
#include "nitz_method.inc"
int main(){
    A6lRadioNetwork hal;
    // Sample received at t=10000; delayed until t=14000 by the core worker queue.
    hal.onNitz("26/09/28,12:00:00+0,0",10000);
    assert(hal.out.received==14000 && hal.out.age==0);
    assert(hal.out.received-hal.out.age != 10000);
    puts("F61 sample_received_ms=10000 sent_ms=14000 reported_age_ms=0 reference_error_ms=4000");
    hal.onNitz("26/09/28,12:00:00+0,0",14000);
    assert(hal.out.received-hal.out.age==14000);
    puts("F61 immediate_delivery_positive_control reference_error_ms=0");
}
