// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): AIDL interface implementations on top of libminradio
// (config/data/modem/network/sim, copied from hardware/interfaces/radio/aidl/minradio) and the
// generated not-supported bases for messaging/voice.
#pragma once

#include "ModemCore.h"
#include "gen/RadioMessagingBase.h"
#include "gen/RadioVoiceBase.h"

#include <libminradio/config/RadioConfig.h>
#include <libminradio/data/RadioData.h>
#include <libminradio/modem/RadioModem.h>
#include <libminradio/network/RadioNetwork.h>
#include <libminradio/sim/RadioSim.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>

namespace android::hardware::radio::a6l {

namespace aidlr = ::aidl::android::hardware::radio;

// QMI result -> RadioError (RadioSimModemConfig.cpp)
aidlr::RadioError toRadioError(const ::a6l::qmi::Result& r);

// Simple serial executor for slow requests (data calls, SMS sending) so the single binder thread
// never blocks for tens of seconds.
class Executor {
  public:
    explicit Executor(const char* name);
    void post(std::function<void()> fn);

  private:
    std::mutex mLock;
    std::condition_variable mCv;
    std::deque<std::function<void()>> mQ;
    std::thread mThread;
};

// ------------------------------------------------------------------ config
class A6lRadioConfig : public minimal::RadioConfig {
  protected:
    ::ndk::ScopedAStatus getSimSlotsStatus(int32_t serial) override;
};

// ------------------------------------------------------------------ modem
class A6lRadioModem : public minimal::RadioModem, public ModemCore::Listener {
  public:
    explicit A6lRadioModem(std::shared_ptr<minimal::SlotContext> context);

  protected:
    void onUpdatedResponseFunctions() override;
    ::ndk::ScopedAStatus getImei(int32_t serial) override;
    ::ndk::ScopedAStatus getBasebandVersion(int32_t serial) override;
    ::ndk::ScopedAStatus setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                       bool preferredForEmergencyCall) override;

    void onModemReady() override;
    void onModemLost() override;
    void onRadioPowerChanged(bool on) override;
};

// ------------------------------------------------------------------ sim
class A6lRadioSim : public minimal::RadioSim, public ModemCore::Listener {
  public:
    explicit A6lRadioSim(std::shared_ptr<minimal::SlotContext> context);

  protected:
    ::ndk::ScopedAStatus getIccCardStatus(int32_t serial) override;
    ::ndk::ScopedAStatus getImsiForApp(int32_t serial, const std::string& aid) override;
    ::ndk::ScopedAStatus iccIoForApp(int32_t serial, const aidlr::sim::IccIo& iccIo) override;
    ::ndk::ScopedAStatus supplyIccPinForApp(int32_t serial, const std::string& pin,
                                            const std::string& aid) override;
    ::ndk::ScopedAStatus supplyIccPukForApp(int32_t serial, const std::string& puk,
                                            const std::string& pin, const std::string& aid) override;
    ::ndk::ScopedAStatus changeIccPinForApp(int32_t serial, const std::string& oldPin,
                                            const std::string& newPin,
                                            const std::string& aid) override;
    ::ndk::ScopedAStatus getFacilityLockForApp(int32_t serial, const std::string& facility,
                                               const std::string& password, int32_t serviceClass,
                                               const std::string& appId) override;
    ::ndk::ScopedAStatus setFacilityLockForApp(int32_t serial, const std::string& facility,
                                               bool lockState, const std::string& passwd,
                                               int32_t serviceClass,
                                               const std::string& appId) override;

    void onModemReady() override;
    void onModemLost() override;
    void onSimChanged() override;

  private:
    ::a6l::qmi::uim::Session sessionFor(const std::string& aidHex);
};

// ------------------------------------------------------------------ network
class A6lRadioNetwork : public minimal::RadioNetwork, public ModemCore::Listener {
  public:
    using minimal::RadioNetwork::RadioNetwork;

  protected:
    ::ndk::ScopedAStatus getDataRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRegistrationState(int32_t serial) override;
    ::ndk::ScopedAStatus getSignalStrength(int32_t serial) override;
    ::ndk::ScopedAStatus getOperator(int32_t serial) override;
    ::ndk::ScopedAStatus getVoiceRadioTechnology(int32_t serial) override;
    ::ndk::ScopedAStatus getNetworkSelectionMode(int32_t serial) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeAutomatic(int32_t serial) override;
    ::ndk::ScopedAStatus setNetworkSelectionModeManual(int32_t serial,
                                                       const std::string& operatorNumeric,
                                                       aidlr::AccessNetwork ran) override;
    ::ndk::ScopedAStatus getAllowedNetworkTypesBitmap(int32_t serial) override;
    ::ndk::ScopedAStatus setAllowedNetworkTypesBitmap(int32_t serial,
                                                      int32_t networkTypeBitmap) override;

    void onModemReady() override;
    void onModemLost() override;
    void onNetworkChanged() override;
    void onSignalChanged() override;
    void onNitz(const std::string& nitz, int64_t receivedMs) override;

  private:
    aidlr::network::RegStateResult buildRegState(bool voice);
    aidlr::network::SignalStrength buildSignal(const ::a6l::qmi::nas::SignalInfo& s);
    aidlr::RadioTechnology currentRat();
    int32_t mAllowedBitmap = 0;
};

// ------------------------------------------------------------------ data
class A6lRadioData : public minimal::RadioData, public ModemCore::Listener {
  public:
    using minimal::RadioData::RadioData;

