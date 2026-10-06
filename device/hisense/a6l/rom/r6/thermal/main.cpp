// SPDX-License-Identifier: Apache-2.0
#define LOG_TAG "thermal-a6l"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

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

    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<Thermal> thermal = ndk::SharedRefBase::make<Thermal>(std::move(sensors));
    const std::string instance = std::string() + Thermal::descriptor + "/default";
    CHECK(AServiceManager_addService(thermal->asBinder().get(), instance.c_str()) == STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
