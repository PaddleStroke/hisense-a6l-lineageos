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

#include <aidl/android/hardware/radio/RadioError.h>
#include <aidl/android/hardware/radio/sim/IccIo.h>
#include <android-base/macros.h>
#include <libminradio/sim/App.h>

#include <map>

namespace android::hardware::radio::minimal::sim {

class AppManager {
  public:
    AppManager();

    void addApp(std::shared_ptr<App> app);

    std::pair<::aidl::android::hardware::radio::RadioError, std::shared_ptr<App::Channel>>
    openLogicalChannel(std::string_view aid, int32_t p2);
    ::aidl::android::hardware::radio::RadioError closeLogicalChannel(int32_t channelId);

    ::aidl::android::hardware::radio::sim::IccIoResult transmit(
            const ::aidl::android::hardware::radio::sim::SimApdu& message);
    ::aidl::android::hardware::radio::sim::IccIoResult iccIo(
            const ::aidl::android::hardware::radio::sim::IccIo& iccIo);

  private:
    std::map<std::string, std::shared_ptr<App>, std::less<>> mApps;
    mutable std::mutex mChannelsGuard;
    std::map<int32_t, std::shared_ptr<App::Channel>> mChannels;

    ::aidl::android::hardware::radio::sim::IccIoResult commandManageChannel(int32_t p1, int32_t p2);

    DISALLOW_COPY_AND_ASSIGN(AppManager);
};

}  // namespace android::hardware::radio::minimal::sim
