// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): AIDL interface implementations on top of libminradio
// (config/data/modem/network/sim, copied from hardware/interfaces/radio/aidl/minradio) and the
// generated not-supported bases for messaging/voice.
#pragma once

#include "ModemCore.h"
#include "TelephonyFlows.h"
#include "gen/RadioMessagingBase.h"
#include "gen/RadioVoiceBase.h"

#include <libminradio/config/RadioConfig.h>
#include <libminradio/data/RadioData.h>
#include <libminradio/modem/RadioModem.h>
#include <libminradio/network/RadioNetwork.h>
#include <libminradio/sim/RadioSim.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <set>
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
    // ril3 (DSDS): one entry per physical slot, phone count / capability from ModemCore::slotCount()
    ::ndk::ScopedAStatus getSimSlotsStatus(int32_t serial) override;
    ::ndk::ScopedAStatus getNumOfLiveModems(int32_t serial) override;
    ::ndk::ScopedAStatus getPhoneCapability(int32_t serial) override;
    ::ndk::ScopedAStatus setNumOfLiveModems(int32_t serial, int8_t numOfLiveModems) override;
    ::ndk::ScopedAStatus setPreferredDataModem(int32_t serial, int8_t modemId) override;
};

// ------------------------------------------------------------------ modem
class A6lRadioModem : public minimal::RadioModem, public ModemCore::Listener {
  public:
    ModemCore& slotCore() { return ModemCore::get(mContext->getSlotIndex()); }
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
    ModemCore& slotCore() { return ModemCore::get(mContext->getSlotIndex()); }
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

    // r5 round5 F42: applied state only (disable not supported)
    ::ndk::ScopedAStatus enableUiccApplications(int32_t serial, bool enable) override;
    ::ndk::ScopedAStatus areUiccApplicationsEnabled(int32_t serial) override;
    // r5 round5 F47: never the inherited SIM emulator (REQUEST_NOT_SUPPORTED until UIM APDU transport exists)
    ::ndk::ScopedAStatus iccOpenLogicalChannel(int32_t serial, const std::string& aid, int32_t p2) override;
    ::ndk::ScopedAStatus iccCloseLogicalChannelWithSessionInfo(
            int32_t serial, const aidlr::sim::SessionInfo& sessionInfo) override;
    ::ndk::ScopedAStatus iccTransmitApduBasicChannel(int32_t serial, const aidlr::sim::SimApdu& message) override;
    ::ndk::ScopedAStatus iccTransmitApduLogicalChannel(int32_t serial, const aidlr::sim::SimApdu& message) override;

    void onModemReady() override;
    void onModemLost() override;
    void onSimChanged() override;

  private:
    ::a6l::qmi::uim::Session sessionFor(const std::string& aidHex);
    // r5 round5 F44: effective PIN id of the app (PIN1/UPIN); nullopt with *err set when nothing may be sent
    std::optional<uint8_t> pinIdOrError(const std::string& aid, aidlr::RadioError* err);
};

// ------------------------------------------------------------------ network
class A6lRadioNetwork : public minimal::RadioNetwork, public ModemCore::Listener {
  public:
    ModemCore& slotCore() { return ModemCore::get(mContext->getSlotIndex()); }
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
    // volte2: modem IMS registration (IMSA), deprecated in AIDL but still answered
    ::ndk::ScopedAStatus getImsRegistrationState(int32_t serial) override;

    void onModemReady() override;
    void onModemLost() override;
    void onNetworkChanged() override;
    void onImsChanged() override;
    void onSignalChanged() override;
    void onNitz(const std::string& nitz, int64_t receivedMs) override;

  private:
    aidlr::network::RegStateResult buildRegState(bool voice);
    aidlr::network::SignalStrength buildSignal(const ::a6l::qmi::nas::SignalInfo& s);
    aidlr::RadioTechnology currentRat();
    std::atomic<int32_t> mAllowedBitmap{0};  // r5 round5 F45: applied subset, 0 = ask the modem
};

// ------------------------------------------------------------------ data
class A6lRadioData : public minimal::RadioData, public ModemCore::Listener {
  public:
    ModemCore& slotCore() { return ModemCore::get(mContext->getSlotIndex()); }
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
    void onDataCallLost(int cid, uint64_t generation) override;
    void onDataCallReconfigured(int cid, uint64_t generation) override;  // r5 round8 F59
    void onNetworkChanged() override;  // r5 F9: roaming transitions

