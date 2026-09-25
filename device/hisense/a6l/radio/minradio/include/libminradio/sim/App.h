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

#include <aidl/android/hardware/radio/sim/IccIo.h>
#include <aidl/android/hardware/radio/sim/IccIoResult.h>
#include <aidl/android/hardware/radio/sim/SimApdu.h>
#include <android-base/macros.h>

namespace android::hardware::radio::minimal::sim {

class App {
  public:
    class Channel {
      public:
        Channel(uint8_t channelId);
        virtual ~Channel() = default;

        uint8_t getId() const;
        std::vector<uint8_t> getSelectResponse() const;

        virtual ::aidl::android::hardware::radio::sim::IccIoResult transmit(
                const ::aidl::android::hardware::radio::sim::SimApdu& message) = 0;

      private:
        uint8_t mChannelId;

        DISALLOW_COPY_AND_ASSIGN(Channel);
    };

    virtual ~App() = default;

    std::string_view getAid() const;

    virtual std::shared_ptr<Channel> newChannel(int32_t id) = 0;

    virtual ::aidl::android::hardware::radio::sim::IccIoResult iccIo(
            const ::aidl::android::hardware::radio::sim::IccIo& iccIo);

  protected:
    App(std::string_view aid);

  private:
    std::string mAid;

    DISALLOW_COPY_AND_ASSIGN(App);
};

}  // namespace android::hardware::radio::minimal::sim
