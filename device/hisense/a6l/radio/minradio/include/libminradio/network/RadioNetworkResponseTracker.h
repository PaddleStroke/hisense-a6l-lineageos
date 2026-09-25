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
#pragma once

#include <libminradio/ResponseTracker.h>

#include <aidl/android/hardware/radio/network/BnRadioNetworkResponse.h>
#include <aidl/android/hardware/radio/network/IRadioNetwork.h>

namespace android::hardware::radio::minimal {

class RadioNetworkResponseTracker
    : public ResponseTracker<::aidl::android::hardware::radio::network::IRadioNetwork,
                             ::aidl::android::hardware::radio::network::IRadioNetworkResponse> {
  public:
    RadioNetworkResponseTracker(
            std::shared_ptr<::aidl::android::hardware::radio::network::IRadioNetwork> req,
            const std::shared_ptr<::aidl::android::hardware::radio::network::IRadioNetworkResponse>&
                    resp);

    ResponseTrackerResult<::aidl::android::hardware::radio::network::RegStateResult>
    getDataRegistrationState();
    ResponseTrackerResult<::aidl::android::hardware::radio::network::SignalStrength>
    getSignalStrength();

  protected:
    ::ndk::ScopedAStatus getDataRegistrationStateResponse(
            const ::aidl::android::hardware::radio::RadioResponseInfo& info,
            const ::aidl::android::hardware::radio::network::RegStateResult& dataRegResp) override;
    ::ndk::ScopedAStatus getSignalStrengthResponse(
            const ::aidl::android::hardware::radio::RadioResponseInfo& info,
            const ::aidl::android::hardware::radio::network::SignalStrength& signalStrength)
            override;
};

}  // namespace android::hardware::radio::minimal
