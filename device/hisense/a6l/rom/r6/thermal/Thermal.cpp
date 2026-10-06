// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "thermal-a6l"

#include "Thermal.h"

#include <android-base/logging.h>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace aidl::android::hardware::thermal::impl::a6l {

using ndk::ScopedAStatus;
namespace th = ::a6l::thermal;

namespace {
constexpr float kHysteresis = 2.0f;                 // degC below a threshold before a severity is left
constexpr auto kPollInterval = std::chrono::seconds(5);
constexpr int kRescanPolls = 12;                    // re-resolve sensors / cooling devices every poll for the first ~60 s,
                                                    // then once a minute (thermal follow-up 29 Sep: the misc module group with
                                                    // qcom-spmi-adc5 = skin quiet_therm may load after the first minute)

bool sameBinder(const std::shared_ptr<::ndk::ICInterface>& a, const std::shared_ptr<::ndk::ICInterface>& b) {
    if (a == nullptr || b == nullptr || !a->isRemote() || !b->isRemote()) return a == b;
    return a->asBinder() == b->asBinder();
}
ScopedAStatus illegal(const char* m) { return ScopedAStatus::fromExceptionCodeWithMessage(EX_ILLEGAL_ARGUMENT, m); }
}  // namespace

Thermal::Thermal(std::vector<th::Sensor> sensors, std::string sysRoot)
    : sysRoot_(std::move(sysRoot)), sensors_(std::move(sensors)), severity_(sensors_.size(), 0) {
    for (const auto& s : sensors_)
        LOG(INFO) << "sensor " << s.name << " type " << s.type << " -> " << (s.path.empty() ? "(absent)" : s.path);
    cooling_ = th::scanCooling(sysRoot_);
    coolState_.assign(cooling_.size(), -1);
    for (const auto& c : cooling_) LOG(INFO) << "cooling " << c.name << " type " << c.type << " max " << c.maxState;
    poller_ = std::thread([this] { pollLoop(); });
}

Thermal::~Thermal() {
    {
        std::lock_guard<std::mutex> l(lock_);
        stop_ = true;
    }
    cv_.notify_all();
    if (poller_.joinable()) poller_.join();
}

Temperature Thermal::readOne(size_t i) {
    const th::Sensor& s = sensors_[i];
    std::string path;
    float scale;
    {
        std::lock_guard<std::mutex> l(lock_);
        path = s.path;
        scale = s.resolvedScale;
    }
    float t = NAN;
    if (!path.empty() && !th::readTemp(path, scale, &t)) t = NAN;
    std::lock_guard<std::mutex> l(lock_);
    int sev = th::severityFor(t, s.hot, severity_[i], kHysteresis);
    return Temperature{.type = static_cast<TemperatureType>(s.type),
                       .name = s.name,
                       .value = t,
                       .throttlingStatus = static_cast<ThrottlingSeverity>(sev)};
}

void Thermal::readAll(std::vector<Temperature>* out, bool filter, TemperatureType type) {
    out->clear();
    for (size_t i = 0; i < sensors_.size(); ++i) {
        if (filter && static_cast<TemperatureType>(sensors_[i].type) != type) continue;
        Temperature t = readOne(i);  // absent sensor -> NAN -> skipped
        if (std::isfinite(t.value)) out->push_back(t);
    }
}

