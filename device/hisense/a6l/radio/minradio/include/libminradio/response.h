/*
 * Copyright (C) 2021 The Android Open Source Project
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

#include <aidl/android/hardware/radio/RadioResponseInfo.h>

namespace android::hardware::radio::minimal {

aidl::android::hardware::radio::RadioResponseInfo noError(int32_t serial);
aidl::android::hardware::radio::RadioResponseInfo notSupported(int32_t serial);
aidl::android::hardware::radio::RadioResponseInfo errorResponse(
        int32_t serial, aidl::android::hardware::radio::RadioError error);

#define RESPOND_ERROR_IF_NOT_CONNECTED(responseMethod, ...) \
    if (!mContext->isConnected()) RESPOND_NOT_CONNECTED(responseMethod, __VA_ARGS__);

#define RESPOND_NOT_CONNECTED(responseMethod, ...)                                          \
    {                                                                                       \
        LOG(WARNING) << (RADIO_MODULE ".") << __func__ << " called before rilConnected";    \
        const auto responseInfo = ::android::hardware::radio::minimal::errorResponse(       \
                serial, ::aidl::android::hardware::radio::RadioError::RADIO_NOT_AVAILABLE); \
        respond()->responseMethod(responseInfo __VA_OPT__(, ) __VA_ARGS__);                 \
        return ok();                                                                        \
    }

}  // namespace android::hardware::radio::minimal