  private:
    Executor mExec{"a6l-data"};
    // r5 F9: cids set up with roamingAllowed=false (non-emergency): torn down if the serving network turns roaming
    std::mutex mRoamLock;
    std::set<int32_t> mNoRoamCids;
};

// ------------------------------------------------------------------ messaging
class A6lRadioMessaging : public RadioMessagingBase, public ModemCore::Listener {
  public:
    ModemCore& slotCore() { return ModemCore::get(mContext->getSlotIndex()); }
    using RadioMessagingBase::RadioMessagingBase;

  protected:
    ::ndk::ScopedAStatus sendSms(int32_t serial, const aidlr::messaging::GsmSmsMessage& message) override;
    ::ndk::ScopedAStatus sendSmsExpectMore(int32_t serial,
                                           const aidlr::messaging::GsmSmsMessage& message) override;
    ::ndk::ScopedAStatus acknowledgeLastIncomingGsmSms(
            int32_t serial, bool success, aidlr::messaging::SmsAcknowledgeFailCause cause) override;
    ::ndk::ScopedAStatus getSmscAddress(int32_t serial) override;
    // volte3: the framework sends SMS through sendImsSms once getImsRegistrationState says registered (volte2)
    ::ndk::ScopedAStatus sendImsSms(int32_t serial, const aidlr::messaging::ImsSmsMessage& message) override;
    ::ndk::ScopedAStatus reportSmsMemoryStatus(int32_t serial, bool available) override;
    ::ndk::ScopedAStatus setGsmBroadcastActivation(int32_t serial, bool activate) override;
    ::ndk::ScopedAStatus setGsmBroadcastConfig(
            int32_t serial,
            const std::vector<aidlr::messaging::GsmBroadcastSmsConfigInfo>& configInfo) override;
    ::ndk::ScopedAStatus getGsmBroadcastConfig(int32_t serial) override;

    void onNewSms(const std::vector<uint8_t>& pdu, bool statusReport) override;
    void onNewBroadcastSms(const std::vector<uint8_t>& data) override;  // r5 deep F25
    void onUpdatedResponseFunctions() override;  // r5 deep F27: re-deliver an SMS the old client never acked

  private:
    void send(int32_t serial, const aidlr::messaging::GsmSmsMessage& message, bool more, bool ims = false);
    Executor mExec{"a6l-sms"};
    // r5 F12: last cell broadcast configuration the modem accepted (getGsmBroadcastConfig)
    std::mutex mCbLock;
    std::vector<aidlr::messaging::GsmBroadcastSmsConfigInfo> mCbConfig;
};

// ------------------------------------------------------------------ voice
class A6lRadioVoice : public RadioVoiceBase, public ModemCore::Listener {
  public:
    ModemCore& slotCore() { return ModemCore::get(mContext->getSlotIndex()); }
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
    // telephony-flows (29 Sep 2026): USSD over QMI VOICE (flows::UssdSession decides Originate vs Answer)
    ::ndk::ScopedAStatus sendUssd(int32_t serial, const std::string& ussd) override;
    ::ndk::ScopedAStatus cancelPendingUssd(int32_t serial) override;

    void onModemLost() override;
    void onCallsChanged(bool incomingRinging) override;
    void onUssd(const ::a6l::qmi::voice::ussd::Event& e) override;
    // telephony-flows: the emergency number list depends on SIM presence and the serving MCC (flows::emergencyNumbers)
    void onSimChanged() override;
    void onNetworkChanged() override;

  private:
    std::optional<uint8_t> findCall(std::initializer_list<uint8_t> states);
    bool mMute = false;  // r5 F6: last state a6l-q6voiced applied (fallback for getMute)
    aidlr::voice::TtyMode mTty = aidlr::voice::TtyMode::OFF;
    bool mPrivacy = false;
    Executor mExec{"a6l-voice"};
    // telephony-flows: a sync Originate USSD fallback may block up to 100 s, never on the call executor
    Executor mUssdExec{"a6l-ussd"};
    flows::UssdSession mUssd;
    void reportUssd(const flows::UssdReport& r);
    // last emergency number list sent (re-sent only when it changes, and always on a new client)
    std::mutex mEccLock;
    std::optional<std::vector<flows::EccEntry>> mEccSent;
    void publishEmergencyNumbers(bool force);
};

}  // namespace android::hardware::radio::a6l
