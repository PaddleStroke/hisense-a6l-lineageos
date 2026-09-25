// SPDX-License-Identifier: Apache-2.0
// A6L AIDL GNSS HAL service entry (agent gnss, 24 Sep 2026).
#define LOG_TAG "A6lGnss"

#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <log/log.h>

#include "Gnss.h"

using aidl::android::hardware::gnss::Gnss;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    ABinderProcess_startThreadPool();
    std::shared_ptr<Gnss> gnss = ndk::SharedRefBase::make<Gnss>();
    const std::string instance = std::string() + Gnss::descriptor + "/default";
    binder_status_t status = AServiceManager_addService(gnss->asBinder().get(), instance.c_str());
    if (status != STATUS_OK) {
        ALOGE("addService %s failed: %d", instance.c_str(), status);
        return 1;
    }
    ALOGI("registered %s", instance.c_str());
    ABinderProcess_joinThreadPool();
    return 1;
}
