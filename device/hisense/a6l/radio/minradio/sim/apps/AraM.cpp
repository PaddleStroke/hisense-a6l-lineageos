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

#include <libminradio/sim/apps/AraM.h>

#include "tlv.h"

#include <android-base/logging.h>
#include <libminradio/binder_printing.h>
#include <libminradio/sim/IccConstants.h>
#include <libminradio/sim/IccUtils.h>

namespace android::hardware::radio::minimal::sim::apps {

using namespace ::android::hardware::radio::minimal::binder_printing;
using namespace ::android::hardware::radio::minimal::sim::constants;
using namespace ::android::hardware::radio::minimal::sim::tlv_operators;
namespace aidl = ::aidl::android::hardware::radio::sim;

// From https://source.android.com/docs/core/connect/uicc
static constexpr uint16_t TAG_ALL_REF_AR_DO = 0xFF40;
static constexpr uint8_t TAG_REF_AR_DO = 0xE2;
static constexpr uint8_t TAG_REF_DO = 0xE1;
static constexpr uint8_t TAG_DEVICE_APP_ID_REF_DO = 0xC1;
static constexpr uint8_t TAG_PKG_REF_DO = 0xCA;
static constexpr uint8_t TAG_AR_DO = 0xE3;
static constexpr uint8_t TAG_PERM_AR_DO = 0xDB;

class AraMChannel : public App::Channel {
  public:
    AraMChannel(int32_t channelId, std::shared_ptr<AraM> app);

    aidl::IccIoResult transmit(const aidl::SimApdu& message) override;

  private:
    std::weak_ptr<AraM> mApp;
};

AraM::AraM() : App(AID) {}

std::shared_ptr<App::Channel> AraM::newChannel(int32_t id) {
    return std::make_shared<AraMChannel>(id, shared_from_this());
}

void AraM::addRule(Rule rule) {
    mRules.push_back(rule);
}

std::span<const AraM::Rule> AraM::getRules() const {
    return mRules;
}

AraMChannel::AraMChannel(int32_t channelId, std::shared_ptr<AraM> app)
    : App::Channel(channelId), mApp(app) {}

aidl::IccIoResult AraMChannel::transmit(const aidl::SimApdu& message) {
    auto app = mApp.lock();
    if (!app) {
        LOG(ERROR) << "AraM: App shut down, channel not valid anymore.";
        return toIccIoResult(IO_RESULT_TECHNICAL_PROBLEM);
    }
    if (message.instruction != COMMAND_GET_DATA) {
        LOG(ERROR) << "AraM: Unsupported instruction: " << message;
        return toIccIoResult(IO_RESULT_NOT_SUPPORTED);
    }
    if (message.p1 != (TAG_ALL_REF_AR_DO >> 8) || message.p2 != (TAG_ALL_REF_AR_DO & 0xFF)) {
        LOG(ERROR) << "AraM: Incorrect parameters: " << std::hex << message.p1 << message.p2;
        return toIccIoResult(IO_RESULT_INCORRECT_P1_P2);
    }
    if (message.p3 != 0) {
        return toIccIoResult(IO_RESULT_INCORRECT_LENGTH | 0);
    }

    std::vector<uint8_t> rules;
    for (auto& rule : app->getRules()) {
        // Encoding rules as described in https://source.android.com/docs/core/connect/uicc
        // clang-format off
        rules = rules + makeTlv(TAG_REF_AR_DO,
            makeTlv(TAG_REF_DO,
                makeTlv(TAG_DEVICE_APP_ID_REF_DO, rule.deviceAppID) +
                makeTlv(TAG_PKG_REF_DO, std::vector<uint8_t>(rule.pkg.begin(), rule.pkg.end()))
            ) +
            makeTlv(TAG_AR_DO,
                makeTlv(TAG_PERM_AR_DO, std::vector<uint8_t>{0, 0, 0, 0, 0, 0, 0, 1})
            )
        );
        // clang-format on
    }

    return toIccIoResult(bytesToHexString(makeTlv(TAG_ALL_REF_AR_DO, rules)));
}

}  // namespace android::hardware::radio::minimal::sim::apps