  protected:
    ::ndk::ScopedAStatus setupDataCall(
            int32_t serial, aidlr::AccessNetwork accessNetwork,
            const aidlr::data::DataProfileInfo& dataProfileInfo, bool roamingAllowed,
            aidlr::data::DataRequestReason reason,
            const std::vector<aidlr::data::LinkAddress>& addresses,
            const std::vector<std::string>& dnses, int32_t pduSessionId,
            const std::optional<aidlr::data::SliceInfo>& sliceInfo,
            bool matchAllRuleAllowed) override;
    ::ndk::ScopedAStatus deactivateDataCall(int32_t serial, int32_t cid,
                                            aidlr::data::DataRequestReason reason) override;
    ::ndk::ScopedAStatus setInitialAttachApn(
            int32_t serial,
            const std::optional<aidlr::data::DataProfileInfo>& dpInfo) override;

    void onModemLost() override;
    void onDataCallLost(int cid) override;

  private:
    Executor mExec{"a6l-data"};
};

// ------------------------------------------------------------------ messaging
class A6lRadioMessaging : public RadioMessagingBase, public ModemCore::Listener {
  public:
    using RadioMessagingBase::RadioMessagingBase;

  protected:
    ::ndk::ScopedAStatus sendSms(int32_t serial, const aidlr::messaging::GsmSmsMessage& message) override;
    ::ndk::ScopedAStatus sendSmsExpectMore(int32_t serial,
                                           const aidlr::messaging::GsmSmsMessage& message) override;
    ::ndk::ScopedAStatus acknowledgeLastIncomingGsmSms(
            int32_t serial, bool success, aidlr::messaging::SmsAcknowledgeFailCause cause) override;
    ::ndk::ScopedAStatus getSmscAddress(int32_t serial) override;
    ::ndk::ScopedAStatus reportSmsMemoryStatus(int32_t serial, bool available) override;
    ::ndk::ScopedAStatus setGsmBroadcastActivation(int32_t serial, bool activate) override;
    ::ndk::ScopedAStatus setGsmBroadcastConfig(
            int32_t serial,
            const std::vector<aidlr::messaging::GsmBroadcastSmsConfigInfo>& configInfo) override;
    ::ndk::ScopedAStatus getGsmBroadcastConfig(int32_t serial) override;

    void onNewSms(const std::vector<uint8_t>& pdu, bool statusReport) override;

  private:
    void send(int32_t serial, const aidlr::messaging::GsmSmsMessage& message, bool more);
    Executor mExec{"a6l-sms"};
};

// ------------------------------------------------------------------ voice
class A6lRadioVoice : public RadioVoiceBase, public ModemCore::Listener {
  public:
    using RadioVoiceBase::RadioVoiceBase;

  protected:
    void onUpdatedResponseFunctions() override;
    ::ndk::ScopedAStatus getCurrentCalls(int32_t serial) override;
    ::ndk::ScopedAStatus dial(int32_t serial, const aidlr::voice::Dial& dialInfo) override;
    ::ndk::ScopedAStatus emergencyDial(int32_t serial, const aidlr::voice::Dial& dialInfo,
                                       int32_t categories, const std::vector<std::string>& urns,
                                       aidlr::voice::EmergencyCallRouting routing,
                                       bool hasKnownUserIntentEmergency, bool isTesting) override;
    ::ndk::ScopedAStatus hangup(int32_t serial, int32_t gsmIndex) override;
    ::ndk::ScopedAStatus hangupWaitingOrBackground(int32_t serial) override;
    ::ndk::ScopedAStatus hangupForegroundResumeBackground(int32_t serial) override;
    ::ndk::ScopedAStatus switchWaitingOrHoldingAndActive(int32_t serial) override;
    ::ndk::ScopedAStatus conference(int32_t serial) override;
    ::ndk::ScopedAStatus explicitCallTransfer(int32_t serial) override;
    ::ndk::ScopedAStatus separateConnection(int32_t serial, int32_t gsmIndex) override;
    ::ndk::ScopedAStatus acceptCall(int32_t serial) override;
    ::ndk::ScopedAStatus rejectCall(int32_t serial) override;
    ::ndk::ScopedAStatus sendDtmf(int32_t serial, const std::string& s) override;
    ::ndk::ScopedAStatus startDtmf(int32_t serial, const std::string& s) override;
    ::ndk::ScopedAStatus stopDtmf(int32_t serial) override;
    ::ndk::ScopedAStatus getLastCallFailCause(int32_t serial) override;
    ::ndk::ScopedAStatus getMute(int32_t serial) override;
    ::ndk::ScopedAStatus setMute(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus getTtyMode(int32_t serial) override;
    ::ndk::ScopedAStatus setTtyMode(int32_t serial, aidlr::voice::TtyMode mode) override;
    ::ndk::ScopedAStatus getPreferredVoicePrivacy(int32_t serial) override;
    ::ndk::ScopedAStatus setPreferredVoicePrivacy(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus isVoNrEnabled(int32_t serial) override;
    ::ndk::ScopedAStatus exitEmergencyCallbackMode(int32_t serial) override;

    void onModemLost() override;
    void onCallsChanged(bool incomingRinging) override;

  private:
    std::optional<uint8_t> findCall(std::initializer_list<uint8_t> states);
    bool mMute = false;
    aidlr::voice::TtyMode mTty = aidlr::voice::TtyMode::OFF;
    bool mPrivacy = false;
    Executor mExec{"a6l-voice"};
};

}  // namespace android::hardware::radio::a6l
