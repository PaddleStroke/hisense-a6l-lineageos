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

#include <aidl/android/hardware/radio/modem/BnRadioModem.h>

namespace android::hardware::radio::minimal {

class RadioModem : public RadioSlotBase,
                   public aidl::android::hardware::radio::modem::BnRadioModem {
  public:
    RadioModem(std::shared_ptr<SlotContext> context,
               std::vector<aidl::android::hardware::radio::RadioTechnology> rats);

  protected:
    void onUpdatedResponseFunctions() override;

    ::ndk::ScopedAStatus enableModem(int32_t serial, bool on) override;
    ::ndk::ScopedAStatus getBasebandVersion(int32_t serial) override;
    ::ndk::ScopedAStatus getDeviceIdentity(int32_t serial) override;
    ::ndk::ScopedAStatus getHardwareConfig(int32_t serial) override;
    ::ndk::ScopedAStatus getModemActivityInfo(int32_t serial) override;
    ::ndk::ScopedAStatus getModemStackStatus(int32_t serial) override;
    ::ndk::ScopedAStatus getRadioCapability(int32_t serial) override;
    ::ndk::ScopedAStatus nvReadItem(
            int32_t serial, ::aidl::android::hardware::radio::modem::NvItem itemId) override;
    ::ndk::ScopedAStatus nvResetConfig(
            int32_t serial, ::aidl::android::hardware::radio::modem::ResetNvType type) override;
    ::ndk::ScopedAStatus nvWriteCdmaPrl(int32_t serial, const std::vector<uint8_t>& prl) override;
    ::ndk::ScopedAStatus nvWriteItem(
            int32_t serial, const ::aidl::android::hardware::radio::modem::NvWriteItem& i) override;
    ::ndk::ScopedAStatus requestShutdown(int32_t serial) override;
    ::ndk::ScopedAStatus responseAcknowledgement() override;
    ::ndk::ScopedAStatus sendDeviceState(
            int32_t serial, ::aidl::android::hardware::radio::modem::DeviceStateType stateType,
            bool state) override;
    ::ndk::ScopedAStatus setRadioCapability(
            int32_t s, const ::aidl::android::hardware::radio::modem::RadioCapability& rc) override;
    ::ndk::ScopedAStatus setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                       bool preferredForEmergencyCall) override;
    ::ndk::ScopedAStatus setResponseFunctions(
            const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemResponse>&
                    radioModemResponse,
            const std::shared_ptr<::aidl::android::hardware::radio::modem::IRadioModemIndication>&
                    radioModemIndication) override;

    GuaranteedCallback<::aidl::android::hardware::radio::modem::IRadioModemIndication,
                       ::aidl::android::hardware::radio::modem::IRadioModemIndicationDefault, true>
            indicate;
    GuaranteedCallback<::aidl::android::hardware::radio::modem::IRadioModemResponse,
                       ::aidl::android::hardware::radio::modem::IRadioModemResponseDefault>
            respond;

  private:
    int32_t mRatBitmap;

    std::string getModemUuid() const;
    std::string getSimUuid() const;
};

}  // namespace android::hardware::radio::minimal
