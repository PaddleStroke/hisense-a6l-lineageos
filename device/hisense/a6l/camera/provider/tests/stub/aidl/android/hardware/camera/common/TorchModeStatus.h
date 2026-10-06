// host-test stub of the AIDL NDK enum
#pragma once
#include <cstdint>
namespace aidl::android::hardware::camera::common {
enum class TorchModeStatus : int32_t { NOT_AVAILABLE = 0, AVAILABLE_OFF = 1, AVAILABLE_ON = 2 };
}
