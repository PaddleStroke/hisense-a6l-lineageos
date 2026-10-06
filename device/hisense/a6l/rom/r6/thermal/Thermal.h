// SPDX-License-Identifier: Apache-2.0
// A6L thermal HAL (AIDL android.hardware.thermal V3): real tsens / PMIC / thermistor / battery temperatures from sysfs
// (thermal_logic), a 5 s poller that notifies IThermalChangedCallback on severity changes. thermal-r5prep (29 Sep 2026):
// kernel cooling devices are reported read-only (cooling_logic) with ICoolingDeviceChangedCallback on cur_state changes;
// sensors and cooling devices are re-scanned during the first minute (modules such as qcom-spmi-adc5 / msm.ko may load
// after the HAL starts). The kernel's DT cooling maps stay the only throttling actors.
#pragma once

#include <aidl/android/hardware/thermal/BnThermal.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "cooling_logic.h"
#include "thermal_logic.h"

namespace aidl::android::hardware::thermal::impl::a6l {

class Thermal : public BnThermal {
  public:
    explicit Thermal(std::vector<::a6l::thermal::Sensor> sensors, std::string sysRoot = "/sys");
    ~Thermal();

    ndk::ScopedAStatus getCoolingDevices(std::vector<CoolingDevice>* out) override;
    ndk::ScopedAStatus getCoolingDevicesWithType(CoolingType type, std::vector<CoolingDevice>* out) override;
    ndk::ScopedAStatus getTemperatures(std::vector<Temperature>* out) override;
    ndk::ScopedAStatus getTemperaturesWithType(TemperatureType type, std::vector<Temperature>* out) override;
    ndk::ScopedAStatus getTemperatureThresholds(std::vector<TemperatureThreshold>* out) override;
    ndk::ScopedAStatus getTemperatureThresholdsWithType(TemperatureType type,
                                                        std::vector<TemperatureThreshold>* out) override;
    ndk::ScopedAStatus registerThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& cb) override;
    ndk::ScopedAStatus registerThermalChangedCallbackWithType(
            const std::shared_ptr<IThermalChangedCallback>& cb, TemperatureType type) override;
    ndk::ScopedAStatus unregisterThermalChangedCallback(
            const std::shared_ptr<IThermalChangedCallback>& cb) override;
    ndk::ScopedAStatus registerCoolingDeviceChangedCallbackWithType(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& cb, CoolingType type) override;
    ndk::ScopedAStatus unregisterCoolingDeviceChangedCallback(
            const std::shared_ptr<ICoolingDeviceChangedCallback>& cb) override;
    ndk::ScopedAStatus forecastSkinTemperature(int32_t forecastSeconds, float* out) override;

  private:
    struct Cb {
        std::shared_ptr<IThermalChangedCallback> cb;
        bool filtered;
        TemperatureType type;
    };
    struct CoolCb {
        std::shared_ptr<ICoolingDeviceChangedCallback> cb;
        CoolingType type;
    };
    void readAll(std::vector<Temperature>* out, bool filter, TemperatureType type);
    Temperature readOne(size_t i);
    void readCooling(std::vector<CoolingDevice>* out, bool filter, CoolingType type);
    void rescan();
    void pollLoop();

    const std::string sysRoot_;
    std::vector<::a6l::thermal::Sensor> sensors_;  // name/type/thresholds fixed; path/resolvedScale guarded by lock_
    std::mutex lock_;                 // guards severity_, callbacks_, stop_, sensor paths, cooling_, coolState_, coolCbs_
    std::vector<int> severity_;       // last notified severity per sensor
    std::vector<Cb> callbacks_;
    std::vector<::a6l::thermal::Cooling> cooling_;
    std::vector<long> coolState_;     // last notified cur_state per cooling device (-1 = never read)
    std::vector<CoolCb> coolCbs_;
    bool stop_ = false;
    std::condition_variable cv_;
    std::thread poller_;
};

}  // namespace aidl::android::hardware::thermal::impl::a6l
