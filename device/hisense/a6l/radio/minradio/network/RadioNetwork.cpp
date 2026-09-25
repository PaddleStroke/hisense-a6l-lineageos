/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <libminradio/network/RadioNetwork.h>

#include <libminradio/debug.h>
#include <libminradio/network/structs.h>
#include <libminradio/response.h>

#include <chrono>
#include <thread>

#define RADIO_MODULE "Network"

namespace android::hardware::radio::minimal {

using namespace ::android::hardware::radio::minimal::binder_printing;
using ::aidl::android::hardware::radio::AccessNetwork;
using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::ndk::ScopedAStatus;
namespace aidl = ::aidl::android::hardware::radio::network;
namespace aidlRadio = ::aidl::android::hardware::radio;
constexpr auto ok = &ScopedAStatus::ok;

std::vector<aidl::CellInfo> RadioNetwork::getCellInfoListBase() {
    if (!mResponseTracker) return {};

    // There's a slight race between get*RegistrationState and getSignalStrength, but
    // getCellInfoListBase is best-effort anyway, so it's the best we can do here.
    auto dataRegistrationState = mResponseTracker()->getDataRegistrationState();
    auto signalStrength = mResponseTracker()->getSignalStrength();
    if (!dataRegistrationState.expectOk() || !signalStrength.expectOk()) return {};

    return {structs::makeCellInfo(*dataRegistrationState, *signalStrength)};
}

ScopedAStatus RadioNetwork::getAllowedNetworkTypesBitmap(int32_t serial) {
    LOG_CALL;
    respond()->getAllowedNetworkTypesBitmapResponse(noError(serial), mAllowedNetworkTypesBitmap);
    return ok();
}

ScopedAStatus RadioNetwork::getAvailableBandModes(int32_t serial) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioNetwork::getAvailableNetworks(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->getAvailableNetworksResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioNetwork::getBarringInfo(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->getBarringInfoResponse(notSupported(serial), {}, {});
    return ok();
}

ScopedAStatus RadioNetwork::getCdmaRoamingPreference(int32_t serial) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioNetwork::getCellInfoList(int32_t serial) {
    LOG_CALL;
    RESPOND_ERROR_IF_NOT_CONNECTED(getCellInfoListResponse, {});
    respond()->getCellInfoListResponse(noError(serial), getCellInfoListBase());
    return ok();
}

ScopedAStatus RadioNetwork::getImsRegistrationState(int32_t serial) {
    LOG_AND_RETURN_DEPRECATED();
}

// Starting with HALs certified after 2025-04, this method must be overridden or the device
// will fail CTS. Non-VTS devices certified for SDK levels > 36 must do the same.
ScopedAStatus RadioNetwork::getNetworkSelectionMode(int32_t serial) {
    LOG_CALL;
    respond()->getNetworkSelectionModeResponse(noError(serial), /*manual*/ false);
    return ok();
}

ScopedAStatus RadioNetwork::getOperator(int32_t serial) {
    LOG_CALL;

    auto dataRegistrationState = mResponseTracker()->getDataRegistrationState();
    if (!dataRegistrationState.expectOk()) {
        respond()->getOperatorResponse(errorResponse(serial, RadioError::INTERNAL_ERR), {}, {}, {});
        return ok();
    }

    auto opInfo = structs::getOperatorInfo(dataRegistrationState->cellIdentity);
    respond()->getOperatorResponse(noError(serial), opInfo.alphaLong, opInfo.alphaShort,
                                   opInfo.operatorNumeric);
    return ok();
}

ScopedAStatus RadioNetwork::getSystemSelectionChannels(int32_t serial) {
    LOG_CALL_IGNORED;
    respond()->getSystemSelectionChannelsResponse(noError(serial), {});
    return ok();
}

ScopedAStatus RadioNetwork::getVoiceRadioTechnology(int32_t serial) {
    LOG_CALL;
    respond()->getVoiceRadioTechnologyResponse(noError(serial),
                                               aidlRadio::RadioTechnology::UNKNOWN);
    return ok();
}

ScopedAStatus RadioNetwork::getVoiceRegistrationState(int32_t serial) {
    LOG_CALL;
    respond()->getVoiceRegistrationStateResponse(noError(serial),
                                                 {aidl::RegState::NOT_REG_MT_NOT_SEARCHING_OP});
    return ok();
}

ScopedAStatus RadioNetwork::isNrDualConnectivityEnabled(int32_t serial) {
    // Disabled with modemReducedFeatureSet1.
    LOG_NOT_SUPPORTED;
    respond()->isNrDualConnectivityEnabledResponse(notSupported(serial), false);
    return ok();
}

ScopedAStatus RadioNetwork::responseAcknowledgement() {
    LOG_CALL_NOSERIAL;
    return ok();
}

ScopedAStatus RadioNetwork::setAllowedNetworkTypesBitmap(int32_t serial, int32_t ntype) {
    LOG_CALL_IGNORED << ntype;
    mAllowedNetworkTypesBitmap = ntype;
    respond()->setAllowedNetworkTypesBitmapResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setBandMode(int32_t serial, aidl::RadioBandMode) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioNetwork::setBarringPassword(int32_t serial, const std::string& facility,
                                               const std::string& oldPw, const std::string& newPw) {
    LOG_NOT_SUPPORTED << facility << ' ' << oldPw << ' ' << newPw;
    respond()->setBarringPasswordResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setCdmaRoamingPreference(int32_t serial, aidl::CdmaRoamingType) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioNetwork::setCellInfoListRate(int32_t serial, int32_t rate) {
    LOG_NOT_SUPPORTED << rate;
    respond()->setCellInfoListRateResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setIndicationFilter(int32_t serial, int32_t indFilter) {
    LOG_CALL_IGNORED << indFilter;
    respond()->setIndicationFilterResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setLinkCapacityReportingCriteria(
        int32_t serial, int32_t hysteresisMs, int32_t hysteresisDlKbps, int32_t hysteresisUlKbps,
        const std::vector<int32_t>& thrDownlinkKbps, const std::vector<int32_t>& thrUplinkKbps,
        AccessNetwork accessNetwork) {
    LOG_NOT_SUPPORTED << hysteresisMs << ' ' << hysteresisDlKbps << ' ' << hysteresisUlKbps << ' '
                      << thrDownlinkKbps << ' ' << thrUplinkKbps << ' ' << accessNetwork;
    respond()->setLinkCapacityReportingCriteriaResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setLocationUpdates(int32_t serial, bool) {
    LOG_AND_RETURN_DEPRECATED();
}

// Starting with HALs certified after 2025-04, this method must be overridden or the device
// will fail CTS. Non-VTS devices certified for SDK levels > 36 must do the same.
ScopedAStatus RadioNetwork::setNetworkSelectionModeAutomatic(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->setNetworkSelectionModeAutomaticResponse(notSupported(serial));
    return ok();
}

// Starting with HALs certified after 2025-04, this method must be overridden or the device
// will fail CTS. Non-VTS devices certified for SDK levels > 36 must do the same.
ScopedAStatus RadioNetwork::setNetworkSelectionModeManual(int32_t serial,
                                                          const std::string& opNumeric,
                                                          AccessNetwork ran) {
    LOG_NOT_SUPPORTED << opNumeric << ' ' << ran;
    respond()->setNetworkSelectionModeManualResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setNrDualConnectivityState(int32_t serial,
                                                       aidl::NrDualConnectivityState st) {
    // Disabled with modemReducedFeatureSet1.
    LOG_NOT_SUPPORTED << st;
    respond()->setNrDualConnectivityStateResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setResponseFunctions(
        const std::shared_ptr<aidl::IRadioNetworkResponse>& response,
        const std::shared_ptr<aidl::IRadioNetworkIndication>& indication) {
    LOG_CALL_NOSERIAL << response << ' ' << indication;
    CHECK(response);
    CHECK(indication);
    mResponseTracker = ndk::SharedRefBase::make<RadioNetworkResponseTracker>(
            ref<aidl::IRadioNetwork>(), response);
    respond = mResponseTracker.get();
    indicate = indication;
    setResponseFunctionsBase();
    return ok();
}

void RadioNetwork::onUpdatedResponseFunctions() {
    indicate()->cellInfoList(RadioIndicationType::UNSOLICITED, getCellInfoListBase());
    auto signalStrengthResponse = mResponseTracker()->getSignalStrength();
    if (signalStrengthResponse.expectOk()) {
        aidl::SignalStrength signalStrength = *signalStrengthResponse;
        indicate()->currentSignalStrength(RadioIndicationType::UNSOLICITED, signalStrength);

        // TODO(b/379302126): fix race condition in ServiceStateTracker which doesn't listen for
        //       EVENT_UNSOL_CELL_INFO_LIST for the first ~1.3s after setResponseFunctions
        // TODO(b/379302126): fix race condition in SignalStrengthController, starting to listen for
        //       EVENT_SIGNAL_STRENGTH_UPDATE after ~3.7s
        // This workaround thread would be a race condition itself (with use-after-free), but we can
        // drop it once the two bugs mentioned above are fixed.
        std::thread([this, signalStrength] {
            for (int i = 0; i < 10; i++) {
                using namespace std::chrono_literals;
                std::this_thread::sleep_for(1s);
                indicate()->cellInfoList(RadioIndicationType::UNSOLICITED, getCellInfoListBase());
                indicate()->currentSignalStrength(RadioIndicationType::UNSOLICITED, signalStrength);
            }
        }).detach();
    }
}

ScopedAStatus RadioNetwork::setSignalStrengthReportingCriteria(
        int32_t serial, const std::vector<aidl::SignalThresholdInfo>& infos) {
    LOG_CALL_IGNORED << infos;
    respond()->setSignalStrengthReportingCriteriaResponse(
            structs::validateSignalThresholdInfos(infos)
                    ? noError(serial)
                    : errorResponse(serial, RadioError::INVALID_ARGUMENTS));
    return ok();
}

ScopedAStatus RadioNetwork::setSuppServiceNotifications(int32_t serial, bool) {
    LOG_AND_RETURN_DEPRECATED();
}

ScopedAStatus RadioNetwork::setSystemSelectionChannels(  //
        int32_t serial, bool specifyCh, const std::vector<aidl::RadioAccessSpecifier>& specifiers) {
    LOG_CALL_IGNORED << specifyCh << ' ' << specifiers;
    if (specifiers.empty()) {
        respond()->setSystemSelectionChannelsResponse(noError(serial));
    } else {
        respond()->setSystemSelectionChannelsResponse(notSupported(serial));
    }
    return ok();
}

ScopedAStatus RadioNetwork::startNetworkScan(int32_t serial, const aidl::NetworkScanRequest& req) {
    LOG_NOT_SUPPORTED << req;
    respond()->startNetworkScanResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::stopNetworkScan(int32_t serial) {
    LOG_CALL_IGNORED;
    respond()->stopNetworkScanResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioNetwork::supplyNetworkDepersonalization(int32_t serial,
                                                           const std::string& nPin) {
    LOG_NOT_SUPPORTED << nPin;
    respond()->supplyNetworkDepersonalizationResponse(notSupported(serial), -1);
    return ok();
}

ScopedAStatus RadioNetwork::setUsageSetting(int32_t serial, aidl::UsageSetting usageSetting) {
    LOG_CALL_IGNORED << usageSetting;
    if (usageSetting == aidl::UsageSetting::DATA_CENTRIC) {
        respond()->setUsageSettingResponse(noError(serial));
    } else {
        respond()->setUsageSettingResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
    }
    return ok();
}

ScopedAStatus RadioNetwork::getUsageSetting(int32_t serial) {
    LOG_CALL;
    respond()->getUsageSettingResponse(noError(serial), aidl::UsageSetting::DATA_CENTRIC);
    return ok();
}

ScopedAStatus RadioNetwork::setEmergencyMode(int32_t serial, aidl::EmergencyMode emergencyMode) {
    LOG_NOT_SUPPORTED << emergencyMode;
    respond()->setEmergencyModeResponse(notSupported(serial), {});
    return ok();
}

ScopedAStatus RadioNetwork::triggerEmergencyNetworkScan(
        int32_t serial, const aidl::EmergencyNetworkScanTrigger& trigger) {
    LOG_NOT_SUPPORTED << trigger;
    respond()->triggerEmergencyNetworkScanResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::cancelEmergencyNetworkScan(int32_t serial, bool resetScan) {
    LOG_NOT_SUPPORTED << resetScan;
    respond()->cancelEmergencyNetworkScanResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::exitEmergencyMode(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->exitEmergencyModeResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setNullCipherAndIntegrityEnabled(int32_t serial, bool enabled) {
    LOG_CALL_IGNORED << enabled;
    respond()->setNullCipherAndIntegrityEnabledResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioNetwork::isNullCipherAndIntegrityEnabled(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->isNullCipherAndIntegrityEnabledResponse(notSupported(serial), false);
    return ok();
}

ScopedAStatus RadioNetwork::isN1ModeEnabled(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->isN1ModeEnabledResponse(notSupported(serial), false);
    return ok();
}

ScopedAStatus RadioNetwork::setN1ModeEnabled(int32_t serial, bool enable) {
    LOG_NOT_SUPPORTED << enable;
    respond()->setN1ModeEnabledResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::isCellularIdentifierTransparencyEnabled(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->isCellularIdentifierTransparencyEnabledResponse(notSupported(serial), false);
    return ok();
}

ScopedAStatus RadioNetwork::setCellularIdentifierTransparencyEnabled(int32_t serial, bool enabled) {
    LOG_CALL_IGNORED << enabled;
    respond()->setCellularIdentifierTransparencyEnabledResponse(noError(serial));
    return ok();
}

ScopedAStatus RadioNetwork::isSecurityAlgorithmsUpdatedEnabled(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->isSecurityAlgorithmsUpdatedEnabledResponse(notSupported(serial), false);
    return ok();
}

ScopedAStatus RadioNetwork::setSecurityAlgorithmsUpdatedEnabled(int32_t serial, bool enable) {
    LOG_NOT_SUPPORTED << enable;
    respond()->setSecurityAlgorithmsUpdatedEnabledResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setSatellitePlmn(
        int32_t serial, const std::vector<std::string>& carrierPlmnArray,
        const std::vector<std::string>& allSatellitePlmnArray) {
    LOG_NOT_SUPPORTED << carrierPlmnArray << ' ' << allSatellitePlmnArray;
    respond()->setSatellitePlmnResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::setSatelliteEnabledForCarrier(int32_t serial, bool satelliteEnabled) {
    LOG_NOT_SUPPORTED << satelliteEnabled;
    respond()->setSatelliteEnabledForCarrierResponse(notSupported(serial));
    return ok();
}

ScopedAStatus RadioNetwork::isSatelliteEnabledForCarrier(int32_t serial) {
    LOG_NOT_SUPPORTED;
    respond()->isSatelliteEnabledForCarrierResponse(notSupported(serial), false);
    return ok();
}

}  // namespace android::hardware::radio::minimal
