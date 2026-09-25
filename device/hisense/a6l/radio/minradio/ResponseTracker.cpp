/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <libminradio/ResponseTracker.h>

#include <libminradio/debug.h>

#include <random>

namespace android::hardware::radio::minimal {

using namespace ::android::hardware::radio::minimal::binder_printing;
using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioResponseInfo;
using ::ndk::ScopedAStatus;

RadioError ResponseTrackerResultBase::toError(const ScopedAStatus& status) {
    CHECK(!status.isOk()) << "statusToError called with no error";
    return RadioError::GENERIC_FAILURE;
}

ResponseTrackerResultBase::ResponseTrackerResultBase(const char* descriptor)
    : ResponseTrackerResultBase(descriptor, RadioError::RADIO_NOT_AVAILABLE) {}

ResponseTrackerResultBase::ResponseTrackerResultBase(const char* descriptor, RadioError error)
    : mDescriptor(descriptor), mError(error) {}

ResponseTrackerResultBase::ResponseTrackerResultBase(const char* descriptor, ScopedAStatus st)
    : ResponseTrackerResultBase(descriptor, toError(st)) {}

bool ResponseTrackerResultBase::isOk() const {
    return mError == RadioError::NONE;
}

bool ResponseTrackerResultBase::expectOk() const {
    if (isOk()) return true;
    LOG(ERROR) << "Request for " << mDescriptor << " failed: " << mError;
    return false;
}

RadioError ResponseTrackerResultBase::getError() const {
    return mError;
}

const char* ResponseTrackerResultBase::getDescriptor() const {
    return mDescriptor;
}

ResponseTrackerBase::ScopedSerial::ScopedSerial(int32_t serial, ResponseTrackerBase* tracker)
    : mSerial(serial), mTracker(tracker) {}

ResponseTrackerBase::ScopedSerial::~ScopedSerial() {
    if (mIsReleased) return;
    mTracker->cancelTracking(*this);
}

ResponseTrackerBase::ScopedSerial::operator int32_t() const {
    CHECK(!mIsReleased) << "ScopedSerial " << mSerial << " is not valid anymore";
    return mSerial;
}

void ResponseTrackerBase::ScopedSerial::release() {
    mIsReleased = true;
}

int32_t ResponseTrackerBase::initialSerial() {
    /* Android framework tends to start request serial numbers from 0, so let's pick something from
     * the second quarter of int32_t negative range. This way the chance of having a conflict is
     * closer to zero. */
    static const int32_t rangeSize = std::abs(std::numeric_limits<int32_t>::min() / 4);
    static const int32_t rangeStart = std::numeric_limits<int32_t>::min() + rangeSize;

    static std::random_device generator;
    static std::uniform_int_distribution<int32_t> distribution(rangeStart, rangeStart + rangeSize);

    return distribution(generator);
}

ResponseTrackerBase::ScopedSerial ResponseTrackerBase::newSerial() {
    std::unique_lock lck(mSerialsGuard);

    auto serial = mSerial++;
    if (serial == 0) [[unlikely]] {
        serial = mSerial++;
    }
    if constexpr (debug::kSuperCrazyVerbose) {
        LOG(VERBOSE) << "Tracking " << serial << " internally";
    }

    auto inserted = mTrackedSerials.emplace(serial, nullptr).second;
    CHECK(inserted) << "Detected tracked serials conflict at " << serial;

    return {serial, this};
}

bool ResponseTrackerBase::isTracked(int32_t serial) const {
    std::unique_lock lck(mSerialsGuard);
    return mTrackedSerials.contains(serial);
}

void ResponseTrackerBase::cancelTracking(ResponseTrackerBase::ScopedSerial& serial) {
    std::unique_lock lck(mSerialsGuard);
    auto erased = mTrackedSerials.erase(serial);
    CHECK(erased == 1) << "Couldn't cancel tracking " << serial;
    LOG(VERBOSE) << "Cancelled tracking " << serial << " internally";
    serial.release();
}

ScopedAStatus ResponseTrackerBase::handle(const RadioResponseInfo& info,
                                          std::unique_ptr<ResponseTrackerResultBase> result) {
    std::unique_lock lck(mSerialsGuard);
    if constexpr (debug::kSuperCrazyVerbose) {
        LOG(VERBOSE) << "Handling " << info.serial << " internally (not sending to the framework)";
    }

    auto it = mTrackedSerials.find(info.serial);
    CHECK(it != mTrackedSerials.end()) << "Request not tracked: " << info;
    CHECK(it->second == nullptr) << "Request already handled: " << info;
    it->second = std::move(result);

    return ScopedAStatus::ok();
}

std::unique_ptr<ResponseTrackerResultBase> ResponseTrackerBase::getResultBase(
        ResponseTrackerBase::ScopedSerial& serial) {
    std::unique_lock lck(mSerialsGuard);
    auto node = mTrackedSerials.extract(serial);
    CHECK(node.key()) << "Request " << serial << " is not tracked";
    if (!node.mapped()) {
        LOG(WARNING) << "Didn't get result for " << serial
                     << ". It may either mean setResponseFunctions has reset the callbacks or"
                        " the callback wasn't called synchronously from the scope of "
                        "request method implementation.";
        serial.release();
        return nullptr;
    }
    if constexpr (debug::kSuperCrazyVerbose) {
        LOG(VERBOSE) << "Finished tracking " << serial << " internally";
    }
    serial.release();
    return std::move(node.mapped());
}

// This symbol silences "Mismatched versions of delegator and implementation" errors from Delegator
// implementation. In this specific case, Delegators are used to encapsulate incoming callbacks, not
// outgoing interfaces - so clamping delegator interface version to lower than implementation's
// version wouldn't make any difference - the local binary wouldn't know what to do with a newer
// interface anyways. This happens when Radio HAL (which includes callback interfaces) defined on
// system partition is newer than one used to build local binary (usually on vendor partition).
extern "C" void assert2_no_op(const char*, int, const char*, const char*) {}

}  // namespace android::hardware::radio::minimal
