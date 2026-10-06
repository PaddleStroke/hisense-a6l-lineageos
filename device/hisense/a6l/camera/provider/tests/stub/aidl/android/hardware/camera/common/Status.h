// host-test stub of the AIDL NDK enum (values as in android.hardware.camera.common Status.aidl)
#pragma once
#include <cstdint>
namespace aidl::android::hardware::camera::common {
enum class Status : int32_t { OK = 0, ILLEGAL_ARGUMENT = 1, CAMERA_IN_USE = 2, MAX_CAMERAS_IN_USE = 3,
    OPERATION_NOT_SUPPORTED = 4, CAMERA_DISCONNECTED = 5, INTERNAL_ERROR = 6 };
}
