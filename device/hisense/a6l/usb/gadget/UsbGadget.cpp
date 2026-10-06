// SPDX-License-Identifier: Apache-2.0
// Hisense A6L USB gadget AIDL HAL (android-usb, 29 Sep 2026): IUsbGadget on the mainline configfs gadget built by
// rom/init/init.a6l.usb.rc (sys.usb.configfs=2). Replaces the unfinished hals/usb-gadget example fork (which never
// touched configfs and reported success anyway).
#define LOG_TAG "android.hardware.usb.gadget-service.a6l"

#include "UsbGadget.h"

#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/stringprintf.h>
#include <unistd.h>

namespace aidl {
namespace android {
namespace hardware {
namespace usb {
namespace gadget {

using ::a6l::usb::Controller;
using ::a6l::usb::Gadget;
using ::a6l::usb::Paths;

namespace {
void halLog(int prio, const std::string& msg) {
    if (prio >= 2)
        LOG(ERROR) << msg;
    else if (prio == 1)
        LOG(WARNING) << msg;
    else
        LOG(INFO) << msg;
}

Status toStatus(Controller::Result r) {
    switch (r) {
        case Controller::Result::kSuccess:
            return Status::SUCCESS;
        case Controller::Result::kNotSupported:
            return Status::CONFIGURATION_NOT_SUPPORTED;
        default:
            return Status::ERROR;
    }
}
}  // namespace

UsbGadget::UsbGadget() {
    mGadget = std::make_unique<Gadget>(
            Paths(), [] { return ::android::base::GetProperty("sys.usb.controller", ""); }, halLog);
    mController = std::make_unique<Controller>(mGadget.get(), halLog);
    mController->start();
}

UsbGadget::~UsbGadget() {
    mController->stop();
}

::ndk::ScopedAStatus UsbGadget::setCurrentUsbFunctions(int64_t functions,
                                                       const std::shared_ptr<IUsbGadgetCallback>& callback,
                                                       int64_t timeoutMs, int64_t transactionId) {
    if (!mTookOver) {
        // From now on the HAL owns the gadget: the pre-framework "early adb" rules of init.a6l.usb.rc stop.
        ::android::base::SetProperty("vendor.a6l.usb.hal", "1");
        mTookOver = true;
    }
    LOG(INFO) << "setCurrentUsbFunctions 0x" << std::hex << functions << std::dec << " timeout " << timeoutMs;
    const Controller::Result r = mController->setFunctions(static_cast<uint64_t>(functions), timeoutMs);
    if (r == Controller::Result::kApplyFailed) {
        // r6c: the gadget could not be written at all (r6b: EACCES on configfs). Hand it back to init's early-adb rules
        // (init.a6l.usb.rc, vendor.a6l.usb.hal=0) so adb still comes up; the next request takes over again.
        LOG(ERROR) << "setCurrentUsbFunctions: gadget not writable, handing it back to init (vendor.a6l.usb.hal=0)";
        ::android::base::SetProperty("vendor.a6l.usb.hal", "0");
        mTookOver = false;
    }
    Status st = toStatus(r);
    if (callback) {
        auto ret = callback->setCurrentUsbFunctionsCb(functions, st, transactionId);
        if (!ret.isOk()) LOG(ERROR) << "setCurrentUsbFunctionsCb: " << ret.getDescription();
    }
    if (st == Status::SUCCESS) return ::ndk::ScopedAStatus::ok();
    return ::ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(static_cast<int32_t>(st),
                                                                     "setCurrentUsbFunctions failed");
}

::ndk::ScopedAStatus UsbGadget::getCurrentUsbFunctions(const std::shared_ptr<IUsbGadgetCallback>& callback,
                                                       int64_t transactionId) {
    if (callback == nullptr) return ::ndk::ScopedAStatus::fromExceptionCode(EX_NULL_POINTER);
    auto ret = callback->getCurrentUsbFunctionsCb(
            static_cast<int64_t>(mController->currentFunctions()),
            mController->applied() ? Status::FUNCTIONS_APPLIED : Status::FUNCTIONS_NOT_APPLIED, transactionId);
    if (!ret.isOk()) LOG(ERROR) << "getCurrentUsbFunctionsCb: " << ret.getDescription();
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus UsbGadget::getUsbSpeed(const std::shared_ptr<IUsbGadgetCallback>& callback,
                                            int64_t transactionId) {
    const std::string s = mGadget->currentSpeed();
    UsbSpeed speed = UsbSpeed::UNKNOWN;
    if (s == "low-speed")
        speed = UsbSpeed::LOWSPEED;
    else if (s == "full-speed")
        speed = UsbSpeed::FULLSPEED;
    else if (s == "high-speed")
        speed = UsbSpeed::HIGHSPEED;
    else if (s == "super-speed")
        speed = UsbSpeed::SUPERSPEED;
    else if (s == "super-speed-plus")
        speed = UsbSpeed::SUPERSPEED_10Gb;
    if (callback) {
        auto ret = callback->getUsbSpeedCb(speed, transactionId);
        if (!ret.isOk()) LOG(ERROR) << "getUsbSpeedCb: " << ret.getDescription();
    }
    return ::ndk::ScopedAStatus::ok();
}

::ndk::ScopedAStatus UsbGadget::reset(const std::shared_ptr<IUsbGadgetCallback>& callback, int64_t transactionId) {
    const bool ok = mController->reset();
    if (callback) {
        auto ret = callback->resetCb(ok ? Status::SUCCESS : Status::ERROR, transactionId);
        if (!ret.isOk()) LOG(ERROR) << "resetCb: " << ret.getDescription();
    }
    if (ok) return ::ndk::ScopedAStatus::ok();
    return ::ndk::ScopedAStatus::fromServiceSpecificErrorWithMessage(-1, "reset: no applied configuration");
}

binder_status_t UsbGadget::dump(int fd, const char** /*args*/, uint32_t /*numArgs*/) {
    const std::string s = ::android::base::StringPrintf(
            "a6l usb gadget: functions=0x%llx applied=%d bound=%d udc=%s speed=%s binds=%u adopts=%u owner=%s\n",
            static_cast<unsigned long long>(mController->currentFunctions()), mController->applied(),
            mGadget->bound(), mGadget->udc().c_str(), mGadget->currentSpeed().c_str(), mController->bindCount(),
            mController->adoptCount(), mTookOver ? "hal" : "init");
    ssize_t r = write(fd, s.data(), s.size());
    (void)r;
    return STATUS_OK;
}

}  // namespace gadget
}  // namespace usb
}  // namespace hardware
}  // namespace android
}  // namespace aidl
