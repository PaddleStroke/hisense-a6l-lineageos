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
#include <a6lqmi/ims.h>
#include <a6lqmi/multisim.h>
#include <a6lqmi/services.h>
#include <a6lqmi/ussd.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <set>
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
        // receivedMs: CLOCK_BOOTTIME ms (TimePolicy.h bootTimeMs) when the QMI indication arrived (r5 round8 F61)
        virtual void onNitz(const std::string& /*nitz*/, int64_t /*receivedMs*/) {}
        virtual void onCallsChanged(bool /*incomingRinging*/) {}
        virtual void onNewSms(const std::vector<uint8_t>& /*pdu*/, bool /*statusReport*/) {}
        // r5 deep F25: 3GPP cell broadcast page(s) / ETWS notification, as Android newBroadcastSms wants them
        virtual void onNewBroadcastSms(const std::vector<uint8_t>& /*data*/) {}
        // r5 review F28: `generation` identifies the connection (cids are reused); pass both to
        // DataCallManager::deactivateIfCurrent() so a late loss never removes a newer call.
        virtual void onDataCallLost(int /*cid*/, uint64_t /*generation*/) {}
        // r5 round8 F59: the modem asked for (cid, generation) to be reconfigured: DataCallManager::refresh() on a worker
        virtual void onDataCallReconfigured(int /*cid*/, uint64_t /*generation*/) {}
        virtual void onRadioPowerChanged(bool /*on*/) {}
        virtual void onImsChanged() {}  // volte2: IMSA registration / service status changed
        // telephony-flows (29 Sep 2026): USSD indication / release / Originate-No-Wait result (worker thread)
        virtual void onUssd(const ::a6l::qmi::voice::ussd::Event& /*e*/) {}
    };

    // ril3 (DSDS): one core per logical slot (1-based like SlotContext / "slot1", "slot2"). Slot 1
    // = primary subscription and is exactly the v1 single-SIM core; slot N>1 owns its own QRTR
    // client whose NAS/WMS/VOICE/DMS are bound to the secondary subscription.
    static ModemCore& get(unsigned slot = 1);
    static int slotCount();              // ro.vendor.a6l.ril.slots (1|2), else multisim config
    static ::a6l::qmi::multisim::PowerVote& powerVote();
    // Host tests (tests/modemcore_tests.cc) inject the fake modem; default = AF_QIPCRTR socket.
    using TransportFactory = std::function<std::unique_ptr<::a6l::qmi::Transport>()>;
    static void setTransportFactory(TransportFactory f);
    int sub() const { return mSub; }     // 0 primary, 1 secondary
    // false when slot N>1 could not bind its services (modem in single standby / firmware without
    // DSDS): the slot then reports NOT_REG and refuses calls/SMS instead of using SIM 1.
    bool bound() const { return mBound; }

    // Card view for this slot (copied out of the card status cache).
    struct SlotSim {
        bool valid = false;               // card status known
        int card = -1;                    // physical UIM slot (0-based)
        std::optional<::a6l::qmi::uim::Card> cardInfo;
        int gwAppIndex = -1;
        bool provisioned = false;
        uint8_t provSession = 0, cardSession = 6, nonProvSession = 4;
        std::vector<uint8_t> gwAid;
    };
    SlotSim slotSim(bool refresh = false);

    // pinsafe (27 Sep 2026): the modem does not always activate the primary GW provisioning session
    // (gw_primary=0xffff, USIM app DETECTED) and VERIFY PIN then fails INTERNAL before reaching the
    // card; LOW_POWER -> ONLINE also powers the SIM down (PIN needed again). The HAL now activates the
    // session itself (slot 1 too) on every card status change and after radio power changes, and
    // guards every PIN it forwards: the card must say PIN required with >= 1 attempt left, the PIN
    // must be 4-8 digits, and a PIN the card already rejected is never sent again automatically.
    enum class PinVerdict { Send, NoCard, NotProvisioned, AlreadyReady, Blocked, NotPinState, BadFormat, RepeatedWrong };
    struct PinCheck {
        PinVerdict verdict = PinVerdict::NoCard;
        int retries = -1;  // PIN1 (or UPIN) attempts left as reported by the card, -1 unknown
        std::string why;
        // r5 round5 F44: the credential the verdict was computed for (UPIN when the app says UPIN replaces PIN1).
        // The HAL sends exactly this identifier, so validation, command and returned counters agree.
        uint8_t pinId = ::a6l::qmi::uim::kPin1;
    };
    // r5 round5 F44: PIN identifier for the app `aidHex` of this subscription's card (empty = its GW app):
    // kUpin when that app has UPIN replacing PIN1, else kPin1. nullopt: no card / no such app on the card (the
    // HAL then answers without sending anything).
    static std::optional<uint8_t> pinIdFor(const ::a6l::qmi::uim::CardStatus& cs, int sub, const std::string& aidHex);
    // pinIdFor() on a fresh card status (PUK / change PIN / SC lock, which have no preflight of their own)
    std::optional<uint8_t> pinTarget(const std::string& aidHex);
    // Pure decision on a card status snapshot (host-tested in tests/simprov_tests.cc).
    static PinCheck checkPin(const ::a6l::qmi::uim::CardStatus& cs, int sub, const std::string& pin);
    // checkPin() on a fresh card status, activating the provisioning session first when needed
    // (waits up to waitMs for the app to leave DETECTED); also applies the repeated-wrong-PIN guard.
    PinCheck pinPreflight(const std::string& pin, int waitMs = 5000);
    // Record the outcome of a VERIFY PIN the HAL sent (wrong PINs are remembered per ICCID).
    void notePinResult(const std::string& pin, bool accepted);
    // Activates this slot's GW provisioning session when the card has an app but the modem did not
    // attach it. Rate limited (5 s apart, 3 tries per SIM power cycle). Returns true if a request was sent.
    bool provisionIfNeeded(bool force = false);

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

    // voice RAT of the ongoing call(s) (from NAS serving system during a call), logged as A6L_RIL_CALL_RAT
    std::string callRat() const;

    // r5 review round7 F58: cause of the most recently terminated call (Android LastCallFailCause value, mapped from the
    // QMI call end reason of All Call Status TLV 0x14; VOICE/modem loss with calls -> RADIO_INTERNAL_ERROR; unknown ->
    // ERROR_UNSPECIFIED; nothing terminated yet -> ERROR_UNSPECIFIED). vendor = raw QMI reason / loss for diagnostics.
    struct LastCallFail {
        int cause = 0xffff;
        std::string vendor;
    };
    LastCallFail lastCallFail();
    int lastCallFailCause() { return lastCallFail().cause; }

    // SMS acknowledgement (r5 deep F26/F27): one ordered queue owns every MT SMS (transfer or stored route) together
    // with its acknowledgement identity (WMS transaction or storage index + modem generation). Only its head is
    // delivered to Android (IRadioMessagingIndication: no newSms until the previous one is acknowledged); the ack
    // applies to exactly that message: transfer -> WMS Send Ack, stored -> delete only after a positive ack.
    bool ackLastSms(bool success, uint8_t rpCause, uint8_t tpCause);
    // Framework (re)connected: an unacknowledged delivered message is delivered again (its old client never acked).
    void smsClientReconnected();
    size_t pendingSmsCount();  // host tests
    // r5 round11 F17 follow-up: VOICE/WMS services whose bind/registration is not acknowledged yet (host tests, dumps)
    std::set<uint32_t> pendingServiceSetup() const;

    ::a6l::radio::DataCallManager& data() { return *mData; }

    // volte2 (25 Sep 2026): modem IMS state from IMSA (QMI 33; published by the modem only when its
    // IMS task runs). Slot 1 only. Logged as A6L_RIL_IMS; Android sees it through
    // getImsRegistrationState / imsNetworkStateChanged (transparent VoLTE, no ImsService).
    struct ImsState {
        bool present = false;  // IMSA service published
        ::a6l::qmi::imsa::RegStatus reg;
        ::a6l::qmi::imsa::ServicesStatus services;
        bool registered() const { return present && reg.registered(); }
        bool voiceOverIms() const { return registered() && services.voiceAvailable(); }
    };
    ImsState imsState() const;

    // Properties (with defaults) read once at start
    struct Config {
        std::string dataParent = "rmnet_ipa0";
        // IPA v2.6L (SDM660) modem endpoints have QMAP but no checksum offload (msm8953 v2.6L data):
    // ingress deaggregation only. Mainline IPA v3.1+ would use 0x0d (+MAPv4 checksum).
    uint32_t rmnetFlags = 0x01;
        uint32_t epType = 4, epIface = 1;
        // data2: DPM Open Port before WDA (SDM660 needs it) + WDA aggregation (5 = QMAP)
        bool dpmOpenPort = true;
        uint32_t wdaAgg = 5;
        bool smsStoreRoute = false;
        int logLevel = 2;
    };
    const Config& config() const { return mCfg; }

  private:
    explicit ModemCore(int sub);
    void connectLoop();
    bool initModem();
    void workerLoop();
    void wireIndications();
    void onServiceChange(uint32_t svc, bool up);
    template <typename F>
    void each(F f);

    void logCallRat(const std::vector<qmi::voice::CallInfo>& calls);
    void postDelayed(int ms, std::function<void()> fn);  // pinsafe: re-check the SIM after power changes
    void imsaChanged(bool up);  // volte2 (worker thread)
    // r5 review F17: VOICE or WMS withdrawn alone (DMS/UIM/NAS still there): invalidate that service's state; when
    // it comes back (worker thread) re-bind (slot 2) and re-register it.
    void serviceLost(uint32_t svc);
    void restoreService(uint32_t svc);
    // r5 round11 F17 follow-up (28 Sep 2026): a VOICE/WMS setup step the modem rejected stays pending (mLock) until it
    // is acknowledged, instead of being logged and forgotten. Startup (initModem) and isolated restoration share it;
    // the worker retries due steps with bounded exponential backoff (1 s .. 60 s). A service loss or a modem re-init
    // bumps that service's epoch: an attempt started before is discarded and the loss/re-init path owns it again.
    // Steps the modem refuses as unsupported (INVALID_QMI_COMMAND / NOT_SUPPORTED / DEVICE_UNSUPPORTED) are optional:
    // logged once, not retried.
    enum SetupStep : unsigned { kStepBind = 1, kStepRegister = 2, kStepEvents = 4 };
    struct PendingSetup {
        unsigned steps = 0;
        int attempts = 0;
        std::chrono::steady_clock::time_point due{};
    };
    unsigned setupService(uint32_t svc, unsigned steps);  // QMI thread; returns the steps still to retry
    void notePendingSetup(uint32_t svc, unsigned remaining, uint64_t epoch);  // after an attempt
    void reconcileServices();  // worker thread
    void maybeScheduleReconcile();  // connect loop, core ready
    // r5 review F18: set this slot's radio state; publish mRadioOn for every slot from a DMS mode (worker/binder).
    void publishModemMode(bool online);

    const int mSub;
    bool mBound = true;
    Config mCfg;
    std::unique_ptr<qmi::Client> mCtl;
    std::unique_ptr<::a6l::radio::DataCallManager> mData;
    std::thread mConnThread, mWorker;

    mutable std::mutex mLock;
    std::condition_variable mCv;
    bool mReady = false;
    bool mEverReady = false;
    bool mNeedsInit = true;
    std::set<uint32_t> mSvcLost;  // r5 F17: VOICE/WMS withdrawn while the rest stayed (mLock)
    std::map<uint32_t, PendingSetup> mSetupPending;  // r5 round11 F17 follow-up (mLock)
    std::map<uint32_t, uint64_t> mSvcEpoch;           // bumped on loss / re-init (mLock)
    bool mReconcileQueued = false;                    // (mLock)
    uint64_t mLossEpoch = 0;  // r5 bug hunt round2 R1: bumped on every DMS/UIM/NAS loss (mLock)
    std::vector<Listener*> mListeners;
    std::vector<std::function<void()>> mFirstReady;

    std::mutex mWorkLock;
    std::condition_variable mWorkCv;
    std::deque<std::function<void()>> mWork;

    // caches
    std::mutex mCacheLock;
    std::optional<qmi::uim::CardStatus> mCard;
    std::string mIccid;
    int mIccidCard = -1;  // r5 round5 F43: physical card mIccid was read from (mCacheLock)
    std::optional<qmi::nas::ServingSystem> mServing;
    qmi::nas::SignalInfo mSignal;
    std::optional<qmi::nas::OperatorName> mOpName;
    std::vector<qmi::voice::CallInfo> mCalls;
    std::optional<qmi::dms::Ids> mIds;
    std::string mRevision;
    std::optional<bool> mRadioOn;
    LastCallFail mLastCallFail;          // r5 round7 F58 (mCacheLock)
    std::set<uint8_t> mEndRecorded;      // call ids whose termination is recorded (mCacheLock)
    void recordCallEnds(const std::vector<qmi::voice::CallInfo>& calls,
                        const std::vector<std::pair<uint8_t, uint16_t>>& reasons);  // mCacheLock held
    void recordCallsLost(const char* why);                                          // mCacheLock held
    bool mInCall = false;
    std::string mCallRat;
    std::map<uint8_t, std::string> mCallDomain;  // volte3: call id -> "ims"/"ims-wlan"/"cs" (guarded by mCacheLock)
    ImsState mIms;  // volte2, mCacheLock
    // r5 review F29: cache generations (mCacheLock). Every indication write and every reset bumps the generation;
    // a synchronous query started before that is discarded instead of restoring older modem state.
    uint64_t mCardGen = 0, mServingGen = 0, mOpNameGen = 0, mCallsGen = 0, mIccidGen = 0;

    struct SmsDelivery {
        std::vector<uint8_t> pdu;  // SMSC + TPDU
        bool statusReport = false;
        uint32_t txn = 0;
        bool needed = false;  // transfer route with ack indicator SEND
        qmi::wms::AckOptions opts;
        std::optional<std::pair<uint8_t, uint32_t>> stored;  // store-and-notify: storage, index (deleted after ack)
        uint64_t gen = 0;        // modem generation at reception
        bool delivered = false;  // handed to Android, ack outstanding
    };
    void enqueueSms(SmsDelivery d);  // worker thread
    void pumpSms();                  // worker thread: deliver the head if not yet delivered
    void invalidateSmsTransactions();  // modem / WMS instance lost (any thread, no QMI)
    // pinsafe (guarded by mCacheLock)
    std::chrono::steady_clock::time_point mLastProvision{};
    int mProvisionTries = 0;
    size_t mWrongPinHash = 0;
    std::string mWrongPinIccid;

    std::mutex mSmsLock;
    std::deque<SmsDelivery> mSmsQueue;
    uint64_t mSmsGen = 0;  // bumped when the modem is lost (old transactions/indexes are void)
};

}  // namespace android::hardware::radio::a6l
