/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A6L (lc2, 29 Sep 2026): torch with strength levels on /sys/class/leds/white:flash. See A6lTorch.h.
 */

#define LOG_TAG "A6lTorch"

#include "A6lTorch.h"

#ifndef A6L_TORCH_HOST_TEST
#include "CameraDeviceSession.h"

#include <CameraMetadata.h>
#include <log/log.h>
#else
static inline void a6lNoLog(const char*, ...) {}
#define ALOGI(...) a6lNoLog(__VA_ARGS__)
#define ALOGW(...) a6lNoLog(__VA_ARGS__)
#define ALOGE(...) a6lNoLog(__VA_ARGS__)
#endif

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

namespace android {
namespace hardware {
namespace camera {
namespace device {
namespace implementation {

namespace {

const char* const kLedCandidates[] = {
        "/sys/class/leds/white:flash",  // leds-qcom-flash, function=flash color=white (a6l-flash-v75.dtso)
        "/sys/class/leds/white:torch",
        "/sys/class/leds/led:torch_0",
};

bool readText(const std::string& path, std::string* out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buf[64];
    ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
    ::close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';
    *out = buf;
    return true;
}

bool readInt(const std::string& path, int* out) {
    std::string s;
    if (!readText(path, &s)) return false;
    errno = 0;
    char* end = nullptr;
    long v = strtol(s.c_str(), &end, 10);
    if (errno || end == s.c_str()) return false;
    *out = static_cast<int>(v);
    return true;
}

// DT property cell: one big-endian u32
bool readDtU32(const std::string& path, int* out) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    uint8_t b[4];
    ssize_t n = ::read(fd, b, sizeof(b));
    ::close(fd);
    if (n != 4) return false;
    *out = static_cast<int>((uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3]);
    return true;
}

bool writeText(const std::string& path, const std::string& value) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        ALOGE("open %s: %s", path.c_str(), strerror(errno));
        return false;
    }
    ssize_t n = ::write(fd, value.c_str(), value.size());
    int err = errno;
    ::close(fd);
    if (n != static_cast<ssize_t>(value.size())) {
        ALOGE("write %s=%s: %s", path.c_str(), value.c_str(), strerror(err));
        return false;
    }
    return true;
}

}  // namespace

A6lTorch& A6lTorch::instance() {
    static A6lTorch sInstance;
    return sInstance;
}

int A6lTorch::levelsForMaxUa(int maxUa) {
    maxUa = std::clamp(maxUa, kStepUa, kKernelMaxUa);
    return std::max(1, maxUa / kStepUa);
}

int A6lTorch::defaultLevelFor(int maxLevel) {
    return std::clamp(kDefaultUa / kStepUa, 1, std::max(1, maxLevel));
}

int A6lTorch::brightnessForLevel(int level, int maxUa, int maxBrightness) {
    if (level <= 0 || maxUa <= 0 || maxBrightness <= 0) return 0;
    // kernel: current = brightness * led-max-microamp / LED_FULL (rounded down) -> round the brightness UP so a level
    // never lands one 5 mA step below its nominal current
    long long ua = static_cast<long long>(level) * kStepUa;
    long long b = (ua * maxBrightness + maxUa - 1) / maxUa;
    return static_cast<int>(std::clamp<long long>(b, 1, maxBrightness));
}

void A6lTorch::setCameraId(const std::string& cameraId) {
    std::lock_guard<std::mutex> l(mLock);
    mCameraId = cameraId;
    ALOGI("torch camera: '%s'", cameraId.c_str());
}

void A6lTorch::setNotifier(Notifier notifier) {
    std::lock_guard<std::mutex> l(mNotifyLock);
    mNotifier = std::move(notifier);
}

bool A6lTorch::isTorchCamera(const std::string& cameraId) {
    std::lock_guard<std::mutex> l(mLock);
    return !mCameraId.empty() && cameraId == mCameraId;
}

void A6lTorch::probeLocked() {
    if (mProbed) return;
    for (const char* led : kLedCandidates) {
        std::string b = std::string(led) + "/brightness";
        if (::access(b.c_str(), F_OK) != 0) continue;
        mLed = led;
        mPresent = true;
        int mb = 0;
        if (readInt(mLed + "/max_brightness", &mb) && mb > 0) mMaxBrightness = mb;
        // led-max-microamp of the LED's DT child node (the class device's parent is the flash controller)
        int ua = 0;
        std::string on = mLed + "/device/of_node";
        if (DIR* d = ::opendir(on.c_str())) {
            while (dirent* e = ::readdir(d)) {
                if (e->d_name[0] == '.') continue;
                if (readDtU32(on + "/" + e->d_name + "/led-max-microamp", &ua) && ua > 0) break;
                ua = 0;
            }
            ::closedir(d);
        }
        mMaxUa = ua > 0 ? std::clamp(ua, kStepUa, kKernelMaxUa) : kFallbackMaxUa;
        ALOGI("torch LED %s: max_brightness %d, led-max-microamp %d%s -> %d levels, default %d", mLed.c_str(),
              mMaxBrightness, mMaxUa, ua > 0 ? "" : " (DT not readable: fallback)", levelsForMaxUa(mMaxUa),
              defaultLevelFor(levelsForMaxUa(mMaxUa)));
        break;
    }
    // not present yet (leds-qcom-flash loads with the misc module group): probe again next time
    mProbed = mPresent;
}

bool A6lTorch::available() {
    std::lock_guard<std::mutex> l(mLock);
    probeLocked();
    return mPresent;
}

int A6lTorch::maxLevel() {
    std::lock_guard<std::mutex> l(mLock);
    probeLocked();
    return levelsForMaxUa(mMaxUa);
}

int A6lTorch::defaultLevel() {
    return defaultLevelFor(maxLevel());
}

bool A6lTorch::inUseLocked() {
#ifndef A6L_TORCH_HOST_TEST
    std::shared_ptr<CameraDeviceSession> s = mSession.lock();
    return s != nullptr && !s->isClosed();
#else
    return false;
#endif
}

A6lTorch::Status A6lTorch::applyLocked(int level) {
    int b = brightnessForLevel(level, mMaxUa, mMaxBrightness);
    if (!writeText(mLed + "/brightness", std::to_string(b))) return Status::INTERNAL_ERROR;
    mLevel = level;
    return Status::OK;
}

void A6lTorch::notify(const std::string& cameraId, TorchModeStatus status) {
    Notifier n;
    {
        std::lock_guard<std::mutex> l(mNotifyLock);
        n = mNotifier;
    }
    if (n) n(cameraId, status);
}

A6lTorch::Status A6lTorch::setTorchMode(const std::string& cameraId, bool on) {
    Status st;
    {
        std::lock_guard<std::mutex> l(mLock);
        if (mCameraId.empty() || cameraId != mCameraId) return Status::OPERATION_NOT_SUPPORTED;
        probeLocked();
        if (!mPresent) return Status::OPERATION_NOT_SUPPORTED;
        if (inUseLocked()) return Status::CAMERA_IN_USE;
        // setTorchMode(true) = the default level; turning off resets the level to the default (CameraManager contract)
        st = applyLocked(on ? defaultLevelFor(levelsForMaxUa(mMaxUa)) : 0);
    }
    if (st == Status::OK) notify(cameraId, on ? TorchModeStatus::AVAILABLE_ON : TorchModeStatus::AVAILABLE_OFF);
    return st;
}

A6lTorch::Status A6lTorch::turnOnWithStrengthLevel(const std::string& cameraId, int32_t level) {
    Status st;
    {
        std::lock_guard<std::mutex> l(mLock);
        if (mCameraId.empty() || cameraId != mCameraId) return Status::OPERATION_NOT_SUPPORTED;
        probeLocked();
        if (!mPresent) return Status::OPERATION_NOT_SUPPORTED;
        if (level < 1 || level > levelsForMaxUa(mMaxUa)) return Status::ILLEGAL_ARGUMENT;
        if (inUseLocked()) return Status::CAMERA_IN_USE;
        st = applyLocked(level);
    }
    if (st == Status::OK) notify(cameraId, TorchModeStatus::AVAILABLE_ON);
    return st;
}

A6lTorch::Status A6lTorch::getStrengthLevel(const std::string& cameraId, int32_t* level) {
    std::lock_guard<std::mutex> l(mLock);
    if (mCameraId.empty() || cameraId != mCameraId) return Status::OPERATION_NOT_SUPPORTED;
    probeLocked();
    if (!mPresent) return Status::OPERATION_NOT_SUPPORTED;
    *level = mLevel > 0 ? mLevel : defaultLevelFor(levelsForMaxUa(mMaxUa));
    return Status::OK;
}

void A6lTorch::cameraOpened(const std::string& cameraId,
                            const std::shared_ptr<CameraDeviceSession>& session) {
    std::lock_guard<std::mutex> l(mLock);
    if (mCameraId.empty() || cameraId != mCameraId) return;
    mSession = session;
    if (mPresent && mLevel > 0) {
        ALOGI("camera %s opened: torch off", cameraId.c_str());
        applyLocked(0);
    }
}

#ifndef A6L_TORCH_HOST_TEST
camera_metadata_t* A6lTorch::patchCharacteristics(const std::string& cameraId,
                                                  const camera_metadata_t* src) {
    if (src == nullptr || !isTorchCamera(cameraId) || !available()) return nullptr;
    const int32_t maxL = maxLevel();
    const int32_t defL = defaultLevelFor(maxL);
    ::android::hardware::camera::common::helper::CameraMetadata md(clone_camera_metadata(src));
    const uint8_t flash = ANDROID_FLASH_INFO_AVAILABLE_TRUE;
    md.update(ANDROID_FLASH_INFO_AVAILABLE, &flash, 1);
    md.update(ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL, &maxL, 1);
    md.update(ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL, &defL, 1);
    std::vector<int32_t> keys;
    camera_metadata_entry e = md.find(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS);
    for (size_t i = 0; i < e.count; i++) keys.push_back(e.data.i32[i]);
    for (int32_t k : {ANDROID_FLASH_INFO_AVAILABLE, ANDROID_FLASH_INFO_STRENGTH_MAXIMUM_LEVEL,
                      ANDROID_FLASH_INFO_STRENGTH_DEFAULT_LEVEL}) {
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) keys.push_back(k);
    }
    md.update(ANDROID_REQUEST_AVAILABLE_CHARACTERISTICS_KEYS, keys.data(), keys.size());
    return md.release();
}
#endif

}  // namespace implementation
}  // namespace device
}  // namespace camera
}  // namespace hardware
}  // namespace android