void Thermal::rescan() {
    std::vector<th::Sensor> fresh;
    {
        std::lock_guard<std::mutex> l(lock_);
        fresh = sensors_;
    }
    th::resolve(&fresh, th::scanZones(sysRoot_), th::scanIio(sysRoot_));
    std::vector<th::Cooling> cool = th::scanCooling(sysRoot_);
    std::lock_guard<std::mutex> l(lock_);
    for (size_t i = 0; i < sensors_.size(); ++i) {
        if (fresh[i].path == sensors_[i].path) continue;
        LOG(INFO) << "sensor " << sensors_[i].name << " -> " << (fresh[i].path.empty() ? "(absent)" : fresh[i].path);
        sensors_[i].path = fresh[i].path;
        sensors_[i].resolvedScale = fresh[i].resolvedScale;
    }
    bool same = cool.size() == cooling_.size();
    for (size_t i = 0; same && i < cool.size(); ++i) same = cool[i].curPath == cooling_[i].curPath;
    if (same) return;
    for (const auto& c : cool) LOG(INFO) << "cooling " << c.name << " type " << c.type << " max " << c.maxState;
    cooling_ = std::move(cool);
    coolState_.assign(cooling_.size(), -1);
}

void Thermal::pollLoop() {
    int polls = 0;
    std::unique_lock<std::mutex> l(lock_);
    while (!stop_) {
        l.unlock();
        if (polls > 0 && (polls <= kRescanPolls || polls % kRescanPolls == 0)) rescan();
        ++polls;
        std::vector<std::pair<Temperature, std::vector<std::shared_ptr<IThermalChangedCallback>>>> notify;
        for (size_t i = 0; i < sensors_.size(); ++i) {
            Temperature t = readOne(i);
            if (!std::isfinite(t.value)) continue;
            std::lock_guard<std::mutex> g(lock_);
            int sev = static_cast<int>(t.throttlingStatus);
            if (sev == severity_[i]) continue;
            LOG(INFO) << t.name << " " << t.value << " C: severity " << severity_[i] << " -> " << sev;
            severity_[i] = sev;
            std::vector<std::shared_ptr<IThermalChangedCallback>> cbs;
            for (const Cb& c : callbacks_)
                if (!c.filtered || c.type == t.type) cbs.push_back(c.cb);
            notify.emplace_back(t, std::move(cbs));
        }
        // cooling devices: notify cur_state changes (first read only records the state)
        std::vector<std::pair<CoolingDevice, std::vector<std::shared_ptr<ICoolingDeviceChangedCallback>>>> cnotify;
        {
            std::lock_guard<std::mutex> g(lock_);
            for (size_t i = 0; i < cooling_.size(); ++i) {
                long v;
                if (!th::readState(cooling_[i].curPath, &v)) continue;
                long prev = coolState_[i];
                coolState_[i] = v;
                if (prev < 0 || prev == v) continue;
                LOG(INFO) << "cooling " << cooling_[i].name << " state " << prev << " -> " << v;
                CoolingDevice d{.type = static_cast<CoolingType>(cooling_[i].type), .name = cooling_[i].name, .value = v};
                std::vector<std::shared_ptr<ICoolingDeviceChangedCallback>> cbs;
                for (const CoolCb& c : coolCbs_)
                    if (c.type == d.type) cbs.push_back(c.cb);
                if (!cbs.empty()) cnotify.emplace_back(d, std::move(cbs));
            }
        }
        for (auto& [t, cbs] : notify)  // outside the lock: a callback may call back into the HAL
            for (auto& cb : cbs)
                if (!cb->notifyThrottling(t).isOk()) LOG(WARNING) << "notifyThrottling failed for " << t.name;
        for (auto& [d, cbs] : cnotify)
            for (auto& cb : cbs)
                if (!cb->notifyCoolingDeviceChanged(d).isOk()) LOG(WARNING) << "notifyCoolingDeviceChanged failed";
        l.lock();
        cv_.wait_for(l, kPollInterval, [this] { return stop_; });
    }
}

void Thermal::readCooling(std::vector<CoolingDevice>* out, bool filter, CoolingType type) {
    out->clear();
    std::lock_guard<std::mutex> l(lock_);
    for (const auto& c : cooling_) {
        if (filter && static_cast<CoolingType>(c.type) != type) continue;
        long v;
        if (!th::readState(c.curPath, &v)) continue;
        out->push_back(CoolingDevice{.type = static_cast<CoolingType>(c.type), .name = c.name, .value = v});
    }
}

