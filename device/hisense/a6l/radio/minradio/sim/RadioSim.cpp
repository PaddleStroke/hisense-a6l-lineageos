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

#include <libminradio/sim/RadioSim.h>

#include <libminradio/debug.h>
#include <libminradio/response.h>
#include <libminradio/sim/IccUtils.h>
#include <libminradio/sim/apps/AraM.h>
#include <libminradio/sim/apps/FilesystemApp.h>

#define RADIO_MODULE "Sim"

namespace android::hardware::radio::minimal {

using namespace ::android::hardware::radio::minimal::binder_printing;
using ::aidl::android::hardware::radio::RadioError;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::sim;
constexpr auto ok = &ScopedAStatus::ok;

RadioSim::RadioSim(std::shared_ptr<SlotContext> context) : RadioSlotBase(context) {
    mAppManager.addApp(std::make_shared<sim::apps::FilesystemApp>(mFilesystem));

    mFilesystem->write(sim::paths::fplmn, sim::encodeFplmns({}));
    mFilesystem->write(sim::paths::pl, "en");
}

void RadioSim::setIccid(std::string iccid) {
    mFilesystem->writeBch(sim::paths::iccid, iccid);
}

std::optional<std::string> RadioSim::getIccid() const {
    return mFilesystem->readBch(sim::paths::iccid);
}

void RadioSim::addCtsCertificate() {
    static constexpr char CTS_UICC_2021[] =
            "CE7B2B47AE2B7552C8F92CC29124279883041FB623A5F194A82C9BF15D492AA0";

    auto aram = std::make_shared<sim::apps::AraM>();
    mAppManager.addApp(aram);
    aram->addRule({
            .deviceAppID = sim::hexStringToBytes(CTS_UICC_2021),
            .pkg = "android.carrierapi.cts",
    });
}

ScopedAStatus RadioSim::areUiccApplicationsEnabled(int32_t serial) {
    LOG_CALL;
    respond()->areUiccApplicationsEnabledResponse(noError(serial), mAreUiccApplicationsEnabled);
    return ok();
}

ScopedAStatus RadioSim::changeIccPin2ForApp(int32_t serial, const std::string& oldPin2,
                                            const std::string& newPin2, const std::string& aid) {
    LOG_NOT_SUPPORTED << oldPin2 << ' ' << newPin2 << ' ' << aid;
    respond()->changeIccPin2ForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::changeIccPinForApp(int32_t serial, const std::string& oldPin,
                                           const std::string& newPin, const std::string& aid) {
    LOG_NOT_SUPPORTED << oldPin << ' ' << newPin << ' ' << aid;
    respond()->changeIccPinForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::enableUiccApplications(int32_t serial, bool enable) {
    LOG_CALL_IGNORED << enable;
    mAreUiccApplicationsEnabled = enable;
    respond()->enableUiccApplicationsResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioSim::getAllowedCarriers(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->getAllowedCarriersResponse(notSupported(serial), {}, {});
    return ok();
}

ScopedAStatus RadioSim::getCdmaSubscription(int32_t serial) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioSim::getCdmaSubscriptionSource(int32_t serial) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioSim::getFacilityLockForApp(  //
        int32_t serial, const std::string& facility, const std::string& password,
        int32_t serviceClass, const std::string& appId) {
    LOG_CALL << facility << ' ' << password << ' ' << serviceClass << ' ' << appId;
    respond()->getFacilityLockForAppResponse(noError(serial), 0);  // 0 means "disabled for all"
    return ok();
}

ScopedAStatus RadioSim::getSimPhonebookCapacity(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->getSimPhonebookCapacityResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioSim::getSimPhonebookRecords(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->getSimPhonebookRecordsResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioSim::iccCloseLogicalChannel(int32_t serial, int32_t) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioSim::iccCloseLogicalChannelWithSessionInfo(
        int32_t serial, const aidl::SessionInfo& sessionInfo) {
    LOG_CALL << sessionInfo;
    auto status = mAppManager.closeLogicalChannel(sessionInfo.sessionId);
    respond()->iccCloseLogicalChannelWithSessionInfoResponse(errorResponse(serial, status));
    return ok();
}

ScopedAStatus RadioSim::iccIoForApp(int32_t serial, const aidl::IccIo& iccIo) {
    LOG_CALL << iccIo;
    respond()->iccIoForAppResponse(noError(serial), mAppManager.iccIo(iccIo));
    return ok();
}

ScopedAStatus RadioSim::iccOpenLogicalChannel(int32_t serial, const std::string& aid, int32_t p2) {
    LOG_CALL << aid << ' ' << p2;
    auto [status, channel] = mAppManager.openLogicalChannel(aid, p2);
    respond()->iccOpenLogicalChannelResponse(
            errorResponse(serial, status), channel ? channel->getId() : 0,
            channel ? channel->getSelectResponse() : std::vector<uint8_t>{});
    return ok();
}

ScopedAStatus RadioSim::iccTransmitApduBasicChannel(int32_t serial, const aidl::SimApdu& message) {
    LOG_CALL << message;
    if (message.sessionId != 0) {
        LOG(ERROR) << "Basic channel session ID should be zero, but was " << message.sessionId;
        respond()->iccTransmitApduBasicChannelResponse(
                errorResponse(serial, RadioError::INVALID_ARGUMENTS), {});
        return ok();
    }
    respond()->iccTransmitApduBasicChannelResponse(noError(serial), mAppManager.transmit(message));
    return ok();
}

ScopedAStatus RadioSim::iccTransmitApduLogicalChannel(int32_t serial,
                                                      const aidl::SimApdu& message) {
    LOG_CALL << message;
    respond()->iccTransmitApduLogicalChannelResponse(noError(serial),
                                                     mAppManager.transmit(message));
    return ok();
}

ScopedAStatus RadioSim::reportStkServiceIsRunning(int32_t serial) {
    LOG_CALL_IGNORED;
    respond()->reportStkServiceIsRunningResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioSim::requestIccSimAuthentication(  //
        int32_t serial, int32_t authContext, const std::string& authData, const std::string& aid) {
    LOG_NOT_SUPPORTED << authContext << ' ' << authData << ' ' << aid;
    respond()->requestIccSimAuthenticationResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioSim::responseAcknowledgement() {
    LOG_CALL_NOSERIAL;
    return ok();
}

ScopedAStatus RadioSim::sendEnvelope(int32_t serial, const std::string& command) {
    LOG_NOT_SUPPORTED << command;
    respond()->sendEnvelopeResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioSim::sendEnvelopeWithStatus(int32_t serial, const std::string& contents) {
    LOG_NOT_SUPPORTED << contents;
    respond()->sendEnvelopeWithStatusResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioSim::sendTerminalResponseToSim(int32_t serial,
                                                  const std::string& commandResponse) {
    LOG_NOT_SUPPORTED << commandResponse;
    respond()->sendTerminalResponseToSimResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioSim::setAllowedCarriers(  //
        int32_t serial, const aidl::CarrierRestrictions& carriers, aidl::SimLockMultiSimPolicy mp) {
    LOG_NOT_SUPPORTED << carriers << ' ' << mp;
    respond()->setAllowedCarriersResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioSim::setCarrierInfoForImsiEncryption(
        int32_t serial, const aidl::ImsiEncryptionInfo& imsiEncryptionInfo) {
    LOG_NOT_SUPPORTED << imsiEncryptionInfo;
    respond()->setCarrierInfoForImsiEncryptionResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioSim::setCdmaSubscriptionSource(int32_t serial, aidl::CdmaSubscriptionSource) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioSim::setFacilityLockForApp(  //
        int32_t serial, const std::string& facility, bool lockState, const std::string& password,
        int32_t serviceClass, const std::string& appId) {
    LOG_NOT_SUPPORTED << facility << ' ' << lockState << ' ' << password << ' ' << serviceClass
                      << ' ' << appId;
    respond()->setFacilityLockForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::setResponseFunctions(
        const std::shared_ptr<aidl::IRadioSimResponse>& response,
        const std::shared_ptr<aidl::IRadioSimIndication>& indication) {
    LOG_CALL_NOSERIAL << response << ' ' << indication;
    CHECK(response);
    CHECK(indication);
    respond = response;
    indicate = indication;
    setResponseFunctionsBase();
    return ok();
}

ScopedAStatus RadioSim::setSimCardPower(int32_t serial, aidl::CardPowerState powerUp) {
    LOG_NOT_SUPPORTED << powerUp;
    respond()->setSimCardPowerResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioSim::setUiccSubscription(int32_t serial, const aidl::SelectUiccSub&) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioSim::supplyIccPin2ForApp(int32_t serial, const std::string& pin2,
                                            const std::string& aid) {
    LOG_NOT_SUPPORTED << pin2 << ' ' << aid;
    respond()->supplyIccPin2ForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::supplyIccPinForApp(int32_t serial, const std::string& pin,
                                           const std::string& aid) {
    LOG_CALL << "string[" << pin.size() << "] " << aid
             << " (should not be called with PinState::DISABLED)";
    respond()->supplyIccPinForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::supplyIccPuk2ForApp(int32_t serial, const std::string& puk2,
                                            const std::string& pin2, const std::string& aid) {
    LOG_NOT_SUPPORTED << puk2 << ' ' << pin2 << ' ' << aid;
    respond()->supplyIccPuk2ForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::supplyIccPukForApp(int32_t serial, const std::string& puk,
                                           const std::string& pin, const std::string& aid) {
    LOG_NOT_SUPPORTED << puk << ' ' << pin << ' ' << aid;
    respond()->supplyIccPukForAppResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioSim::supplySimDepersonalization(int32_t serial, aidl::PersoSubstate pss,
                                                   const std::string& controlKey) {
    LOG_NOT_SUPPORTED << pss << ' ' << controlKey;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

ScopedAStatus RadioSim::updateSimPhonebookRecords(int32_t serial,
                                                  const aidl::PhonebookRecordInfo& recordInfo) {
    LOG_NOT_SUPPORTED << recordInfo;
    return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
}

}  // namespace android::hardware::radio::minimal
