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

#include <libminradio/sim/App.h>

#include <android-base/logging.h>
#include <libminradio/sim/IccConstants.h>
#include <libminradio/sim/IccUtils.h>

namespace android::hardware::radio::minimal::sim {

using namespace ::android::hardware::radio::minimal::sim::constants;
namespace aidl = ::aidl::android::hardware::radio::sim;

App::App(std::string_view aid) : mAid(aid) {}

std::string_view App::getAid() const {
    return mAid;
}

App::Channel::Channel(uint8_t channelId) : mChannelId(channelId) {}

uint8_t App::Channel::getId() const {
    return mChannelId;
}

std::vector<uint8_t> App::Channel::getSelectResponse() const {
    return {IO_RESULT_SUCCESS >> 8, IO_RESULT_SUCCESS & 0xFF};
}

aidl::IccIoResult App::iccIo(const aidl::IccIo&) {
    return toIccIoResult(IO_RESULT_NOT_SUPPORTED);
}

}  // namespace android::hardware::radio::minimal::sim
