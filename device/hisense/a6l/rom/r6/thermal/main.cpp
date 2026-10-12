// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "thermal-a6l"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <algorithm>

#include "Thermal.h"

using aidl::android::hardware::thermal::impl::a6l::Thermal;

int main() {
    constexpr const char* kConfig = "/vendor/etc/thermal-a6l.conf";
    std::string text, err;
    std::vector<a6l::thermal::Sensor> sensors;
    if (!android::base::ReadFileToString(kConfig, &text) || !a6l::thermal::parseConfig(text, &sensors, &err)) {
        LOG(ERROR) << kConfig << ": " << (err.empty() ? "unreadable" : err) << "; serving no sensors";
        sensors.clear();
    }
    a6l::thermal::resolve(&sensors, a6l::thermal::scanZones("/sys"), a6l::thermal::scanIio("/sys"));

    // thermal-20261012: stock-like skin mitigation (thermal-a6l-mitigation.conf); persist.vendor.a6l.thermal.mitigation=0 = off
    constexpr const char* kMitConfig = "/vendor/etc/thermal-a6l-mitigation.conf";
    std::vector<a6l::thermal::MitigationRule> rules;
    err.clear();
    if (!android::base::GetBoolProperty("persist.vendor.a6l.thermal.mitigation", true)) {
        LOG(WARNING) << "skin mitigation disabled (persist.vendor.a6l.thermal.mitigation=0)";
    } else if (!android::base::ReadFileToString(kMitConfig, &text) || !a6l::thermal::parseMitigation(text, &rules, &err)) {
        LOG(ERROR) << kMitConfig << ": " << (err.empty() ? "unreadable" : err) << "; no skin mitigation";
        rules.clear();
    } else {
        // a rule whose sensor is not in thermal-a6l.conf would never fire: refuse the file rather than half-apply it
        for (const auto& r : rules) {
            if (std::none_of(sensors.begin(), sensors.end(), [&](const auto& s) { return s.name == r.sensor; })) {
                LOG(ERROR) << kMitConfig << ": rule " << r.name << " uses unknown sensor " << r.sensor
                           << "; no skin mitigation";
                rules.clear();
                break;
            }
        }
    }

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Thermal> thermal = ndk::SharedRefBase::make<Thermal>(std::move(sensors), std::move(rules));
    const std::string instance = std::string() + Thermal::descriptor + "/default";
    CHECK(AServiceManager_addService(thermal->asBinder().get(), instance.c_str()) == STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
