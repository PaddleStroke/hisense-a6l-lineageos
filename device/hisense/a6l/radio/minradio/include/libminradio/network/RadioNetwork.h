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
#pragma once

#include <libminradio/GuaranteedCallback.h>
#include <libminradio/RadioSlotBase.h>
#include <libminradio/network/RadioNetworkResponseTracker.h>

#include <aidl/android/hardware/radio/network/BnRadioNetwork.h>

namespace android::hardware::radio::minimal {

class RadioNetwork : public RadioSlotBase,
                     public aidl::android::hardware::radio::network::BnRadioNetwork {
  public:
    using RadioSlotBase::RadioSlotBase;

  protected:
    std::vector<::aidl::android::hardware::radio::network::CellInfo> getCellInfoListBase();
    void onUpdatedResponseFunctions() override;

    ::ndk::ScopedAStatus getAllowedNetworkTypesBitmap(int32_t serial) override;
    ::ndk::ScopedAStatus getAvailableBandModes(int32_t serial) override;
    ::ndk::ScopedAStatus getAvailableNetworks(int32_t serial) override;
    ::ndk::ScopedAStatus getBarringInfo(int32_t serial) override;
    ::ndk::ScopedAStatus getCdmaRoamingPreference(int32_t serial) override;
    ::ndk::ScopedAStatus getCellInfoList(int32_t serial) override;
    ::ndk::ScopedAStatus getImsRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus getNetworkSelectionMode(int32_t serial) override;
    ::ndk::ScopedAStatus getOperator(int32_t serial) override;
    ::ndk::ScopedAStatus getSystemSelectionChannels(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRadioTechnology(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus isNrDualConnectivityEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus responseAcknowledgement() override;
    ::ndk::ScopedAStatus setAllowedNetworkTypesBitmap(int32_t serial,
                                                      int32_t networkTypeBitmap) override;
    ::ndk::ScopedAStatus setBandMode(
            int32_t serial, ::aidl::android::hardware::radio::network::RadioBandMode mode) override;
    ::ndk::ScopedAStatus setBarringPassword(int32_t serial, const std::string& facility,
                                            const std::string& oldPassword,
                                            const std::string& newPassword) override;
    ::ndk::ScopedAStatus setCdmaRoamingPreference(
            int32_t serial,
            ::aidl::android::hardware::radio::network::CdmaRoamingType type) override;
    ::ndk::ScopedAStatus setCellInfoListRate(int32_t serial, int32_t rate) override;
    ::ndk::ScopedAStatus setIndicationFilter(int32_t serial, int32_t indicationFilter) override;
    ::ndk::ScopedAStatus setLinkCapacityReportingCriteria(
            int32_t serial, int32_t hysteresisMs, int32_t hysteresisDlKbps,
            int32_t hysteresisUlKbps, const std::vector<int32_t>& thresholdsDownlinkKbps,
            const std::vector<int32_t>& thresholdsUplinkKbps,
            ::aidl::android::hardware::radio::AccessNetwork accessNetwork) override;
    ::ndk::ScopedAStatus setLocationUpdates(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeAutomatic(int32_t serial) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeManual(
            int32_t serial, const std::string& operatorNumeric,
            ::aidl::android::hardware::radio::AccessNetwork ran) override;
    ::ndk::ScopedAStatus setNrDualConnectivityState(
            int32_t serial,
            ::aidl::android::hardware::radio::network::NrDualConnectivityState nrSt) override;
    ::ndk::ScopedAStatus setResponseFunctions(
            const std::shared_ptr<::aidl::android::hardware::radio::network::IRadioNetworkResponse>&
                    radioNetworkResponse,
            const std::shared_ptr<
                    ::aidl::android::hardware::radio::network::IRadioNetworkIndication>&
                    radioNetworkIndication) override;
    ::ndk::ScopedAStatus setSignalStrengthReportingCriteria(
            int32_t serial,
            const std::vector<::aidl::android::hardware::radio::network::SignalThresholdInfo>&
                    signalThresholdInfos) override;
    ::ndk::ScopedAStatus setSuppServiceNotifications(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus setSystemSelectionChannels(
            int32_t serial, bool specifyChannels,
            const std::vector<::aidl::android::hardware::radio::network::RadioAccessSpecifier>&
                    specifiers) override;
    ::ndk::ScopedAStatus startNetworkScan(
            int32_t serial,
            const ::aidl::android::hardware::radio::network::NetworkScanRequest& request) override;
    ::ndk::ScopedAStatus stopNetworkScan(int32_t serial) override;
    ::ndk::ScopedAStatus supplyNetworkDepersonalization(int32_t serial,
                                                        const std::string& netPin) override;
    ::ndk::ScopedAStatus setUsageSetting(
            int32_t serial,
            ::aidl::android::hardware::radio::network::UsageSetting usageSetting) override;
    ::ndk::ScopedAStatus getUsageSetting(int32_t serial) override;

    ::ndk::ScopedAStatus setEmergencyMode(
            int32_t serial,
            const ::aidl::android::hardware::radio::network::EmergencyMode emergencyMode) override;
    ::ndk::ScopedAStatus triggerEmergencyNetworkScan(
            int32_t serial,
            const ::aidl::android::hardware::radio::network::EmergencyNetworkScanTrigger&
                    scanTrigger) override;
    ::ndk::ScopedAStatus cancelEmergencyNetworkScan(int32_t serial, bool resetScan) override;
    ::ndk::ScopedAStatus exitEmergencyMode(int32_t serial) override;
    ::ndk::ScopedAStatus setNullCipherAndIntegrityEnabled(int32_t serial, bool enabled) override;
    ::ndk::ScopedAStatus isNullCipherAndIntegrityEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus isN1ModeEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus setN1ModeEnabled(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus isCellularIdentifierTransparencyEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus setCellularIdentifierTransparencyEnabled(int32_t serial,
                                                                  bool enabled) override;
    ::ndk::ScopedAStatus setSecurityAlgorithmsUpdatedEnabled(int32_t serial, bool enabled) override;
    ::ndk::ScopedAStatus isSecurityAlgorithmsUpdatedEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus setSatellitePlmn(
            int32_t in_serial, const std::vector<std::string>& carrierPlmnArray,
            const std::vector<std::string>& allSatellitePlmnArray) override;
    ::ndk::ScopedAStatus setSatelliteEnabledForCarrier(int32_t serial,
                                                       bool satelliteEnabled) override;
    ::ndk::ScopedAStatus isSatelliteEnabledForCarrier(int32_t serial) override;

    GuaranteedCallback<::aidl::android::hardware::radio::network::IRadioNetworkIndication,
                       ::aidl::android::hardware::radio::network::IRadioNetworkIndicationDefault,
                       true>
            indicate;
    GuaranteedCallback<::aidl::android::hardware::radio::network::IRadioNetworkResponse,
                       ::aidl::android::hardware::radio::network::IRadioNetworkResponseDefault>
            respond;

  private:
    int32_t mAllowedNetworkTypesBitmap = std::numeric_limits<int32_t>::max();

    ResponseTrackerHolder<RadioNetworkResponseTracker> mResponseTracker;
};

}  // namespace android::hardware::radio::minimal
