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
#include <libminradio/sim/App.h>
#include <libminradio/sim/Filesystem.h>

namespace android::hardware::radio::minimal::sim::apps {

class FilesystemApp : public App {
  public:
    static constexpr char AID[] = "";

    FilesystemApp(const std::shared_ptr<Filesystem>& filesystem);
    std::shared_ptr<App::Channel> newChannel(int32_t id) override;

    ::aidl::android::hardware::radio::sim::IccIoResult iccIo(
            const ::aidl::android::hardware::radio::sim::IccIo& iccIo) override;

  private:
    class FilesystemChannel;

    std::shared_ptr<FilesystemChannel> mBasicChannel;
    std::shared_ptr<Filesystem> mFilesystem;
};

}  // namespace android::hardware::radio::minimal::sim::apps
