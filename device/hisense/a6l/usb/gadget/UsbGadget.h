// SPDX-License-Identifier: Apache-2.0
// Hisense A6L USB gadget AIDL HAL (android.hardware.usb.gadget V2), android-usb 29 Sep 2026.
#pragma once

#include <aidl/android/hardware/usb/gadget/BnUsbGadget.h>
#include <aidl/android/hardware/usb/gadget/IUsbGadgetCallback.h>

#include <memory>

#include "a6l_gadget_core.h"

namespace aidl {
namespace android {
namespace hardware {
namespace usb {
namespace gadget {

class UsbGadget : public BnUsbGadget {
  public:
    UsbGadget();
    ~UsbGadget() override;

    ::ndk::ScopedAStatus setCurrentUsbFunctions(int64_t functions,
                                                const std::shared_ptr<IUsbGadgetCallback>& callback,
                                                int64_t timeoutMs, int64_t transactionId) override;
    ::ndk::ScopedAStatus getCurrentUsbFunctions(const std::shared_ptr<IUsbGadgetCallback>& callback,
                                                int64_t transactionId) override;
    ::ndk::ScopedAStatus getUsbSpeed(const std::shared_ptr<IUsbGadgetCallback>& callback,
                                     int64_t transactionId) override;
    ::ndk::ScopedAStatus reset(const std::shared_ptr<IUsbGadgetCallback>& callback,
                               int64_t transactionId) override;
    binder_status_t dump(int fd, const char** args, uint32_t numArgs) override;

  private:
    std::unique_ptr<::a6l::usb::Gadget> mGadget;
    std::unique_ptr<::a6l::usb::Controller> mController;
    bool mTookOver = false;
};

}  // namespace gadget
}  // namespace usb
}  // namespace hardware
}  // namespace android
}  // namespace aidl
