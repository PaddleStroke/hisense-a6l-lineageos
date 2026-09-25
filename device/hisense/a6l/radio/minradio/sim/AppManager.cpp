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

#include <libminradio/sim/AppManager.h>

#include <aidl/android/hardware/radio/RadioConst.h>
#include <android-base/logging.h>
#include <libminradio/binder_printing.h>
#include <libminradio/sim/IccConstants.h>
#include <libminradio/sim/IccUtils.h>
#include <libminradio/sim/apps/FilesystemApp.h>

#include <set>

namespace android::hardware::radio::minimal::sim {

using namespace ::android::hardware::radio::minimal::binder_printing;
using namespace ::android::hardware::radio::minimal::sim::constants;
using ::aidl::android::hardware::radio::RadioConst;
using ::aidl::android::hardware::radio::RadioError;
namespace aidl = ::aidl::android::hardware::radio::sim;

// ETSI TS 102 221 10.1.2 (table 10.5)
static std::map<uint8_t, std::set<uint8_t>> mCommandClasses = {
        {COMMAND_READ_BINARY, {0}},                  //
        {COMMAND_UPDATE_BINARY, {0}},                //
        {COMMAND_READ_RECORD, {0}},                  //
        {COMMAND_UPDATE_RECORD, {0}},                //
        {COMMAND_SEEK, {0}},                         //
        {COMMAND_SELECT, {0}},                       //
        {COMMAND_GET_RESPONSE, {0}},                 //
        {COMMAND_STATUS, {0x80, 0x81, 0x82, 0x83}},  //
        {COMMAND_GET_DATA, {0x80}},                  //
        {COMMAND_MANAGE_CHANNEL, {0}},               //
};

static constexpr uint8_t MANAGE_CHANNEL_OPEN = 0x00;
static constexpr uint8_t MANAGE_CHANNEL_CLOSE = 0x80;

AppManager::AppManager() {}

void AppManager::addApp(std::shared_ptr<App> app) {
    mApps[std::string{app->getAid()}] = app;

    // Channel 0 is always available per 3GPP TS 102 221 11.1.17
    if (app->getAid() == apps::FilesystemApp::AID) {
        std::unique_lock lck(mChannelsGuard);
        mChannels[0] = app->newChannel(0);
    }
}

std::pair<RadioError, std::shared_ptr<App::Channel>> AppManager::openLogicalChannel(
        std::string_view aid, int32_t p2) {
    auto appIt = mApps.find(aid);
    if (appIt == mApps.end()) {
        LOG(WARNING) << "App " << aid << " not found";
        return {RadioError::NO_SUCH_ELEMENT, nullptr};
    }

    // ETSI TS 102 221 11.1.1.2 Table 11.2
    // P2 == 0x00: Application activation / reset; First or only occurrence
    //       0x0C: No data returned
    if (p2 != 0x00 && p2 != 0x0C && p2 != RadioConst::P2_CONSTANT_NO_P2) {
        LOG(ERROR) << "P2 != 0x00 or 0x0C not supported";
        return {RadioError::INVALID_ARGUMENTS, nullptr};
    }

    std::unique_lock lck(mChannelsGuard);

    // Find available channel. It must be in 1-3 range per 3GPP TS 102 221 11.1.17.1
    std::optional<unsigned> channelId;
    for (uint8_t i = 1; i <= 3; i++) {
        if (mChannels.find(i) == mChannels.end()) {
            channelId = i;
            break;
        }
    }
    if (!channelId.has_value()) {
        LOG(ERROR) << "AppManager: All channels are busy";
        return {RadioError::MISSING_RESOURCE, nullptr};
    }

    auto channel = appIt->second->newChannel(*channelId);
    mChannels[*channelId] = channel;
    LOG(DEBUG) << "AppManager: opened logical channel " << *channelId;
    return {RadioError::NONE, std::move(channel)};
}

RadioError AppManager::closeLogicalChannel(int32_t channelId) {
    if (channelId == 0) {
        // 3GPP TS 102 221 11.1.17: channel 0 is guaranteed to be always available
        return RadioError::INVALID_ARGUMENTS;
    }

    std::unique_lock lck(mChannelsGuard);
    auto it = mChannels.find(channelId);
    if (it == mChannels.end()) {
        return RadioError::MISSING_RESOURCE;
    }
    mChannels.erase(it);
    LOG(DEBUG) << "AppManager: closed logical channel " << channelId;
    return RadioError::NONE;
}

aidl::IccIoResult AppManager::transmit(const aidl::SimApdu& message) {
    // Fetch channel
    std::shared_ptr<App::Channel> channel;
    {
        std::unique_lock lck(mChannelsGuard);
        auto chIt = mChannels.find(message.sessionId);
        if (chIt == mChannels.end()) {
            return toIccIoResult(IO_RESULT_CHANNEL_NOT_SUPPORTED);
        }
        channel = chIt->second;
    }

    // Verify instruction matching command class
    auto classIt = mCommandClasses.find(message.instruction);
    if (classIt == mCommandClasses.end()) {
        LOG(ERROR) << "Command not found for " << message;
        return toIccIoResult(IO_RESULT_NOT_SUPPORTED);
    }
    if (!classIt->second.contains(message.cla)) {
        LOG(ERROR) << "Unsupported command class: " << message;
        return toIccIoResult(IO_RESULT_CLASS_NOT_SUPPORTED);
    }

    switch (message.instruction) {
        case COMMAND_MANAGE_CHANNEL:
            return commandManageChannel(message.p1, message.p2);
        default:
            // Pass the message to the channel
            return channel->transmit(message);
    }
}

aidl::IccIoResult AppManager::iccIo(const aidl::IccIo& iccIo) {
    auto appIt = mApps.find(iccIo.aid);
    if (appIt == mApps.end()) {
        LOG(WARNING) << "App " << iccIo.aid << " not found";
        return toIccIoResult(IO_RESULT_FILE_NOT_FOUND);
    }

    return appIt->second->iccIo(iccIo);
}

// ISO 7816 7.1.2
aidl::IccIoResult AppManager::commandManageChannel(int32_t operation, int32_t channelId) {
    if (operation == MANAGE_CHANNEL_OPEN) {
        if (channelId != 0) {
            LOG(ERROR) << "Not implemented: opening explicit channel IDs: " << channelId;
            return toIccIoResult(IO_RESULT_INCORRECT_P1_P2);
        }
        auto [status, channel] = openLogicalChannel("", 0);
        if (channel) {
            return toIccIoResult(uint8ToBytes(channel->getId()));
        } else {
            return toIccIoResult(IO_RESULT_CHANNEL_NOT_SUPPORTED);
        }
    } else if (operation == MANAGE_CHANNEL_CLOSE) {
        auto status = closeLogicalChannel(channelId);
        if (status == RadioError::NONE) {
            return toIccIoResult("");
        }
        return toIccIoResult(IO_RESULT_INCORRECT_P1_P2);
    } else {
        LOG(ERROR) << "Invalid MANAGE_CHANNEL operation: " << operation;
        return toIccIoResult(IO_RESULT_INCORRECT_P1_P2);
    }
}

}  // namespace android::hardware::radio::minimal::sim
