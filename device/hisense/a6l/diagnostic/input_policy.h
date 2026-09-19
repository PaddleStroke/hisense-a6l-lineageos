#pragma once

// Minimal policy bridge for the private InputReader/InputDispatcher diagnostic.
#include <InputReaderBase.h>
#include <dispatcher/Entry.h>
#include <InputDispatcherPolicyInterface.h>

namespace android::a6l {

class ReaderPolicy final : public InputReaderPolicyInterface {
public:
    explicit ReaderPolicy(InputReaderConfiguration configuration) : mConfiguration(std::move(configuration)) {}
    void getReaderConfiguration(InputReaderConfiguration* out) override { *out = mConfiguration; }
    void notifyInputDevicesChanged(const std::vector<InputDeviceInfo>&) override {}
    void notifyTouchpadHardwareState(const SelfContainedHardwareState&, DeviceId) override {}
    void notifyTouchpadGestureInfo(GestureType, DeviceId) override {}
    void notifyTouchpadThreeFingerTap() override {}
    std::shared_ptr<KeyCharacterMap> getKeyboardLayoutOverlay(
            const InputDeviceIdentifier&, const std::optional<KeyboardLayoutInfo>) override { return nullptr; }
    std::string getDeviceAlias(const InputDeviceIdentifier&) override { return {}; }
    TouchAffineTransformation getTouchAffineTransformation(const std::string&, ui::Rotation) override { return {}; }
    void notifyStylusGestureStarted(DeviceId, nsecs_t) override {}
    bool isInputMethodConnectionActive() override { return false; }
    std::optional<DisplayViewport> getPointerViewportForAssociatedDisplay(
            ui::LogicalDisplayId) override { return mConfiguration.getDisplayViewportById(ui::LogicalDisplayId::DEFAULT); }
private:
    InputReaderConfiguration mConfiguration;
};

class DispatcherPolicy final : public InputDispatcherPolicyInterface {
public:
    void notifyNoFocusedWindowAnr(const std::shared_ptr<InputApplicationHandle>&, int32_t, nsecs_t,
                                  std::chrono::milliseconds) override {}
    void notifyWindowUnresponsive(const sp<IBinder>&, std::optional<gui::Pid>, const std::string&, int32_t,
                                  nsecs_t, std::chrono::milliseconds) override {}
    void notifyWindowResponsive(const sp<IBinder>&, std::optional<gui::Pid>) override {}
    void notifyPreNoFocusedWindowAnr(const std::shared_ptr<InputApplicationHandle>&, int32_t,
                                     std::chrono::milliseconds, std::chrono::milliseconds) override {}
    void notifyInputChannelBroken(const sp<IBinder>&) override {}
    void notifyFocusChanged(const sp<IBinder>&, const sp<IBinder>&) override {}
    void notifySensorEvent(DeviceId, InputDeviceSensorType, InputDeviceSensorAccuracy, nsecs_t,
                           const std::vector<float>&) override {}
    void notifySensorAccuracy(DeviceId, InputDeviceSensorType, InputDeviceSensorAccuracy) override {}
    void notifyVibratorState(DeviceId, bool) override {}
    void notifyFocusedDisplayChanged(ui::LogicalDisplayId) override {}
    bool filterInputEvent(const InputEvent&, uint32_t) override { return true; }
    void interceptKeyBeforeQueueing(const KeyEvent&, uint32_t& flags) override { flags |= POLICY_FLAG_PASS_TO_USER; }
    void interceptMotionBeforeQueueing(ui::LogicalDisplayId, uint32_t, int32_t, nsecs_t,
                                       uint32_t& flags) override { flags |= POLICY_FLAG_PASS_TO_USER; }
    std::variant<nsecs_t, inputdispatcher::KeyEntry::InterceptKeyResult>
    interceptKeyBeforeDispatching(const sp<IBinder>&, const KeyEvent&, uint32_t) override {
        return inputdispatcher::KeyEntry::InterceptKeyResult::CONTINUE;
    }
    std::optional<KeyEvent> dispatchUnhandledKey(const sp<IBinder>&, const KeyEvent&, uint32_t) override { return std::nullopt; }
    void notifySwitch(nsecs_t, uint32_t, uint32_t, uint32_t) override {}
    void pokeUserActivity(nsecs_t, int32_t, ui::LogicalDisplayId, int32_t) override {}
    void onPointerDownOutsideFocus(const sp<IBinder>&) override {}
    void setPointerCapture(const PointerCaptureRequest&) override {}
    void notifyDropWindow(const sp<IBinder>&, vec2, vec2) override {}
    void notifyDeviceInteraction(DeviceId, nsecs_t, const std::set<gui::Uid>&) override {}
};

} // namespace android::a6l
