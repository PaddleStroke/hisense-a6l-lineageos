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

#include <libminradio/sim/App.h>

#include <span>

namespace android::hardware::radio::minimal::sim::apps {

/**
 * UICC carrier privileges app (ARA-M) implementation.
 *
 * https://source.android.com/docs/core/connect/uicc
 */
class AraM : public std::enable_shared_from_this<AraM>, public App {
  public:
    static constexpr char AID[] = "A00000015141434C00";

    struct Rule {
        std::vector<uint8_t> deviceAppID;
        std::string pkg;
    };

    AraM();
    std::shared_ptr<App::Channel> newChannel(int32_t id) override;

    void addRule(Rule rule);
    std::span<const Rule> getRules() const;

  private:
    std::vector<Rule> mRules;
};

}  // namespace android::hardware::radio::minimal::sim::apps