ScopedAStatus Thermal::getCoolingDevices(std::vector<CoolingDevice>* out) {
    readCooling(out, false, CoolingType::CPU);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getCoolingDevicesWithType(CoolingType type, std::vector<CoolingDevice>* out) {
    readCooling(out, true, type);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatures(std::vector<Temperature>* out) {
    readAll(out, false, TemperatureType::UNKNOWN);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperaturesWithType(TemperatureType type, std::vector<Temperature>* out) {
    readAll(out, true, type);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatureThresholds(std::vector<TemperatureThreshold>* out) {
    out->clear();
    std::lock_guard<std::mutex> l(lock_);
    for (const auto& s : sensors_) {
        if (s.path.empty()) continue;
        out->push_back(TemperatureThreshold{.type = static_cast<TemperatureType>(s.type),
                                            .name = s.name,
                                            .hotThrottlingThresholds = std::vector<float>(s.hot, s.hot + th::kSeverities),
                                            .coldThrottlingThresholds = std::vector<float>(th::kSeverities, NAN)});
    }
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::getTemperatureThresholdsWithType(TemperatureType type, std::vector<TemperatureThreshold>* out) {
    std::vector<TemperatureThreshold> all;
    getTemperatureThresholds(&all);
    out->clear();
    for (auto& t : all)
        if (t.type == type) out->push_back(t);
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerThermalChangedCallback(const std::shared_ptr<IThermalChangedCallback>& cb) {
    if (cb == nullptr) return illegal("Invalid nullptr callback");
    std::lock_guard<std::mutex> l(lock_);
    for (const Cb& c : callbacks_)
        if (sameBinder(c.cb, cb)) return illegal("Callback already registered");
    callbacks_.push_back({cb, false, TemperatureType::UNKNOWN});
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerThermalChangedCallbackWithType(const std::shared_ptr<IThermalChangedCallback>& cb,
                                                              TemperatureType type) {
    if (cb == nullptr) return illegal("Invalid nullptr callback");
    std::lock_guard<std::mutex> l(lock_);
    for (const Cb& c : callbacks_)
        if (sameBinder(c.cb, cb)) return illegal("Callback already registered");
    callbacks_.push_back({cb, true, type});
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::unregisterThermalChangedCallback(const std::shared_ptr<IThermalChangedCallback>& cb) {
    if (cb == nullptr) return illegal("Invalid nullptr callback");
    std::lock_guard<std::mutex> l(lock_);
    auto it = std::remove_if(callbacks_.begin(), callbacks_.end(), [&](const Cb& c) { return sameBinder(c.cb, cb); });
    if (it == callbacks_.end()) return illegal("Callback wasn't registered");
    callbacks_.erase(it, callbacks_.end());
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::registerCoolingDeviceChangedCallbackWithType(
        const std::shared_ptr<ICoolingDeviceChangedCallback>& cb, CoolingType type) {
    if (cb == nullptr) return illegal("Invalid nullptr callback");
    std::lock_guard<std::mutex> l(lock_);
    for (const CoolCb& c : coolCbs_)
        if (sameBinder(c.cb, cb)) return illegal("Callback already registered");
    coolCbs_.push_back({cb, type});
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::unregisterCoolingDeviceChangedCallback(const std::shared_ptr<ICoolingDeviceChangedCallback>& cb) {
    if (cb == nullptr) return illegal("Invalid nullptr callback");
    std::lock_guard<std::mutex> l(lock_);
    auto it = std::remove_if(coolCbs_.begin(), coolCbs_.end(), [&](const CoolCb& c) { return sameBinder(c.cb, cb); });
    if (it == coolCbs_.end()) return illegal("Callback wasn't registered");
    coolCbs_.erase(it, coolCbs_.end());
    return ScopedAStatus::ok();
}

ScopedAStatus Thermal::forecastSkinTemperature(int32_t, float*) {
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace aidl::android::hardware::thermal::impl::a6l
