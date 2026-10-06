#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include "loc_v02.h"
#define ALOGI(...) ((void)0)
struct ScopedAStatus { static ScopedAStatus ok() { return {}; } };
struct IGnss {
    enum class GnssAidingData { EPHEMERIS=1, TIME=8, ALL=65535 };
    struct PositionModeOptions { int mode, recurrence, minIntervalMs; bool lowPowerMode; };
};
struct Engine {
    unsigned interval = 0; int deletes = 0;
    void setInterval(unsigned ms) { interval = ms; }
    void deleteAll() { ++deletes; }
};
struct Gnss {
    Engine engine; Engine *mEngine = &engine;
    void ensureEngine() {}
    ScopedAStatus setPositionMode(const IGnss::PositionModeOptions&);
    ScopedAStatus deleteAidingData(IGnss::GnssAidingData);
};
#include "controls.inc"
int main() {
    Gnss g;
    g.setPositionMode({0, 0, 10000, false});
    auto periodic = a6l::loc::makeStart(1, g.engine.interval, true);
    g.setPositionMode({0, 1, 10000, false});
    auto single = a6l::loc::makeStart(1, g.engine.interval, true);
    uint32_t value = 0;
    assert(single.getU32(0x10, &value) && value == 1);
    assert(a6l::qmi::encode(single) == a6l::qmi::encode(periodic));
    puts("POSITION_MODE single_and_periodic_identical=1 wire_recurrence=1(periodic)");
    g.deleteAidingData(IGnss::GnssAidingData::EPHEMERIS);
    g.deleteAidingData(IGnss::GnssAidingData::TIME);
    assert(g.engine.deletes == 0);
    puts("DELETE_AIDING ephemeris_and_time_success=1 engine_deletes=0");
    g.deleteAidingData(IGnss::GnssAidingData::ALL);
    assert(g.engine.deletes == 1);
    puts("DELETE_AIDING all_positive_control=1");
}
