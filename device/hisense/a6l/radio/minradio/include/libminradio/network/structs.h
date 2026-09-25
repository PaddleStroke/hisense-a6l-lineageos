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

#include <aidl/android/hardware/radio/network/CellInfo.h>
#include <aidl/android/hardware/radio/network/RegStateResult.h>
#include <aidl/android/hardware/radio/network/SignalStrength.h>
#include <aidl/android/hardware/radio/network/SignalThresholdInfo.h>

namespace android::hardware::radio::minimal::structs {

::aidl::android::hardware::radio::network::SignalStrength makeSignalStrength();
::aidl::android::hardware::radio::network::CellInfo makeCellInfo(
        const ::aidl::android::hardware::radio::network::RegStateResult& regState,
        const ::aidl::android::hardware::radio::network::SignalStrength& signalStrength);

::aidl::android::hardware::radio::network::OperatorInfo getOperatorInfo(
        const ::aidl::android::hardware::radio::network::CellIdentity& cellIdentity);

int32_t rssiToSignalStrength(int32_t rssi);
int32_t validateRsrp(int32_t rsrp);
int32_t validateRsrq(int32_t rsrq);
bool validateSignalThresholdInfos(
        const std::vector<::aidl::android::hardware::radio::network::SignalThresholdInfo>& infos);

}  // namespace android::hardware::radio::minimal::structs
