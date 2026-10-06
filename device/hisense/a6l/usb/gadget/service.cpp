// SPDX-License-Identifier: Apache-2.0
// Hisense A6L USB gadget AIDL HAL service (android-usb, 29 Sep 2026).
#define LOG_TAG "android.hardware.usb.gadget-service.a6l"

#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "UsbGadget.h"

using ::aidl::android::hardware::usb::gadget::UsbGadget;

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    std::shared_ptr<UsbGadget> gadget = ndk::SharedRefBase::make<UsbGadget>();
    const std::string instance = std::string() + UsbGadget::descriptor + "/default";
    binder_status_t status = AServiceManager_addService(gadget->asBinder().get(), instance.c_str());
    CHECK_EQ(status, STATUS_OK) << "cannot register " << instance;
    LOG(INFO) << "A6L USB gadget HAL started";
    ABinderProcess_joinThreadPool();
    return -1;  // unreachable
}
