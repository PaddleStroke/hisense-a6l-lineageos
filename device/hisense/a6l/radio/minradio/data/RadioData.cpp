/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <libminradio/data/RadioData.h>

#include <libminradio/debug.h>
#include <libminradio/response.h>

#include <ranges>

#define RADIO_MODULE "Data"

namespace android::hardware::radio::minimal {

using namespace ::android::hardware::radio::minimal::binder_printing;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::data;
constexpr auto ok = &ScopedAStatus::ok;

int32_t RadioData::setupDataCallCid() {
    return ++mLastDataCallCid;
}

void RadioData::setupDataCallBase(aidl::SetupDataCallResult dataCall) {
    {
        const std::lock_guard<std::mutex> lock(mDataCallListGuard);
        mDataCallList[dataCall.cid] = dataCall;
    }
    indicate()->dataCallListChanged(RadioIndicationType::UNSOLICITED, getDataCallListBase());
}

void RadioData::deactivateDataCallBase(int32_t cid) {
    {
        const std::lock_guard<std::mutex> lock(mDataCallListGuard);
        auto it = mDataCallList.find(cid);
        if (it == mDataCallList.end()) return;

        mDataCallList.erase(it);
    }
    indicate()->dataCallListChanged(RadioIndicationType::UNSOLICITED, getDataCallListBase());
}

std::vector<aidl::SetupDataCallResult> RadioData::getDataCallListBase() const {
    const std::lock_guard<std::mutex> lock(mDataCallListGuard);
    auto dataCalls = std::views::values(mDataCallList);
    return {dataCalls.begin(), dataCalls.end()};
}

ScopedAStatus RadioData::allocatePduSessionId(int32_t serial) {
    LOG_NOT_SUPPORTED;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus RadioData::cancelHandover(int32_t serial, int32_t callId) {
    LOG_NOT_SUPPORTED << callId;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus RadioData::deactivateDataCall(int32_t serial, int32_t cid,
                                            aidl::DataRequestReason reason) {
    LOG_CALL_IGNORED << cid << " " << reason;
    deactivateDataCallBase(cid);
    respond()->deactivateDataCallResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioData::getDataCallList(int32_t serial) {
    LOG_CALL;
    respond()->getDataCallListResponse(noError(serial), getDataCallListBase());
    return ok();
}

ScopedAStatus RadioData::getSlicingConfig(int32_t serial) {
    // Disabled with modemReducedFeatureSet1.
    LOG_NOT_SUPPORTED;
    respond()->getSlicingConfigResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioData::releasePduSessionId(int32_t serial, int32_t id) {
    LOG_NOT_SUPPORTED << id;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus RadioData::responseAcknowledgement() {
    LOG_CALL_NOSERIAL;
    return ok();
}

ScopedAStatus RadioData::setDataAllowed(int32_t serial, bool allow) {
    LOG_NOT_SUPPORTED << allow;
    respond()->setDataAllowedResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioData::setDataProfile(int32_t serial,
                                        const std::vector<aidl::DataProfileInfo>& profiles) {
    LOG_CALL_IGNORED << profiles;
    respond()->setDataProfileResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioData::setDataThrottling(int32_t serial, aidl::DataThrottlingAction dta,
                                           int64_t completionDurationMs) {
    // Disabled with modemReducedFeatureSet1.
    LOG_NOT_SUPPORTED << dta << ' ' << completionDurationMs;
    respond()->setDataThrottlingResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioData::setInitialAttachApn(int32_t serial,
                                             const std::optional<aidl::DataProfileInfo>& info) {
    LOG_CALL_IGNORED << info;
    respond()->setInitialAttachApnResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioData::setResponseFunctions(
        const std::shared_ptr<aidl::IRadioDataResponse>& response,
        const std::shared_ptr<aidl::IRadioDataIndication>& indication) {
    LOG_CALL_NOSERIAL << response << ' ' << indication;
    CHECK(response);
    CHECK(indication);
    respond = response;
    indicate = indication;
    setResponseFunctionsBase();
    return ok();
}

ScopedAStatus RadioData::startHandover(int32_t serial, int32_t callId) {
    LOG_NOT_SUPPORTED << callId;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus RadioData::startKeepalive(int32_t serial, const aidl::KeepaliveRequest& keepalive) {
    LOG_NOT_SUPPORTED << keepalive;
    respond()->startKeepaliveResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioData::stopKeepalive(int32_t serial, int32_t sessionHandle) {
    LOG_NOT_SUPPORTED << sessionHandle;
    respond()->stopKeepaliveResponse(notSupported(serial));
    return ok();
}

}  // namespace android::hardware::radio::minimal
