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

// see assert2_no_op in ResponseTracker.cpp
#define __assert2 assert2_no_op
#define __noreturn__ const
#include <aidl/android/hardware/radio/network/BnRadioNetworkResponse.h>
#undef __assert2
#undef __noreturn__
#include <cassert>

#include <libminradio/network/RadioNetworkResponseTracker.h>

#include <libminradio/debug.h>

#define RADIO_MODULE "NetworkResponse"

namespace android::hardware::radio::minimal {

using namespace ::android::hardware::radio::minimal::binder_printing;
using ::aidl::android::hardware::radio::RadioResponseInfo;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::network;

RadioNetworkResponseTracker::RadioNetworkResponseTracker(
        std::shared_ptr<aidl::IRadioNetwork> req,
        const std::shared_ptr<aidl::IRadioNetworkResponse>& resp)
    : ResponseTracker(req, resp) {}

ResponseTrackerResult<aidl::RegStateResult>
RadioNetworkResponseTracker::getDataRegistrationState() {
    auto serial = newSerial();
    if (auto status = request()->getDataRegistrationState(serial); !status.isOk()) return status;
    return getResult<aidl::RegStateResult>(serial);
}

ScopedAStatus RadioNetworkResponseTracker::getDataRegistrationStateResponse(
        const RadioResponseInfo& info, const aidl::RegStateResult& respData) {
    LOG_CALL_RESPONSE << respData;
    if (isTracked(info.serial)) return handle(info, respData);
    return IRadioNetworkResponseDelegator::getDataRegistrationStateResponse(info, respData);
}

ResponseTrackerResult<aidl::SignalStrength> RadioNetworkResponseTracker::getSignalStrength() {
    auto serial = newSerial();
    if (auto status = request()->getSignalStrength(serial); !status.isOk()) return status;
    return getResult<aidl::SignalStrength>(serial);
}

ScopedAStatus RadioNetworkResponseTracker::getSignalStrengthResponse(
        const RadioResponseInfo& info, const aidl::SignalStrength& respData) {
    LOG_CALL_RESPONSE << respData;
    if (isTracked(info.serial)) return handle(info, respData);
    return IRadioNetworkResponseDelegator::getSignalStrengthResponse(info, respData);
}

}  // namespace android::hardware::radio::minimal
