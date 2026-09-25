// SPDX-License-Identifier: Apache-2.0
// A6L AIDL GNSS HAL (agent gnss, 24 Sep 2026): android.hardware.gnss V7 IGnss backed by the modem's QMI LOC service
// (QRTR service 16) through liba6l_qmiloc's GnssEngine. Extensions: IGnssConfiguration (blocklist) and IGnssDebug
// (last fix), IGnssPsds (XTRA: the framework downloads LONGTERM_PSDS_SERVER_1 over the ROM's network and injects it;
// misc agent 24 Sep); every other extension returns EX_UNSUPPORTED_OPERATION ("not supported").
#pragma once

#include <aidl/android/hardware/gnss/BnGnss.h>
#include <aidl/android/hardware/gnss/BnGnssConfiguration.h>
#include <aidl/android/hardware/gnss/BnGnssDebug.h>
#include <aidl/android/hardware/gnss/BnGnssPsds.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <utility>

#include "android_map.h"
#include "loc_client.h"

namespace aidl::android::hardware::gnss {

class A6lGnssConfiguration : public BnGnssConfiguration {
  public:
    ndk::ScopedAStatus setSuplVersion(int) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setSuplMode(int) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setLppProfile(int) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setGlonassPositioningProtocol(int) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setEmergencySuplPdn(bool) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setEsExtensionSec(int) override { return ndk::ScopedAStatus::ok(); }
    ndk::ScopedAStatus setBlocklist(const std::vector<BlocklistedSource>& blocklist) override;
    bool isBlocklisted(int constellation, int svid) const;

  private:
    mutable std::mutex mMutex;
    std::set<std::pair<int, int>> mBlocked;   // (constellation, svid); svid 0 = whole constellation
};

class A6lGnssDebug : public BnGnssDebug {
  public:
    ndk::ScopedAStatus getDebugData(DebugData* debugData) override;
    void update(const GnssLocation& l, int64_t bootMs);

  private:
    std::mutex mMutex;
    bool mValid = false;
    GnssLocation mLast;
    int64_t mLastBootMs = 0;
};

// XTRA ("PSDS" in AIDL). LONG_TERM data = the Qualcomm XTRA file (xtra3grc.bin / xtra2.bin); NORMAL/REALTIME are
// not supported by this modem path and are rejected.
class A6lGnssPsds : public BnGnssPsds {
  public:
    using Injector = std::function<bool(std::vector<uint8_t>)>;
    explicit A6lGnssPsds(Injector inj) : mInject(std::move(inj)) {}
    ndk::ScopedAStatus injectPsdsData(PsdsType psdsType, const std::vector<uint8_t>& psdsData) override;
    ndk::ScopedAStatus setCallback(const std::shared_ptr<IGnssPsdsCallback>& callback) override;
    void requestDownload(const char* why);   // IGnssPsdsCallback::downloadRequestCb(LONG_TERM)

  private:
    Injector mInject;
    std::mutex mMutex;
    std::shared_ptr<IGnssPsdsCallback> mCallback;
};

class Gnss : public BnGnss, public ::a6l::EngineListener {
  public:
    Gnss();
    ~Gnss() override;

    ndk::ScopedAStatus setCallback(const std::shared_ptr<IGnssCallback>& callback) override;
    ndk::ScopedAStatus close() override;
    ndk::ScopedAStatus getExtensionPsds(std::shared_ptr<IGnssPsds>* out) override;
    ndk::ScopedAStatus getExtensionGnssConfiguration(std::shared_ptr<IGnssConfiguration>* out) override;
    ndk::ScopedAStatus getExtensionGnssMeasurement(std::shared_ptr<IGnssMeasurementInterface>* out) override;
    ndk::ScopedAStatus getExtensionGnssPowerIndication(std::shared_ptr<IGnssPowerIndication>* out) override;
    ndk::ScopedAStatus getExtensionGnssBatching(std::shared_ptr<IGnssBatching>* out) override;
    ndk::ScopedAStatus getExtensionGnssGeofence(std::shared_ptr<IGnssGeofence>* out) override;
    ndk::ScopedAStatus getExtensionGnssNavigationMessage(
            std::shared_ptr<IGnssNavigationMessageInterface>* out) override;
    ndk::ScopedAStatus getExtensionAGnss(std::shared_ptr<IAGnss>* out) override;
    ndk::ScopedAStatus getExtensionAGnssRil(std::shared_ptr<IAGnssRil>* out) override;
    ndk::ScopedAStatus getExtensionGnssDebug(std::shared_ptr<IGnssDebug>* out) override;
    ndk::ScopedAStatus getExtensionGnssVisibilityControl(
            std::shared_ptr<visibility_control::IGnssVisibilityControl>* out) override;
    ndk::ScopedAStatus start() override;
    ndk::ScopedAStatus stop() override;
    ndk::ScopedAStatus injectTime(int64_t timeMs, int64_t timeReferenceMs, int32_t uncertaintyMs) override;
    ndk::ScopedAStatus injectLocation(const GnssLocation& location) override;
    ndk::ScopedAStatus injectBestLocation(const GnssLocation& location) override;
    ndk::ScopedAStatus deleteAidingData(IGnss::GnssAidingData aidingDataFlags) override;
    ndk::ScopedAStatus setPositionMode(const IGnss::PositionModeOptions& options) override;
    ndk::ScopedAStatus getExtensionGnssAntennaInfo(std::shared_ptr<IGnssAntennaInfo>* out) override;
    ndk::ScopedAStatus getExtensionMeasurementCorrections(
            std::shared_ptr<measurement_corrections::IMeasurementCorrectionsInterface>* out) override;
    ndk::ScopedAStatus startSvStatus() override;
    ndk::ScopedAStatus stopSvStatus() override;
    ndk::ScopedAStatus startNmea() override;
    ndk::ScopedAStatus stopNmea() override;
    ndk::ScopedAStatus getExtensionGnssAssistanceInterface(
            std::shared_ptr<gnss_assistance::IGnssAssistanceInterface>* out) override;

    // ::a6l::EngineListener (called from the engine's reader/worker threads)
    void onFix(const ::a6l::loc::Fix& f) override;
    void onSvs(const std::vector<::a6l::loc::Sv>& svs, const std::vector<uint16_t>& used) override;
    void onNmea(const std::string& s, bool synthetic) override;
    void onEngineState(bool on) override;
    void onServiceState(bool up) override;
    void onTimeRequest() override;
    void onPositionRequest() override;
    void onOrbitsRequest() override;
    void onXtraResult(bool ok, const std::string& detail) override;
    void onXtraInfo(const std::string& detail) override;

  private:
    std::shared_ptr<IGnssCallback> cb();
    void ensureEngine();

    std::mutex mMutex;
    std::shared_ptr<IGnssCallback> mCallback;
    std::unique_ptr<::a6l::GnssEngine> mEngine;
    std::shared_ptr<A6lGnssConfiguration> mConfiguration;
    std::shared_ptr<A6lGnssDebug> mDebug;
    std::shared_ptr<A6lGnssPsds> mPsds;
    std::atomic<bool> mSvStatusOn{true};
    std::atomic<bool> mNmeaOn{true};
    std::atomic<bool> mActive{false};
};

}  // namespace aidl::android::hardware::gnss
