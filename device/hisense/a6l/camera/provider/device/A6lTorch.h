/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A6L (lc2, 29 Sep 2026): torch with strength levels for the libcamera HAL, which has no torch at all.
 * The rear LED is the PM660L flash module (leds-qcom-flash, LED class device "white:flash"):
 *   - brightness 0..max_brightness (LED_FULL = 255) maps linearly onto 0..led-max-microamp (DT); torch current step 5 mA;
 *   - Android strength levels are 5 mA steps: MAXIMUM_LEVEL = led-max-microamp / 5 mA (100 at the stock 500 mA),
 *     DEFAULT_LEVEL = 100 mA (20), clamped to the maximum;
 *   - led-max-microamp is read from the LED's DT node (<led>/device/of_node/<child>/led-max-microamp), 100 mA fallback.
 * The torch belongs to ONE camera: the first BACK camera the HAL lists (chosen by the provider). While that camera has an
 * open session the LED is off and setTorchMode answers CAMERA_IN_USE (cameraserver reports NOT_AVAILABLE itself).
 */

#pragma once

#include <aidl/android/hardware/camera/common/Status.h>
#include <aidl/android/hardware/camera/common/TorchModeStatus.h>
#include <system/camera_metadata.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace android {
namespace hardware {
namespace camera {
namespace device {
namespace implementation {

class CameraDeviceSession;

class A6lTorch {
  public:
    using Status = ::aidl::android::hardware::camera::common::Status;
    using TorchModeStatus = ::aidl::android::hardware::camera::common::TorchModeStatus;
    using Notifier = std::function<void(const std::string& cameraId, TorchModeStatus status)>;

    static constexpr int kStepUa = 5000;           // leds-qcom-flash TORCH_IRES_UA
    static constexpr int kDefaultUa = 100000;      // default torch current (QS tile tap)
    static constexpr int kFallbackMaxUa = 100000;  // a6l-flash-v75.dtso first-test value
    static constexpr int kKernelMaxUa = 500000;    // leds-qcom-flash TORCH_CURRENT_MAX_UA (stock torch max)

    static A6lTorch& instance();

    // provider side
    void setCameraId(const std::string& cameraId);  // "" = no torch camera
    void setNotifier(Notifier notifier);

    // device side
    bool isTorchCamera(const std::string& cameraId);
    bool available();  // LED present
    int maxLevel();
    int defaultLevel();
    Status setTorchMode(const std::string& cameraId, bool on);
    Status turnOnWithStrengthLevel(const std::string& cameraId, int32_t level);
    Status getStrengthLevel(const std::string& cameraId, int32_t* level);
    // the torch camera was opened: LED off (no callback: cameraserver reports NOT_AVAILABLE itself)
    void cameraOpened(const std::string& cameraId, const std::shared_ptr<CameraDeviceSession>& session);

    // clone of src + FLASH_INFO_AVAILABLE / STRENGTH_MAXIMUM_LEVEL / STRENGTH_DEFAULT_LEVEL (+ characteristics keys);
    // nullptr when src is to be used unchanged. The caller frees the result with free_camera_metadata().
    camera_metadata_t* patchCharacteristics(const std::string& cameraId, const camera_metadata_t* src);

    // pure helpers (host-testable)
    static int levelsForMaxUa(int maxUa);
    static int defaultLevelFor(int maxLevel);
    static int brightnessForLevel(int level, int maxUa, int maxBrightness);

  private:
    A6lTorch() = default;
    void probeLocked();
    bool inUseLocked();
    Status applyLocked(int level);  // level 0 = off
    void notify(const std::string& cameraId, TorchModeStatus status);

    std::mutex mLock;
    std::mutex mNotifyLock;
    std::string mCameraId;
    std::string mLed;
    bool mProbed = false;
    bool mPresent = false;
    int mMaxUa = kFallbackMaxUa;
    int mMaxBrightness = 255;
    int mLevel = 0;  // current level, 0 = off
    std::weak_ptr<CameraDeviceSession> mSession;
    Notifier mNotifier;
};

}  // namespace implementation
}  // namespace device
}  // namespace camera
}  // namespace hardware
}  // namespace android
