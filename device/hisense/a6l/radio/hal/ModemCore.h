// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): shared modem state for all AIDL radio interfaces.
//
// One control QMI client (DMS, UIM, NAS, WMS, VOICE, WDA) + a worker thread. The core waits for the
// modem's QMI services (the modem is started by a6l-modem.sh only when
// persist.vendor.a6l.radio.enable=1), registers indications, caches state and fans events out to
// the interface objects through Listener callbacks (always called on the worker thread, never on a
// QMI dispatch thread, so listeners may issue QMI requests).
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/datacall.h>
#include <a6lqmi/services.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace android::hardware::radio::a6l {

namespace qmi = ::a6l::qmi;

class ModemCore {
  public:
    struct Listener {
        virtual ~Listener() = default;
        virtual void onModemReady() {}
        virtual void onModemLost() {}
        virtual void onSimChanged() {}
        virtual void onNetworkChanged() {}
        virtual void onSignalChanged() {}
        virtual void onNitz(const std::string& /*nitz*/, int64_t /*receivedMs*/) {}
        virtual void onCallsChanged(bool /*incomingRinging*/) {}
        virtual void onNewSms(const std::vector<uint8_t>& /*pdu*/, bool /*statusReport*/) {}
        virtual void onDataCallLost(int /*cid*/) {}
        virtual void onRadioPowerChanged(bool /*on*/) {}
    };

    static ModemCore& get();

    void start();  // non-blocking; the connection thread keeps retrying forever
    // Called once, the first time the modem is ready (used to call SlotContext::setConnected()).
    void onFirstReady(std::function<void()> fn);
    void addListener(Listener* l);

    bool ready() const;
    qmi::Client& ctl() { return *mCtl; }
    void post(std::function<void()> fn);  // run on the worker thread

    // Cached state (refreshed on indications; getters may refresh synchronously when stale)
    std::optional<qmi::uim::CardStatus> cardStatus(bool refresh = false);
    std::string iccid(bool refresh = false);
    std::optional<qmi::nas::ServingSystem> serving(bool refresh = false);
    qmi::nas::SignalInfo signal(bool refresh = false);
    qmi::nas::OperatorName operatorName(bool refresh = false);
    std::vector<qmi::voice::CallInfo> calls(bool refresh = false);
    qmi::dms::Ids ids();
    std::string revision();
    bool radioOn();
    bool setRadioPower(bool on);

    // Last call end cause (3GPP CC cause), best effort
    int lastCallFailCause() const { return mLastCallFailCause; }

    // SMS acknowledgement bookkeeping (transfer-route messages need a WMS Send Ack)
    bool ackLastSms(bool success, uint8_t rpCause, uint8_t tpCause);

    ::a6l::radio::DataCallManager& data() { return *mData; }

    // Properties (with defaults) read once at start
    struct Config {
        std::string dataParent = "rmnet_ipa0";
        // IPA v2.6L (SDM660) modem endpoints have QMAP but no checksum offload (msm8953 v2.6L data):
    // ingress deaggregation only. Mainline IPA v3.1+ would use 0x0d (+MAPv4 checksum).
    uint32_t rmnetFlags = 0x01;
        uint32_t epType = 4, epIface = 1;
        bool smsStoreRoute = false;
        int logLevel = 2;
    };
    const Config& config() const { return mCfg; }

  private:
    ModemCore();
    void connectLoop();
    bool initModem();
    void workerLoop();
    void wireIndications();
    void onServiceChange(uint32_t svc, bool up);
    template <typename F>
    void each(F f);

    Config mCfg;
    std::unique_ptr<qmi::Client> mCtl;
    std::unique_ptr<::a6l::radio::DataCallManager> mData;
    std::thread mConnThread, mWorker;

    mutable std::mutex mLock;
    std::condition_variable mCv;
    bool mReady = false;
    bool mEverReady = false;
    bool mNeedsInit = true;
    std::vector<Listener*> mListeners;
    std::vector<std::function<void()>> mFirstReady;

    std::mutex mWorkLock;
    std::condition_variable mWorkCv;
    std::deque<std::function<void()>> mWork;

    // caches
    std::mutex mCacheLock;
    std::optional<qmi::uim::CardStatus> mCard;
    std::string mIccid;
    std::optional<qmi::nas::ServingSystem> mServing;
    qmi::nas::SignalInfo mSignal;
    std::optional<qmi::nas::OperatorName> mOpName;
    std::vector<qmi::voice::CallInfo> mCalls;
    std::optional<qmi::dms::Ids> mIds;
    std::string mRevision;
    std::optional<bool> mRadioOn;
    int mLastCallFailCause = 16;  // NORMAL_CLEARING

    std::mutex mSmsLock;
    std::deque<std::pair<uint32_t, bool>> mSmsAcks;  // txn, needs ack
};

}  // namespace android::hardware::radio::a6l
