// SPDX-License-Identifier: Apache-2.0
// A6L AIDL GNSS HAL (agent gnss, 24 Sep 2026).
#define LOG_TAG "A6lGnss"

#include "Gnss.h"

#include <android-base/properties.h>
#include <android/binder_status.h>
#include <log/log.h>
#include <time.h>

#include <algorithm>
#include <cinttypes>

namespace aidl::android::hardware::gnss {

using ::a6l::AndroidLocation;
using ::a6l::AndroidSv;
using ndk::ScopedAStatus;

namespace {

int64_t clockMs(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

int64_t bootNs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return int64_t(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

ScopedAStatus unsupported() { return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); }

void halLog(int level, const std::string& msg) {
    switch (level) {
        case ::a6l::kLogError: ALOGE("%s", msg.c_str()); break;
        case ::a6l::kLogWarn: ALOGW("%s", msg.c_str()); break;
        case ::a6l::kLogInfo: ALOGI("%s", msg.c_str()); break;
        default: ALOGD("%s", msg.c_str()); break;
    }
}

}  // namespace

// --------------------------------------------------------------------------------------------- configuration
// r5 review F10 (28 Sep 2026): IGnssConfiguration.setBlocklist requires the satellites to be excluded from the
// position SOLUTION. Our QMI LOC client (lib/loc_v02.h) has no constellation-control / blacklist-SV request, so the
// modem cannot be told; clearing USED_IN_FIX in the reports only (the old behaviour) misreported the fix. The
// capability is no longer advertised, an empty list (nothing to exclude) is trivially honoured, anything else is refused.
ScopedAStatus A6lGnssConfiguration::setBlocklist(const std::vector<BlocklistedSource>& blocklist) {
    if (blocklist.empty()) return ScopedAStatus::ok();
    ALOGW("blocklist of %zu source(s) refused: modem-side satellite exclusion not supported", blocklist.size());
    return unsupported();
}

// ----------------------------------------------------------------------------------------------------- debug
ScopedAStatus A6lGnssDebug::getDebugData(DebugData* d) {
    std::lock_guard<std::mutex> lk(mMutex);
    d->position.valid = mValid;
    if (mValid) {
        d->position.latitudeDegrees = mLast.latitudeDegrees;
        d->position.longitudeDegrees = mLast.longitudeDegrees;
        d->position.altitudeMeters = float(mLast.altitudeMeters);
        d->position.speedMetersPerSec = float(mLast.speedMetersPerSec);
        d->position.bearingDegrees = float(mLast.bearingDegrees);
        d->position.horizontalAccuracyMeters = mLast.horizontalAccuracyMeters;
        d->position.verticalAccuracyMeters = mLast.verticalAccuracyMeters;
        d->position.speedAccuracyMetersPerSecond = mLast.speedAccuracyMetersPerSecond;
        d->position.bearingAccuracyDegrees = mLast.bearingAccuracyDegrees;
        d->position.ageSeconds = float(clockMs(CLOCK_BOOTTIME) - mLastBootMs) / 1000.f;
    }
    d->time.timeEstimateMs = clockMs(CLOCK_REALTIME);
    d->time.timeUncertaintyNs = mValid ? 1e6f : 1e12f;
    d->time.frequencyUncertaintyNsPerSec = 1e3f;
    d->satelliteDataArray.clear();
    return ScopedAStatus::ok();
}

void A6lGnssDebug::update(const GnssLocation& l, int64_t bootMs) {
    std::lock_guard<std::mutex> lk(mMutex);
    mLast = l;
    mLastBootMs = bootMs;
    mValid = true;
}

// ------------------------------------------------------------------------------------------------------ psds
ScopedAStatus A6lGnssPsds::injectPsdsData(PsdsType type, const std::vector<uint8_t>& data) {
    ALOGI("injectPsdsData type=%d bytes=%zu", int(type), data.size());
    if (type != PsdsType::LONG_TERM) return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    if (!mInject || !mInject(data)) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    return ScopedAStatus::ok();
}

ScopedAStatus A6lGnssPsds::setCallback(const std::shared_ptr<IGnssPsdsCallback>& callback) {
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mCallback = callback;
    }
    // The modem has no XTRA data after boot: ask the framework for a download right away (it waits for a network).
    requestDownload("setCallback");
    return ScopedAStatus::ok();
}

void A6lGnssPsds::requestDownload(const char* why) {
    std::shared_ptr<IGnssPsdsCallback> c;
    {
        std::lock_guard<std::mutex> lk(mMutex);
        c = mCallback;
    }
    ALOGI("PSDS (XTRA) download request (%s)%s", why, c ? "" : ": no callback yet");
    if (c && !c->downloadRequestCb(PsdsType::LONG_TERM).isOk()) ALOGE("downloadRequestCb failed");
}

// ------------------------------------------------------------------------------------------------------ Gnss
Gnss::Gnss()
    : mConfiguration(ndk::SharedRefBase::make<A6lGnssConfiguration>()),
      mDebug(ndk::SharedRefBase::make<A6lGnssDebug>()) {
    mPsds = ndk::SharedRefBase::make<A6lGnssPsds>([this](std::vector<uint8_t> data) {
        std::string why;
        if (!::a6l::loc::looksLikeXtra(data, &why)) {
            ALOGE("PSDS data rejected: %s", why.c_str());
            return false;
        }
        ensureEngine();
        mEngine->injectXtra(std::move(data));   // asynchronous; result in onXtraResult()
        return true;
    });
}

Gnss::~Gnss() {
    if (mEngine) mEngine->end();
}

std::shared_ptr<IGnssCallback> Gnss::cb() {
    std::lock_guard<std::mutex> lk(mMutex);
    return mCallback;
}

void Gnss::ensureEngine() {
    std::lock_guard<std::mutex> lk(mMutex);
    if (mEngine) return;
    ::a6l::EngineConfig c;
    std::string mode = ::android::base::GetProperty("persist.vendor.a6l.gnss.mode", "standalone");
    if (mode == "default") c.operationMode = ::a6l::loc::kModeDefault;
    else if (mode == "msb") c.operationMode = ::a6l::loc::kModeMsb;
    c.unlockEngine = ::android::base::GetBoolProperty("persist.vendor.a6l.gnss.unlock_engine", false);
    c.synthesizeNmea = ::android::base::GetBoolProperty("persist.vendor.a6l.gnss.synth_nmea", true);
    ALOGI("starting QMI LOC engine: mode=%s unlock=%d", mode.c_str(), c.unlockEngine);
    mEngine = std::make_unique<::a6l::GnssEngine>(
            [] { return ::a6l::makeQrtrTransport(halLog); }, this, c, halLog);
    mEngine->begin();
}

ScopedAStatus Gnss::setCallback(const std::shared_ptr<IGnssCallback>& callback) {
    if (callback == nullptr) return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mCallback = callback;
    }
    static_assert(int(::a6l::kCapScheduling) == int(IGnssCallback::CAPABILITY_SCHEDULING));
    static_assert(int(::a6l::kCapSatelliteBlocklist) == int(IGnssCallback::CAPABILITY_SATELLITE_BLOCKLIST));
    int caps = ::a6l::kHalCapabilities;  // r5 F10: no SATELLITE_BLOCKLIST
    if (!callback->gnssSetCapabilitiesCb(caps).isOk()) ALOGE("gnssSetCapabilitiesCb failed");
    IGnssCallback::GnssSystemInfo info;
    info.yearOfHw = 2017;
    info.name = "Hisense A6L SDM660 modem GNSS (QMI LOC)";
    if (!callback->gnssSetSystemInfoCb(info).isOk()) ALOGE("gnssSetSystemInfoCb failed");
    std::vector<GnssSignalType> sig;
    auto add = [&](GnssConstellationType c, double hz, const char* code) {
        GnssSignalType s;
        s.constellation = c;
        s.carrierFrequencyHz = hz;
        s.codeType = code;
        sig.push_back(s);
    };
    add(GnssConstellationType::GPS, 1575.42e6, GnssSignalType::CODE_TYPE_C);
    add(GnssConstellationType::GLONASS, 1602.0e6, GnssSignalType::CODE_TYPE_C);
    add(GnssConstellationType::GALILEO, 1575.42e6, GnssSignalType::CODE_TYPE_C);
    add(GnssConstellationType::BEIDOU, 1561.098e6, GnssSignalType::CODE_TYPE_I);
    add(GnssConstellationType::QZSS, 1575.42e6, GnssSignalType::CODE_TYPE_C);
    add(GnssConstellationType::SBAS, 1575.42e6, GnssSignalType::CODE_TYPE_C);
    if (!callback->gnssSetSignalTypeCapabilitiesCb(sig).isOk()) ALOGE("gnssSetSignalTypeCapabilitiesCb failed");
    ensureEngine();
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::close() {
    ALOGI("close");
    mActive = false;
    if (mEngine) mEngine->setActive(false);
    std::lock_guard<std::mutex> lk(mMutex);
    mCallback = nullptr;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::start() {
    ALOGI("start");
    ensureEngine();
    mActive = true;
    mEngine->setActive(true);
    if (auto c = cb()) c->gnssStatusCb(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::stop() {
    ALOGI("stop");
    mActive = false;
    if (mEngine) mEngine->setActive(false);
    if (auto c = cb()) c->gnssStatusCb(IGnssCallback::GnssStatusValue::SESSION_END);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectTime(int64_t timeMs, int64_t timeReferenceMs, int32_t uncertaintyMs) {
    // timeReferenceMs = elapsedRealtime() when timeMs was sampled
    // r5 review F64: the (timeMs, timeReferenceMs) tuple is handed to the engine unchanged; it is advanced by the
    // elapsed CLOCK_BOOTTIME on the worker immediately before INJECT_UTC_TIME (queue delay included).
    ALOGI("injectTime utc=%" PRId64 " (at boot %" PRId64 ", now %" PRId64 ") unc=%d", timeMs, timeReferenceMs,
          clockMs(CLOCK_BOOTTIME), uncertaintyMs);
    ensureEngine();
    mEngine->injectTime(timeMs, uint32_t(std::max<int32_t>(uncertaintyMs, 0)), timeReferenceMs);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectLocation(const GnssLocation& l) {
    if (!(l.gnssLocationFlags & GnssLocation::HAS_LAT_LONG)) return ScopedAStatus::ok();
    ensureEngine();
    float acc = (l.gnssLocationFlags & GnssLocation::HAS_HORIZONTAL_ACCURACY) ? float(l.horizontalAccuracyMeters)
                                                                              : 5000.f;
    ALOGD("injectLocation acc=%.0f", acc);
    mEngine->injectLocation(l.latitudeDegrees, l.longitudeDegrees, acc);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::injectBestLocation(const GnssLocation& l) { return injectLocation(l); }

// r5 review round6 F54 (29 Sep 2026): every flag means "the next start must not reuse this information". Only the QMI
// deleteAllFlag request (DELETE_ASSIST_DATA 0x44 TLV 0x01) is implemented and verified here, so ANY non-empty subset of
// the defined flags is applied as a full deletion - a deliberate superset (next acquisition is a cold start, XTRA is
// dropped too and re-injected by the framework's PSDS download) instead of an acknowledged no-op. 0 = nothing to
// delete; bits outside the AIDL enum only -> ILLEGAL_ARGUMENT. The engine keeps the request pending across service
// absence and applies it before the next START.
ScopedAStatus Gnss::deleteAidingData(IGnss::GnssAidingData flags) {
    using D = IGnss::GnssAidingData;
    const int f = int(flags);
    const int known = int(D::EPHEMERIS) | int(D::ALMANAC) | int(D::POSITION) | int(D::TIME) | int(D::IONO) |
                      int(D::UTC) | int(D::HEALTH) | int(D::SVDIR) | int(D::SVSTEER) | int(D::SADATA) | int(D::RTI) |
                      int(D::CELLDB_INFO);
    if (f == 0) {
        ALOGI("deleteAidingData 0x0: nothing to delete");
        return ScopedAStatus::ok();
    }
    if (f != int(D::ALL) && !(f & known)) {
        ALOGW("deleteAidingData 0x%x: no defined flag", f);
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    ALOGI("deleteAidingData 0x%x -> %s", f, f == int(D::ALL) ? "all" : "all (superset: selective deletion not mapped)");
    ensureEngine();
    mEngine->deleteAll();
    return ScopedAStatus::ok();
}

// r5 review round6 F53: recurrence is honoured (QMI START fixRecurrence SINGLE, one-result completion in the engine).
// mode (STANDALONE/MS_BASED) and lowPowerMode are not applied: this ROM has no SUPL path (standalone + XTRA) and does
// not advertise low-power capability.
ScopedAStatus Gnss::setPositionMode(const IGnss::PositionModeOptions& o) {
    ALOGI("setPositionMode mode=%d recurrence=%d interval=%d lowPower=%d", int(o.mode), int(o.recurrence),
          o.minIntervalMs, o.lowPowerMode);
    ensureEngine();
    const bool single = o.recurrence == IGnss::GnssPositionRecurrence::RECURRENCE_SINGLE;
    mEngine->setPositionMode(uint32_t(std::max(1000, o.minIntervalMs)), single);
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::startSvStatus() { mSvStatusOn = true; return ScopedAStatus::ok(); }
ScopedAStatus Gnss::stopSvStatus() { mSvStatusOn = false; return ScopedAStatus::ok(); }
ScopedAStatus Gnss::startNmea() { mNmeaOn = true; return ScopedAStatus::ok(); }
ScopedAStatus Gnss::stopNmea() { mNmeaOn = false; return ScopedAStatus::ok(); }

ScopedAStatus Gnss::getExtensionGnssConfiguration(std::shared_ptr<IGnssConfiguration>* out) {
    *out = mConfiguration;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* out) {
    *out = mDebug;
    return ScopedAStatus::ok();
}

ScopedAStatus Gnss::getExtensionPsds(std::shared_ptr<IGnssPsds>* out) {
    if (!::android::base::GetBoolProperty("persist.vendor.a6l.gnss.xtra", true)) return unsupported();
    *out = mPsds;
    return ScopedAStatus::ok();
}
ScopedAStatus Gnss::getExtensionGnssMeasurement(std::shared_ptr<IGnssMeasurementInterface>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionGnssPowerIndication(std::shared_ptr<IGnssPowerIndication>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionGnssBatching(std::shared_ptr<IGnssBatching>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionGnssNavigationMessage(std::shared_ptr<IGnssNavigationMessageInterface>*) {
    return unsupported();
}
ScopedAStatus Gnss::getExtensionAGnss(std::shared_ptr<IAGnss>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionAGnssRil(std::shared_ptr<IAGnssRil>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionGnssVisibilityControl(std::shared_ptr<visibility_control::IGnssVisibilityControl>*) {
    return unsupported();
}
ScopedAStatus Gnss::getExtensionGnssAntennaInfo(std::shared_ptr<IGnssAntennaInfo>*) { return unsupported(); }
ScopedAStatus Gnss::getExtensionMeasurementCorrections(
        std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>*) {
    return unsupported();
}
ScopedAStatus Gnss::getExtensionGnssAssistanceInterface(std::shared_ptr<gnss_assistance::IGnssAssistanceInterface>*) {
    return unsupported();
}

// ------------------------------------------------------------------------------------------ engine listener
void Gnss::onFix(const ::a6l::loc::Fix& f) {
    AndroidLocation a = ::a6l::toAndroidLocation(f, clockMs(CLOCK_REALTIME));
    GnssLocation l;
    l.gnssLocationFlags = a.flags;
    l.latitudeDegrees = a.latitudeDegrees;
    l.longitudeDegrees = a.longitudeDegrees;
    l.altitudeMeters = a.altitudeMeters;
    l.speedMetersPerSec = a.speedMetersPerSec;
    l.bearingDegrees = a.bearingDegrees;
    l.horizontalAccuracyMeters = a.horizontalAccuracyMeters;
    l.verticalAccuracyMeters = a.verticalAccuracyMeters;
    l.speedAccuracyMetersPerSecond = a.speedAccuracyMetersPerSecond;
    l.bearingAccuracyDegrees = a.bearingAccuracyDegrees;
    l.timestampMillis = a.timestampMillis;
    l.elapsedRealtime.flags = ElapsedRealtime::HAS_TIMESTAMP_NS | ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS;
    l.elapsedRealtime.timestampNs = bootNs();
    l.elapsedRealtime.timeUncertaintyNs = 50e6;   // indication latency (QMI over SMD), not measured
    mDebug->update(l, clockMs(CLOCK_BOOTTIME));
    if (!mActive) return;
    if (auto c = cb()) {
        if (!c->gnssLocationCb(l).isOk()) ALOGE("gnssLocationCb failed");
    }
}

void Gnss::onSvs(const std::vector<::a6l::loc::Sv>& svs, const std::vector<uint16_t>& used) {
    if (!mSvStatusOn || !mActive) return;
    auto c = cb();
    if (!c) return;
    std::vector<IGnssCallback::GnssSvInfo> list;
    for (const AndroidSv& s : ::a6l::toAndroidSvs(svs, used)) {
        IGnssCallback::GnssSvInfo i;
        i.svid = s.svid;
        i.constellation = GnssConstellationType(s.constellation);
        i.cN0Dbhz = s.cN0DbHz;
        i.basebandCN0DbHz = s.basebandCN0DbHz;
        i.elevationDegrees = s.elevationDegrees;
        i.azimuthDegrees = s.azimuthDegrees;
        i.carrierFrequencyHz = int64_t(s.carrierFrequencyHz);
        i.svFlag = s.svFlag;  // r5 F10: USED_IN_FIX exactly as the modem's position report says
        GnssSignalType st;
        st.constellation = i.constellation;
        st.carrierFrequencyHz = s.carrierFrequencyHz;
        st.codeType = s.constellation == 5 ? GnssSignalType::CODE_TYPE_I : GnssSignalType::CODE_TYPE_C;
        i.signalType = st;
        list.push_back(i);
    }
    if (!c->gnssSvStatusCb(list).isOk()) ALOGE("gnssSvStatusCb failed");
}

void Gnss::onNmea(const std::string& s, bool) {
    if (!mNmeaOn || !mActive) return;
    if (auto c = cb()) c->gnssNmeaCb(clockMs(CLOCK_REALTIME), s);
}

void Gnss::onEngineState(bool on) {
    if (auto c = cb())
        c->gnssStatusCb(on ? IGnssCallback::GnssStatusValue::ENGINE_ON : IGnssCallback::GnssStatusValue::ENGINE_OFF);
}

void Gnss::onServiceState(bool up) { ALOGI("modem LOC service %s", up ? "up" : "down"); }

void Gnss::onTimeRequest() {
    if (auto c = cb()) c->gnssRequestTimeCb();
}

void Gnss::onPositionRequest() {
    if (auto c = cb()) c->gnssRequestLocationCb(false, false);
}

void Gnss::onOrbitsRequest() {
    if (mPsds) mPsds->requestDownload("modem INJECT_PREDICTED_ORBITS_REQ");
}

void Gnss::onXtraResult(bool ok, const std::string& detail) {
    if (ok)
        ALOGI("XTRA injected: %s", detail.c_str());
    else
        ALOGE("XTRA injection failed: %s", detail.c_str());
}

void Gnss::onXtraInfo(const std::string& detail) { ALOGI("XTRA %s", detail.c_str()); }

}  // namespace aidl::android::hardware::gnss
