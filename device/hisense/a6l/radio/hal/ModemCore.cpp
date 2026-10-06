// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): shared modem state.
#define LOG_TAG "a6l-radio"
#include "ModemCore.h"
#include "TimePolicy.h"

#include <a6lqmi/log.h>
#include <a6lqmi/sms.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>

namespace android::hardware::radio::a6l {

using namespace ::a6l::qmi;
using ::android::base::GetBoolProperty;
using ::android::base::GetIntProperty;
using ::android::base::GetProperty;
using namespace std::chrono_literals;

namespace {
const std::vector<uint32_t> kCore = {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice};
const std::vector<uint32_t> kAll = {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda,
                                   kSvcImsa};  // volte2: IMSA only looked up (optional)

// rom-v2 (agent merge, 25 Sep 2026): in-call audio glue. a6l-q6voiced (kvoice) holds the VoiceMMode1 PCMs, i.e. starts
// the CVD/MVM voice session in the ADSP, while vendor.a6l.voice.active=1; a6l-audio-route switches the codec to the
// voice paths on the same property. A call needs the voice path from dialing (early media / ringback) until it ends;
// a ringing incoming call (ringtone = media path) and a waiting call do not.
bool callNeedsVoicePath(uint8_t st) {
    return st == voice::kStateOrigination || st == voice::kStateConversation || st == voice::kStateCcInProgress ||
           st == voice::kStateAlerting || st == voice::kStateHold || st == voice::kStateSetup;
}

// ril3: aggregated over both subscriptions (a call on slot 2 must not be cut by slot 1's empty list)
void publishVoiceActive(int sub, const std::vector<voice::CallInfo>& calls) {
    static std::mutex sLock;
    static int sLast = -1;
    static bool sSub[multisim::kMaxSlots] = {false, false};
    std::lock_guard<std::mutex> g(sLock);
    bool mine = false;
    bool ims = false;
    for (auto& c : calls)
        if (callNeedsVoicePath(c.state)) {
            mine = true;
            if (voice::isImsCall(c)) ims = true;
        }
    if (sub >= 0 && sub < multisim::kMaxSlots) sSub[sub] = mine;
    int on = 0;
    for (bool b : sSub)
        if (b) on = 1;
    // volte3: call domain for the audio side (set BEFORE voice.active so the MVM session choice is ready when
    // a6l-q6voiced opens the PCMs). Only acted upon when persist.vendor.a6l.voice.session_switch=1 (a6l-q6voiced.rc).
    static int sLastIms = -1;
    if (mine && static_cast<int>(ims) != sLastIms) {
        sLastIms = ims;
        ::android::base::SetProperty("vendor.a6l.voice.domain", ims ? "ims" : "cs");
        LOG(INFO) << "A6L_RIL voice domain " << (ims ? "ims" : "cs");
    }
    if (!on) sLastIms = -1;
    if (on == sLast) return;
    sLast = on;
    ::android::base::SetProperty("vendor.a6l.voice.active", on ? "1" : "0");
    LOG(INFO) << "A6L_RIL voice path " << (on ? "ON" : "OFF") << " (" << calls.size() << " call(s))";
}

bool isStatusReport(const std::vector<uint8_t>& pdu) {
    if (pdu.empty()) return false;
    size_t smsc = pdu[0];
    if (smsc + 1 >= pdu.size()) return false;
    return (pdu[smsc + 1] & 0x03) == 0x02;
}
}  // namespace

ModemCore& ModemCore::get(unsigned slot) {
    // never destroyed (the service lives forever)
    static ModemCore* sCores[multisim::kMaxSlots] = {new ModemCore(0), new ModemCore(1)};
    if (slot < 1 || slot > multisim::kMaxSlots) slot = 1;
    return *sCores[slot - 1];
}

int ModemCore::slotCount() {
    static const int n = multisim::slotCountFromConfig(GetProperty("persist.radio.multisim.config", ""),
                                                       GetIntProperty("ro.vendor.a6l.ril.slots", 0));
    return n;
}

namespace {
ModemCore::TransportFactory& transportFactory() {
    static ModemCore::TransportFactory f = [] { return makeQrtrTransport(); };
    return f;
}
}  // namespace

void ModemCore::setTransportFactory(TransportFactory f) { transportFactory() = std::move(f); }

multisim::PowerVote& ModemCore::powerVote() {
    static multisim::PowerVote* sVote = new multisim::PowerVote();
    return *sVote;
}

ModemCore::ModemCore(int sub) : mSub(sub) {
    mCfg.dataParent = GetProperty("ro.vendor.a6l.ril.data_parent", "rmnet_ipa0");
    mCfg.rmnetFlags = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.rmnet_flags", 0x01));
    mCfg.epType = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.ep_type", 4));
    mCfg.epIface = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.ep_iface", 1));
    mCfg.dpmOpenPort = GetBoolProperty("ro.vendor.a6l.ril.dpm_open_port", true);
    mCfg.wdaAgg = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.wda_agg", 5));
    mCfg.smsStoreRoute = GetBoolProperty("persist.vendor.a6l.ril.sms_store_route", false);
    mCfg.logLevel = GetIntProperty("persist.vendor.a6l.ril.qmi_log", 2);
    ::a6l::qmi::gLogLevel = mCfg.logLevel;
}

void ModemCore::start() {
    mCtl = std::make_unique<Client>(transportFactory()(), mSub == 0 ? "ctl" : "ctl2");
    mCtl->onServiceChange([this](uint32_t svc, bool up) { onServiceChange(svc, up); });
    wireIndications();
    ::a6l::radio::DataConfig dc;
    dc.parentIface = mCfg.dataParent;
    dc.rmnetFlags = mCfg.rmnetFlags;
    dc.epType = mCfg.epType;
    dc.epIface = mCfg.epIface;
    dc.dpmOpenPort = mCfg.dpmOpenPort && mSub == 0;  // one DPM/WDA owner for the IPA port (slot 1)
    dc.wdaUlAgg = dc.wdaDlAgg = mCfg.wdaAgg;
    // slot 2: WDS clients bound to the secondary subscription, mux 5..8 / rmnet_data4..7
    dc.subscription = static_cast<uint8_t>(mSub);
    dc.muxBase = static_cast<uint8_t>(1 + mSub * dc.maxCalls);
    dc.ifIndexBase = mSub * dc.maxCalls;
    dc.setDataFormat = mSub == 0;  // WDA data format is per port, sent once by slot 1
    mData = std::make_unique<::a6l::radio::DataCallManager>(
            dc, [](const char* tag) { return std::make_unique<Client>(transportFactory()(), tag); },
            mCtl.get());
    mData->onLost([this](int cid, uint64_t gen) {
        post([this, cid, gen] { each([cid, gen](Listener* l) { l->onDataCallLost(cid, gen); }); });
    });
    mData->onReconfig([this](int cid, uint64_t gen) {  // r5 round8 F59 (dispatch thread: post only)
        post([this, cid, gen] { each([cid, gen](Listener* l) { l->onDataCallReconfigured(cid, gen); }); });
    });
    mWorker = std::thread([this] { workerLoop(); });
    mConnThread = std::thread([this] { connectLoop(); });
}

void ModemCore::onFirstReady(std::function<void()> fn) {
    std::lock_guard<std::mutex> l(mLock);
    if (mEverReady) {
        post(fn);
    } else {
        mFirstReady.push_back(std::move(fn));
    }
}

void ModemCore::addListener(Listener* l) {
    {
        std::lock_guard<std::mutex> g(mLock);
        mListeners.push_back(l);
    }
    post([this] { pumpSms(); });  // r5 deep F26: MT SMS queued while nobody listened
}

template <typename F>
void ModemCore::each(F f) {
    std::vector<Listener*> ls;
    {
        std::lock_guard<std::mutex> g(mLock);
        ls = mListeners;
    }
    for (auto* l : ls) f(l);
}

// r5 review round7 F58 (28 Sep 2026): record the termination of each call once. A call reported in END state gets the
// reason of TLV 0x14 for its id (later indications with the reason overwrite an "unknown" first report); a call that
// vanishes without an END report gets its reason if one came, else ERROR_UNSPECIFIED. mCacheLock held.
void ModemCore::recordCallEnds(const std::vector<voice::CallInfo>& calls,
                               const std::vector<std::pair<uint8_t, uint16_t>>& reasons) {
    auto reasonFor = [&](uint8_t id) -> std::optional<uint16_t> {
        for (auto& [i, r] : reasons)
            if (i == id) return r;
        return std::nullopt;
    };
    auto record = [&](uint8_t id, std::optional<uint16_t> r) {
        mLastCallFail.cause = r ? voice::lastCallFailCauseFromQmi(*r) : voice::kLcfErrorUnspecified;
        mLastCallFail.vendor = r ? "qmi-end-reason=" + std::to_string(*r) : "qmi-end-reason=none";
        LOG(INFO) << "slot" << mSub + 1 << " call " << int(id) << " ended: " << mLastCallFail.vendor << " -> cause "
                  << mLastCallFail.cause;
    };
    for (auto& c : calls) {
        if (c.state != voice::kStateEnd) {
            mEndRecorded.erase(c.id);  // live (or a new call reusing the id)
            continue;
        }
        auto r = reasonFor(c.id);
        if (r || !mEndRecorded.count(c.id)) record(c.id, r);
        mEndRecorded.insert(c.id);
    }
    for (auto& old : mCalls) {
        bool present = false;
        for (auto& c : calls)
            if (c.id == old.id) present = true;
        if (present) continue;
        auto r = reasonFor(old.id);
        if (r || !mEndRecorded.count(old.id)) record(old.id, r);
        mEndRecorded.erase(old.id);
    }
}

// VOICE service or the whole modem gone while calls existed: those calls did not end normally. mCacheLock held.
void ModemCore::recordCallsLost(const char* why) {
    bool live = false;
    for (auto& c : mCalls)
        if (c.state != voice::kStateEnd && !mEndRecorded.count(c.id)) live = true;
    if (live) {
        mLastCallFail.cause = voice::kLcfRadioInternalError;
        mLastCallFail.vendor = why;
        LOG(WARNING) << "slot" << mSub + 1 << " calls lost (" << why << "): last call fail cause RADIO_INTERNAL_ERROR";
    }
    mEndRecorded.clear();
}

ModemCore::LastCallFail ModemCore::lastCallFail() {
    std::lock_guard<std::mutex> l(mCacheLock);
    return mLastCallFail;
}

bool ModemCore::ready() const {
    std::lock_guard<std::mutex> l(mLock);
    return mReady;
}

void ModemCore::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> l(mWorkLock);
    mWork.push_back(std::move(fn));
    mWorkCv.notify_one();
}

void ModemCore::workerLoop() {
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> l(mWorkLock);
            mWorkCv.wait(l, [this] { return !mWork.empty(); });
            fn = std::move(mWork.front());
            mWork.pop_front();
        }
        fn();
    }
}

void ModemCore::connectLoop() {
    bool started = false;
    int waitedS = 0;
    while (true) {
        if (!started) {
            started = mCtl->start(kAll);
            if (!started) {
                LOG(WARNING) << "AF_QIPCRTR not available yet (qrtr module?), retrying";
                std::this_thread::sleep_for(5s);
                continue;
            }
            LOG(INFO) << "QRTR control client started, waiting for the modem QMI services";
        }
        if (mCtl->failed()) {
            // r5 review F16: the reader died on a fatal QRTR error. The client already failed its requests and
            // reported every service down (-> onServiceChange invalidated the state); stop() delivers those events
            // and closes the socket, then the loop reopens it and re-initializes like after a modem restart.
            LOG(ERROR) << "QRTR control client transport failed: reopening";
            mCtl->stop();
            {
                std::lock_guard<std::mutex> l(mLock);
                mNeedsInit = true;
            }
            started = false;
            std::this_thread::sleep_for(1s);
            continue;
        }
        bool needInit;
        {
            std::lock_guard<std::mutex> l(mLock);
            needInit = mNeedsInit;
        }
        if (!needInit) {
            maybeScheduleReconcile();  // r5 round11 F17 follow-up
            std::this_thread::sleep_for(1s);
            continue;
        }
        auto missing = mCtl->waitForServices(kCore, 10000);
        if (!missing.empty()) {
            waitedS += 10;
            if (waitedS % 60 == 0) {
                std::string m;
                for (auto s : missing) m += std::string(serviceName(s)) + " ";
                LOG(INFO) << "modem not up yet (missing " << m
                          << "); the modem starts only with persist.vendor.a6l.radio.enable=1";
            }
            continue;
        }
        uint64_t lossEpoch;
        {
            std::lock_guard<std::mutex> l(mLock);
            mSvcLost.clear();  // r5 F17: initModem registers every service
            mSetupPending.clear();  // r5 round11: initModem records what it could not register
            for (uint32_t svc : {kSvcVoice, kSvcWms}) mSvcEpoch[svc]++;
            lossEpoch = mLossEpoch;
        }
        if (!initModem()) {
            std::this_thread::sleep_for(3s);
            continue;
        }
        std::vector<std::function<void()>> first;
        {
            std::lock_guard<std::mutex> l(mLock);
            // r5 bug hunt round2 R1: DMS/UIM/NAS withdrawn while initModem() ran (modem crash right after boot). The
            // loss set mNeedsInit, but was then overwritten here: the core reported ready with its indications
            // registered on the dead instance and never re-initialized when the modem came back.
            if (mLossEpoch != lossEpoch) {
                mNeedsInit = true;
                LOG(WARNING) << "slot" << mSub + 1 << " modem lost during initialization: initializing again";
                continue;
            }
            mReady = true;
            mNeedsInit = false;
            if (!mEverReady) {
                mEverReady = true;
                first.swap(mFirstReady);
            }
        }
        LOG(INFO) << "modem ready";
        for (auto& f : first) post(f);
        post([this] { each([](Listener* l) { l->onModemReady(); }); });
    }
}

bool ModemCore::initModem() {
    Result r;
    if (mSub > 0) {
        std::string log;
        r = multisim::bindAll(*mCtl, mSub, true, &log);
        mBound = r.ok();
        LOG(INFO) << "slot" << mSub + 1 << " bind subscription " << mSub << ": " << log
                  << (mBound ? "" : " -> slot unusable (single standby?), reporting no service");
    }
    const bool subIo = mSub == 0 || mBound;  // never register slot 2 indications on SIM 1
    r = uim::registerEvents(*mCtl);
    LOG(INFO) << "UIM register events: " << r.describe();
    if (!subIo) {
        uint8_t mode = dms::kUnknownMode;
        dms::getOperatingMode(*mCtl, &mode);
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            mRadioOn = (mode == dms::kOnline);
        }
        cardStatus(true);
        return true;
    }
    r = nas::registerIndications(*mCtl);
    LOG(INFO) << "NAS register indications: " << r.describe();
    r = nas::configSignalInfo(*mCtl);
    LOG(INFO) << "NAS config signal info: " << r.describe();
    // r5 round11 F17 follow-up: a rejected VOICE/WMS registration stays pending and is retried once ready
    for (uint32_t svc : {kSvcVoice, kSvcWms}) {
        uint64_t epoch;
        {
            std::lock_guard<std::mutex> l(mLock);
            epoch = mSvcEpoch[svc];
        }
        notePendingSetup(svc, setupService(svc, kStepRegister | (svc == kSvcWms ? kStepEvents : 0u)), epoch);
    }

    dms::Ids ids;
    r = dms::getIds(*mCtl, &ids);
    if (!r.ok()) {
        LOG(WARNING) << "DMS get IDs failed: " << r.describe();
        return false;  // DMS must answer, else retry
    }
    std::string rev;
    dms::getRevision(*mCtl, &rev);
    uint8_t mode = dms::kUnknownMode;
    dms::getOperatingMode(*mCtl, &mode);
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        mIds = ids;
        mRevision = rev;
        mRadioOn = (mode == dms::kOnline);
    }
    LOG(INFO) << "modem revision '" << rev << "' operating mode " << int(mode);
    cardStatus(true);
    provisionIfNeeded();
    iccid(true);
    serving(true);
    signal(true);
    calls(true);
    return true;
}

// A USIM whose GW provisioning session the modem did not activate: slot 2 after a SIM swap (stock
// qcril activates it with UIM Change Provisioning Session), and - pinsafe 27 Sep 2026 - slot 1 too:
// the A6L modem sometimes leaves gw_primary=0xffff with the app DETECTED, and VERIFY PIN then fails
// INTERNAL. Rate limited: 5 s apart, 3 tries until the SIM power-cycles or the session appears.
// persist.vendor.a6l.ril.auto_provision=false disables it.
bool ModemCore::provisionIfNeeded(bool force) {
    if ((mSub > 0 && !mBound) || !GetBoolProperty("persist.vendor.a6l.ril.auto_provision", true)) return false;
    auto cs = cardStatus();
    if (!cs) return false;
    auto cand = multisim::provisioningCandidate(*cs, mSub);
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        if (!cand) {
            if (multisim::viewFor(*cs, mSub).provisioned) mProvisionTries = 0;  // session up: re-arm
            return false;
        }
        if (!force && (mProvisionTries >= 3 || now - mLastProvision < 5s)) return false;
        mProvisionTries++;
        mLastProvision = now;
    }
    const uint8_t session = mSub == 0 ? multisim::kSessionPrimaryGw : multisim::kSessionSecondaryGw;
    auto r = multisim::changeProvisioning(*mCtl, session, true, cand->first, cand->second);
    LOG(INFO) << "A6L_RIL_PROVISION slot" << mSub + 1 << " UIM change provisioning ("
              << (mSub == 0 ? "primary" : "secondary") << " GW, uim slot " << int(cand->first) << "): " << r.describe();
    return true;
}

// Pure PIN guard (see ModemCore.h). The PIN is sent only when the card itself asks for PIN1 (app
// state PIN) with at least one attempt left; everything else is answered without touching the card.
ModemCore::PinCheck ModemCore::checkPin(const uim::CardStatus& cs, int sub, const std::string& pin) {
    PinCheck c;
    auto v = multisim::viewFor(cs, sub);
    if (!v.present()) {
        c.verdict = PinVerdict::NoCard;
        c.why = "no card";
        return c;
    }
    if (!v.provisioned || !v.gwApp) {
        c.verdict = PinVerdict::NotProvisioned;
        c.why = "no provisioned GW app (session not active)";
        return c;
    }
    const uim::App& a = *v.gwApp;
    const bool upin = a.upinReplacesPin1;
    const uint8_t st = upin ? v.cardPtr->upinState : a.pin1State;
    c.retries = upin ? v.cardPtr->upinRetries : a.pin1Retries;
    c.pinId = upin ? uim::kUpin : uim::kPin1;  // r5 round5 F44
    if (a.state == uim::kAppStateReady) {
        c.verdict = PinVerdict::AlreadyReady;
        c.why = "SIM app ready (PIN disabled or already verified)";
        return c;
    }
    if (a.state == uim::kAppStatePuk || a.state == uim::kAppStatePinBlocked || st == uim::kPinBlocked ||
        st == uim::kPinPermBlocked || c.retries < 1) {
        c.verdict = PinVerdict::Blocked;
        c.retries = 0;
        c.why = "PIN blocked (PUK required) or no attempt left";
        return c;
    }
    if (a.state != uim::kAppStatePin) {
        c.verdict = PinVerdict::NotPinState;
        c.why = "SIM app state " + std::to_string(a.state) + " is not PIN required";
        return c;
    }
    if (pin.size() < 4 || pin.size() > 8 || pin.find_first_not_of("0123456789") != std::string::npos) {
        c.verdict = PinVerdict::BadFormat;
        c.why = "PIN must be 4-8 digits";
        return c;
    }
    c.verdict = PinVerdict::Send;
    c.why = "PIN required, " + std::to_string(c.retries) + " attempt(s) left";
    return c;
}

std::optional<uint8_t> ModemCore::pinIdFor(const uim::CardStatus& cs, int sub, const std::string& aidHex) {
    auto v = multisim::viewFor(cs, sub);
    if (!v.present()) return std::nullopt;
    const uim::App* a = nullptr;
    if (aidHex.empty()) {
        a = v.gwApp;
    } else {
        auto aid = unhex(aidHex);
        if (!aid) return std::nullopt;
        for (const auto& app : v.cardPtr->apps)
            if (app.aid == *aid) a = &app;
    }
    if (!a) return std::nullopt;
    return a->upinReplacesPin1 ? uim::kUpin : uim::kPin1;
}

std::optional<uint8_t> ModemCore::pinTarget(const std::string& aidHex) {
    auto cs = cardStatus(true);
    if (!cs) return std::nullopt;
    return pinIdFor(*cs, mSub, aidHex);
}

ModemCore::PinCheck ModemCore::pinPreflight(const std::string& pin, int waitMs) {
    auto cs = cardStatus(true);
    if (!cs) return PinCheck{PinVerdict::NoCard, -1, "card status unavailable"};
    if (multisim::provisioningCandidate(*cs, mSub) && provisionIfNeeded(true)) {
        for (int t = 0; t < waitMs; t += 250) {  // wait for the app to leave DETECTED
            std::this_thread::sleep_for(250ms);
            cs = cardStatus(true);
            if (!cs) break;
            auto v = multisim::viewFor(*cs, mSub);
            if (v.provisioned && v.gwApp && v.gwApp->state != uim::kAppStateDetected &&
                v.gwApp->state != uim::kAppStateUnknown)
                break;
        }
        if (!cs) return PinCheck{PinVerdict::NoCard, -1, "card status unavailable"};
    }
    PinCheck c = checkPin(*cs, mSub, pin);
    if (c.verdict == PinVerdict::Send) {
        const std::string id = iccid();
        std::lock_guard<std::mutex> l(mCacheLock);
        if (mWrongPinHash && mWrongPinHash == std::hash<std::string>{}(pin) && mWrongPinIccid == id) {
            c.verdict = PinVerdict::RepeatedWrong;
            c.why = "the card already rejected this PIN: not sent again";
        }
    }
    LOG(INFO) << "A6L_RIL_PIN slot" << mSub + 1 << " preflight verdict=" << static_cast<int>(c.verdict)
              << " retries=" << c.retries << " (" << c.why << ")";
    return c;
}

void ModemCore::notePinResult(const std::string& pin, bool accepted) {
    const std::string id = iccid();
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        if (accepted) {
            mWrongPinHash = 0;
            mWrongPinIccid.clear();
        } else {
            mWrongPinHash = std::hash<std::string>{}(pin);
            mWrongPinIccid = id;
        }
    }
    post([this] {
        cardStatus(true);
        each([](Listener* l) { l->onSimChanged(); });
    });
}

void ModemCore::postDelayed(int ms, std::function<void()> fn) {
    // cores are process-lifetime singletons: a detached timer thread is safe
    std::thread([this, ms, fn = std::move(fn)]() mutable {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        post(std::move(fn));
    }).detach();
}

ModemCore::SlotSim ModemCore::slotSim(bool refresh) {
    SlotSim out;
    auto cs = cardStatus(refresh);
    if (!cs) return out;
    auto v = multisim::viewFor(*cs, mSub);
    out.valid = true;
    out.card = v.card;
    if (v.cardPtr) out.cardInfo = *v.cardPtr;
    out.gwAppIndex = v.gwAppIndex;
    out.provisioned = v.provisioned;
    out.provSession = v.provSession;
    out.cardSession = v.cardSession;
    out.nonProvSession = v.nonProvSession;
    if (v.gwApp) out.gwAid = v.gwApp->aid;
    return out;
}

std::string ModemCore::callRat() const {
    std::lock_guard<std::mutex> l(const_cast<std::mutex&>(mCacheLock));
    return mCallRat;
}

// Voice RAT bookkeeping (Astra review H25: record the actual voice RAT; CSFB LTE -> UMTS seen on
// 25 Sep). Logged on call start, on every RAT change during a call and at the end.
void ModemCore::logCallRat(const std::vector<voice::CallInfo>& calls) {
    bool active = false;
    for (auto& c : calls)
        if (c.state != voice::kStateEnd) active = true;
    std::string rat, before;
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        rat = mServing ? nas::radioIfList(*mServing) : "unknown";
        // volte3: per-call domain (IMS = VoLTE/VoWiFi, CS) and SRVCC (IMS -> CS during the call)
        for (auto& c : calls) {
            std::string d = voice::callDomain(c);
            if (d == "unknown") continue;
            auto& was = mCallDomain[c.id];
            if (was == d) continue;
            if (was.empty())
                LOG(INFO) << "A6L_RIL_CALL_DOMAIN slot" << mSub + 1 << " id=" << int(c.id) << " " << d
                          << " type=" << voice::callTypeName(c.type) << " mode=" << voice::callModeName(c.mode);
            else
                LOG(INFO) << "A6L_RIL_CALL_DOMAIN slot" << mSub + 1 << " id=" << int(c.id) << " " << was << " -> " << d
                          << (was.rfind("ims", 0) == 0 && d == "cs" ? " (SRVCC)" : "");
            was = d;
        }
        if (!active) mCallDomain.clear();
        if (active == mInCall && (!active || rat == mCallRat)) return;
        before = mCallRat;
        bool starting = active && !mInCall;
        bool ending = !active && mInCall;
        mInCall = active;
        if (starting) {
            LOG(INFO) << "A6L_RIL_CALL_RAT slot" << mSub + 1 << " start rat=" << rat;
        } else if (ending) {
            LOG(INFO) << "A6L_RIL_CALL_RAT slot" << mSub + 1 << " end rat=" << rat << " (during call: " << before << ")";
        } else {
            LOG(INFO) << "A6L_RIL_CALL_RAT slot" << mSub + 1 << " change " << before << " -> " << rat
                      << (before == "lte" && (rat == "umts" || rat == "gsm") ? " (CSFB)" : "");
        }
        mCallRat = active ? rat : std::string();
    }
    if (active) post([this] { serving(true); });  // refresh: the serving indication may lag the CSFB
}

void ModemCore::onServiceChange(uint32_t svc, bool up) {
    if (svc == kSvcImsa) {  // volte2: the modem publishes IMSA late (IMS task), or never
        if (mSub == 0) post([this, up] { imsaChanged(up); });
        return;
    }
    if (svc == kSvcVoice || svc == kSvcWms) {  // r5 review F17
        if (up) {
            post([this, svc] { restoreService(svc); });
        } else {
            serviceLost(svc);
        }
        return;
    }
    if (up) return;  // the connect loop notices new servers
    if (svc != kSvcUim && svc != kSvcNas && svc != kSvcDms) return;
    bool was;
    {
        std::lock_guard<std::mutex> l(mLock);
        was = mReady;
        mReady = false;
        mNeedsInit = true;
        mLossEpoch++;  // r5 bug hunt round2 R1: an initModem() in progress must not mark this instance ready
        for (uint32_t s : {kSvcVoice, kSvcWms}) mSvcEpoch[s]++;  // r5 round11: in-flight setup attempts are void
    }
    if (!was) return;
    LOG(WARNING) << "modem lost (" << serviceName(svc) << " gone): modem crash/SSR or stop";
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        mCard.reset();
        mServing.reset();
        recordCallsLost("modem-lost");
        mCalls.clear();
        mRadioOn.reset();
        mOpName.reset();
        // r5 F29: queries still in flight must not restore pre-restart state
        mCardGen++;
        mServingGen++;
        mOpNameGen++;
        mCallsGen++;
        mIccidGen++;
    }
    publishVoiceActive(mSub, {});
    powerVote().reset();
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        if (mInCall) LOG(INFO) << "A6L_RIL_CALL_RAT slot" << mSub + 1 << " end (modem lost) rat=" << mCallRat;
        mInCall = false;
        mCallRat.clear();
    }
    invalidateSmsTransactions();
    post([this] {
        mData->modemReset();
        each([](Listener* l) { l->onModemLost(); });
    });
}

// r5 review F17 (28 Sep 2026): VOICE/WMS withdrawn alone. Before, only UIM/NAS/DMS loss was handled: a VOICE restart
// kept the cached calls and vendor.a6l.voice.active=1, and neither service was re-registered when it came back.
// Dispatcher thread: no QMI requests here.
void ModemCore::serviceLost(uint32_t svc) {
    {
        std::lock_guard<std::mutex> l(mLock);
        mSvcLost.insert(svc);
        mSetupPending.erase(svc);  // r5 round11: the restoration path owns it again
        mSvcEpoch[svc]++;
    }
    LOG(WARNING) << "slot" << mSub + 1 << " " << serviceName(svc) << " service gone (other services still up)";
    if (svc == kSvcVoice) {
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            recordCallsLost("voice-service-lost");
            mCalls.clear();
            mCallsGen++;
            mCallDomain.clear();
            if (mInCall) LOG(INFO) << "A6L_RIL_CALL_RAT slot" << mSub + 1 << " end (VOICE lost) rat=" << mCallRat;
            mInCall = false;
            mCallRat.clear();
        }
        publishVoiceActive(mSub, {});
        post([this] { each([](Listener* l) { l->onCallsChanged(false); }); });
    } else {
        invalidateSmsTransactions();  // the transactions belonged to the old WMS instance
    }
}

// Worker thread. Only after an isolated loss while the core is ready; a full modem restart goes through initModem.
// r5 round11 F17 follow-up: the service leaves mSvcLost only into mSetupPending (bind + registrations), which is cleared
// only once the modem acknowledged every required step (reconcileServices).
void ModemCore::restoreService(uint32_t svc) {
    {
        std::lock_guard<std::mutex> l(mLock);
        if (!mReady || mNeedsInit || !mSvcLost.count(svc)) return;
        mSvcLost.erase(svc);
        PendingSetup p;
        p.steps = (mSub > 0 ? kStepBind : 0u) | kStepRegister | (svc == kSvcWms ? kStepEvents : 0u);
        mSetupPending[svc] = p;  // due now
    }
    reconcileServices();
    if (svc == kSvcVoice && mCtl->hasService(svc)) {
        calls(true);
        each([](Listener* l) { l->onCallsChanged(false); });
    }
}

namespace {
// The modem does not implement the request at all: an optional step, retrying cannot help.
bool unsupportedStep(const Result& r) {
    return r.status == Result::QmiFailure && (r.qmiError == kErrInvalidQmiCommand || r.qmiError == kErrNotSupported ||
                                              r.qmiError == kErrDeviceUnsupported);
}
}  // namespace

// r5 round11 F17 follow-up. QMI thread (connect loop in initModem, worker otherwise). Returns the steps that failed
// transiently (to retry); unsupported steps are dropped with a warning. A failed slot-2 bind keeps every step pending:
// registering before the bind would subscribe to SIM 1's events.
unsigned ModemCore::setupService(uint32_t svc, unsigned steps) {
    if (!mCtl->hasService(svc)) return steps;  // not published (yet): registered when it appears
    if (mSub > 0 && !mBound) return 0;         // slot unusable (single standby): nothing is registered on it
    const char* name = svc == kSvcVoice ? "VOICE" : "WMS";
    auto outcome = [&](unsigned step, const Result& r, const char* what) -> unsigned {
        LOG(INFO) << "slot" << mSub + 1 << " " << name << " " << what << ": " << r.describe();
        if (r.ok()) return 0;
        if (unsupportedStep(r)) {
            LOG(WARNING) << "slot" << mSub + 1 << " " << name << " " << what << " unsupported by the modem: not retried";
            return 0;
        }
        return step;
    };
    if ((steps & kStepBind) && mSub > 0) {
        auto b = svc == kSvcVoice ? multisim::voiceBind(*mCtl, static_cast<uint8_t>(mSub))
                                  : multisim::wmsBind(*mCtl, static_cast<uint8_t>(mSub));
        LOG(INFO) << "slot" << mSub + 1 << " " << name << " bind subscription: " << b.describe();
        if (!b.ok()) return steps;
    }
    unsigned left = 0;
    if (steps & kStepRegister) {
        if (svc == kSvcVoice) {
            left |= outcome(kStepRegister, voice::indicationRegister(*mCtl), "indication register");
        } else {
            auto r = wms::setRoutes(*mCtl, mCfg.smsStoreRoute);
            LOG(INFO) << "WMS routes (" << (mCfg.smsStoreRoute ? "store" : "transfer-only") << "): " << r.describe();
            if (!r.ok() && !mCfg.smsStoreRoute) r = wms::setRoutes(*mCtl, true);
            left |= outcome(kStepRegister, r, mCfg.smsStoreRoute ? "routes" : "routes (store-and-notify fallback)");
        }
    }
    if (steps & kStepEvents) left |= outcome(kStepEvents, wms::setEventReport(*mCtl, true), "event report");
    return left;
}

void ModemCore::notePendingSetup(uint32_t svc, unsigned remaining, uint64_t epoch) {
    std::lock_guard<std::mutex> l(mLock);
    if (mSvcEpoch[svc] != epoch) return;  // lost / re-initialized meanwhile: that path owns the service now
    if (!remaining) {
        if (mSetupPending.erase(svc)) LOG(INFO) << "slot" << mSub + 1 << " " << serviceName(svc) << " setup complete";
        return;
    }
    auto& p = mSetupPending[svc];
    p.steps = remaining;
    p.attempts++;
    const auto delay = std::chrono::milliseconds(std::min<int64_t>(1000LL << std::min(p.attempts - 1, 6), 60000));
    p.due = std::chrono::steady_clock::now() + delay;
    LOG(WARNING) << "slot" << mSub + 1 << " " << serviceName(svc) << " setup incomplete (steps 0x" << std::hex
                 << remaining << std::dec << "), retry " << p.attempts << " in " << delay.count() << " ms";
}

void ModemCore::reconcileServices() {
    struct Job {
        uint32_t svc;
        unsigned steps;
        uint64_t epoch;
        int attempts;
    };
    std::vector<Job> jobs;
    {
        std::lock_guard<std::mutex> l(mLock);
        mReconcileQueued = false;
        if (!mReady || mNeedsInit) return;
        const auto now = std::chrono::steady_clock::now();
        for (auto& [svc, p] : mSetupPending)
            if (p.due <= now && !mSvcLost.count(svc)) jobs.push_back({svc, p.steps, mSvcEpoch[svc], p.attempts});
    }
    for (auto& j : jobs) {
        if (!mCtl->hasService(j.svc)) continue;  // withdrawn again: serviceLost took it over
        unsigned left = setupService(j.svc, j.steps);
        notePendingSetup(j.svc, left, j.epoch);
        if (j.svc == kSvcVoice && j.attempts > 0 && (j.steps & kStepRegister) && !(left & kStepRegister)) {
            calls(true);  // call notifications registered late: resynchronize the call list
            each([](Listener* l) { l->onCallsChanged(false); });
        }
    }
}

// Connect loop (1 s tick while ready): schedule due retries, and restore a service that returned while the core was
// not ready (restoreService ignored it then and nothing else would register it).
void ModemCore::maybeScheduleReconcile() {
    std::vector<uint32_t> lost;
    bool due = false;
    {
        std::lock_guard<std::mutex> l(mLock);
        if (!mReady || mNeedsInit) return;
        lost.assign(mSvcLost.begin(), mSvcLost.end());
        const auto now = std::chrono::steady_clock::now();
        if (!mReconcileQueued)
            for (auto& [svc, p] : mSetupPending)
                if (p.due <= now) due = true;
        if (due) mReconcileQueued = true;
    }
    for (auto svc : lost)
        if (mCtl->hasService(svc)) post([this, svc] { restoreService(svc); });
    if (due) post([this] { reconcileServices(); });
}

std::set<uint32_t> ModemCore::pendingServiceSetup() const {
    std::lock_guard<std::mutex> l(mLock);
    std::set<uint32_t> out;
    for (auto& [svc, p] : mSetupPending) out.insert(svc);
    return out;
}

// volte2: IMSA attach/detach. Worker thread (QMI requests allowed).
void ModemCore::imsaChanged(bool up) {
    ImsState st;
    st.present = up && mCtl->hasService(kSvcImsa);
    if (st.present) {
        // volte5 (28 Sep): bind first, as stock qcril does (unbound IMSA answers INVALID_OPERATION)
        auto b = imsa::bindSubscription(*mCtl, static_cast<uint32_t>(mSub));
        LOG(INFO) << "IMSA bind subscription " << mSub << ": " << b.describe();
        auto r = imsa::registerIndications(*mCtl);
        LOG(INFO) << "IMSA register indications: " << r.describe();
        r = imsa::getRegStatus(*mCtl, &st.reg);
        if (!r.ok()) LOG(WARNING) << "IMSA get registration status: " << r.describe();
        r = imsa::getServicesStatus(*mCtl, &st.services);
        if (!r.ok()) LOG(WARNING) << "IMSA get services status: " << r.describe();
    }
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        mIms = st;
    }
    LOG(INFO) << "A6L_RIL_IMS slot" << mSub + 1 << " imsa=" << (st.present ? "present" : "absent") << " "
              << st.reg.summary() << " " << st.services.summary();
    each([](Listener* l) { l->onImsChanged(); });
}

ModemCore::ImsState ModemCore::imsState() const {
    std::lock_guard<std::mutex> l(const_cast<std::mutex&>(mCacheLock));
    return mIms;
}

void ModemCore::wireIndications() {
    mCtl->onIndication(kSvcImsa, imsa::kRegStatusInd, [this](const Message& m) {  // volte2
        auto rs = imsa::parseRegStatus(m);
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            mIms.present = true;
            mIms.reg = rs;
        }
        LOG(INFO) << "A6L_RIL_IMS slot" << mSub + 1 << " reg " << rs.summary();
        post([this] { each([](Listener* l) { l->onImsChanged(); }); });
    });
    mCtl->onIndication(kSvcImsa, imsa::kServicesStatusInd, [this](const Message& m) {  // volte2
        auto ss = imsa::parseServicesStatus(m);
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            mIms.present = true;
            mIms.services = ss;
        }
        LOG(INFO) << "A6L_RIL_IMS slot" << mSub + 1 << " services " << ss.summary();
        post([this] { each([](Listener* l) { l->onImsChanged(); }); });
    });
    mCtl->onIndication(kSvcUim, uim::kCardStatusInd, [this](const Message& m) {
        auto* t = m.get(0x10);
        if (auto cs = t ? uim::parseCardStatus(*t) : std::nullopt) {
            std::lock_guard<std::mutex> l(mCacheLock);
            mCard = cs;
            mCardGen++;
        }
        post([this] {
            iccid(true);
            provisionIfNeeded();  // pinsafe: slot 1 too (gw_primary=0xffff seen on 27 Sep)
            each([](Listener* l) { l->onSimChanged(); });
        });
    });
    mCtl->onIndication(kSvcNas, nas::kGetServingSystem, [this](const Message& m) {
        if (auto s = nas::parseServingSystem(m, true)) {
            std::vector<voice::CallInfo> calls;
            {
                std::lock_guard<std::mutex> l(mCacheLock);
                mServing = s;
                mServingGen++;
                mOpName.reset();
                mOpNameGen++;
                calls = mCalls;
            }
            logCallRat(calls);
        }
        post([this] { each([](Listener* l) { l->onNetworkChanged(); }); });
    });
    mCtl->onIndication(kSvcNas, nas::kSystemInfoInd, [this](const Message&) {
        post([this] {
            serving(true);
            each([](Listener* l) { l->onNetworkChanged(); });
        });
    });
    mCtl->onIndication(kSvcNas, nas::kOperatorNameInd, [this](const Message& m) {
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            mOpName = nas::parseOperatorName(m);
            mOpNameGen++;
        }
        post([this] { each([](Listener* l) { l->onNetworkChanged(); }); });
    });
    mCtl->onIndication(kSvcNas, nas::kSignalInfoInd, [this](const Message& m) {
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            mSignal = nas::parseSignalInfo(m);
        }
        post([this] { each([](Listener* l) { l->onSignalChanged(); }); });
    });
    mCtl->onIndication(kSvcNas, nas::kNetworkTimeInd, [this](const Message& m) {
        Reader r(m.get(0x01));
        unsigned y = r.u16(), mo = r.u8(), d = r.u8(), h = r.u8(), mi = r.u8(), s = r.u8();
        if (!r.good()) return;
        // r5 round11 F65: absent time zone / DST stay unknown (TimePolicy.h formatNitz), never "+0" / ",0"
        std::optional<int> tz, dst;
        if (auto* t = m.get(0x10); t && !t->empty()) tz = static_cast<int8_t>((*t)[0]);
        if (auto* t = m.get(0x11); t && !t->empty()) dst = (*t)[0];
        auto formatted = formatNitz(y, mo, d, h, mi, s, tz, dst);
        if (!formatted) {
            LOG(INFO) << "NAS network time " << y << "-" << mo << "-" << d << " without a valid time zone ("
                      << (tz ? std::to_string(*tz) : std::string("absent")) << "): no NITZ sent";
            return;
        }
        std::string nitz = *formatted;
        // r5 round8 F61: receipt stamped on the suspend-inclusive boot clock (= android::elapsedRealtime); the HAL
        // reports the time spent in the worker queue as ageMs (TimePolicy.h)
        int64_t now = bootTimeMs();
        post([this, nitz, now] { each([&](Listener* l) { l->onNitz(nitz, now); }); });
    });
    // telephony-flows (29 Sep 2026): USSD. Text is never logged (balances, codes); only kind/action/length/error.
    for (uint16_t id : {voice::ussd::kUssdInd, voice::ussd::kReleaseInd, voice::ussd::kOriginateNoWait}) {
        mCtl->onIndication(kSvcVoice, id, [this](const Message& m) {
            auto e = voice::ussd::parseIndication(m);
            if (!e) {
                LOG(WARNING) << "slot" << mSub + 1 << " USSD indication 0x" << std::hex << m.msgId << std::dec
                             << " malformed, ignored";
                return;
            }
            LOG(INFO) << "slot" << mSub + 1 << " USSD ind kind=" << e->kind << " action=" << int(e->userAction)
                      << " text=" << (e->text.present ? std::to_string(e->text.utf8.size()) + "B" : "none")
                      << (e->text.decodeError ? " (undecodable)" : "")
                      << (e->error ? " error=" + std::to_string(*e->error) : "")
                      << (e->failureCause ? " cause=" + std::to_string(*e->failureCause) : "");
            auto ev = *e;
            post([this, ev] { each([&](Listener* l) { l->onUssd(ev); }); });
        });
    }
    mCtl->onIndication(kSvcVoice, voice::kAllCallStatusInd, [this](const Message& m) {
        auto calls = voice::parseCalls(m, true);
        auto ends = voice::parseCallEndReasons(m);  // r5 round7 F58
        bool ringing = false;
        for (auto& c : calls)
            if (c.state == voice::kStateIncoming) ringing = true;
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            recordCallEnds(calls, ends);
            mCalls = calls;
            mCallsGen++;
        }
        publishVoiceActive(mSub, calls);
        logCallRat(calls);
        post([this, ringing] { each([ringing](Listener* l) { l->onCallsChanged(ringing); }); });
    });
    mCtl->onIndication(kSvcWms, wms::kSetEventReport, [this](const Message& m) {
        auto e = wms::parseEventReport(m);
        uint64_t gen;
        {
            std::lock_guard<std::mutex> l(mSmsLock);
            gen = mSmsGen;
        }
        if (e.etwsType) {  // r5 deep F25: ETWS primary/secondary notification
            auto data = e.etws;
            LOG(INFO) << "ETWS notification type " << int(*e.etwsType) << " len=" << data.size();
            post([this, data] { each([&](Listener* l) { l->onNewBroadcastSms(data); }); });
        }
        if (e.hasTransfer) {
            wms::AckOptions ao;
            ao.protocol = wms::messageProtocolFor(e.format);
            ao.smsOnIms = e.smsOnIms;
            LOG(INFO) << "MT SMS txn=" << e.txn << " fmt=" << int(e.format) << " ack_ind=" << int(e.ackIndicator)
                      << " ims=" << (e.smsOnIms ? (*e.smsOnIms ? 1 : 0) : -1) << " len=" << e.data.size();
            if (e.isBroadcast()) {
                // r5 deep F25: cell broadcast page -> newBroadcastSms (UNSOLICITED, no framework ack). Never through
                // the point-to-point SMS queue; if the modem asked for an ack it is positive (the page was delivered).
                auto data = e.data;
                bool ack = e.needsAck();
                uint32_t txn = e.txn;
                post([this, data, ack, txn, ao] {
                    each([&](Listener* l) { l->onNewBroadcastSms(data); });
                    if (ack) wms::sendAck(*mCtl, txn, true, 0, 0, ao);
                });
                return;
            }
            if (e.format != wms::kFormatGwPp) {
                LOG(WARNING) << "ignoring MT SMS in format " << int(e.format);
                if (e.needsAck()) {
                    uint32_t txn = e.txn;
                    post([this, txn, ao] { wms::sendAck(*mCtl, txn, false, 0x6F /*unspecified*/, 0xFF, ao); });
                }
                return;
            }
            // the modem sends the bare TPDU (no SMSC prefix, real capture 24 Sep); Android wants SMSC+TPDU
            SmsDelivery d;
            d.pdu = ::a6l::sms::toSmscPrefixed(e.data);
            d.statusReport = isStatusReport(d.pdu);
            d.txn = e.txn;
            d.needed = e.needsAck();
            d.opts = ao;
            d.gen = gen;
            // r5 deep F27: queued on the worker, in the same order as stored-route messages (one ordered owner)
            post([this, d] { enqueueSms(d); });
        } else if (e.stored) {
            auto st = *e.stored;
            post([this, st, gen] {
                uint8_t fmt = 0;
                std::vector<uint8_t> d;
                auto r = wms::rawRead(*mCtl, st.first, st.second, &fmt, &d);
                if (!r.ok()) {
                    LOG(ERROR) << "WMS raw read " << int(st.first) << "/" << st.second << ": " << r.describe();
                    return;
                }
                if (fmt == wms::kFormatGwBc) {  // stored broadcast page: deliver, then free the modem slot
                    each([&](Listener* l) { l->onNewBroadcastSms(d); });
                    wms::deleteMessage(*mCtl, st.first, st.second);
                    return;
                }
                if (fmt != wms::kFormatGwPp) {
                    LOG(WARNING) << "stored MT message " << int(st.first) << "/" << st.second << " format " << int(fmt)
                                 << " left in modem storage";
                    return;
                }
                // r5 deep F26: the modem copy is kept until Android acknowledges receipt positively (ackLastSms)
                SmsDelivery m;
                m.pdu = ::a6l::sms::toSmscPrefixed(d);
                m.statusReport = isStatusReport(m.pdu);
                m.stored = st;
                m.gen = gen;
                enqueueSms(m);
            });
        }
    });
}

// r5 deep F27: transactions/indexes of a lost modem (or WMS) instance are void. Undelivered ones are dropped (the
// SMSC retransmits unacknowledged transfer messages; stored copies stay in modem storage); a delivered one stays at the
// head so Android's pending acknowledgement consumes it (stale generation: nothing is sent to the new instance).
void ModemCore::invalidateSmsTransactions() {
    std::lock_guard<std::mutex> l(mSmsLock);
    mSmsGen++;
    std::deque<SmsDelivery> keep;
    for (auto& d : mSmsQueue)
        if (d.delivered) keep.push_back(d);
    mSmsQueue.swap(keep);
}

void ModemCore::enqueueSms(SmsDelivery d) {
    {
        std::lock_guard<std::mutex> l(mSmsLock);
        if (d.gen != mSmsGen) {
            LOG(WARNING) << "MT SMS from a lost modem instance dropped (not acknowledged: the SMSC retransmits)";
            return;
        }
        if (d.stored)
            for (auto& q : mSmsQueue)
                if (q.stored == d.stored && q.gen == d.gen) return;  // duplicate stored-message indication
        mSmsQueue.push_back(std::move(d));
    }
    pumpSms();
}

void ModemCore::pumpSms() {
    {
        std::lock_guard<std::mutex> g(mLock);
        if (mListeners.empty()) return;  // kept queued (not acked, not deleted) until Android is there
    }
    SmsDelivery d;
    {
        std::lock_guard<std::mutex> l(mSmsLock);
        if (mSmsQueue.empty() || mSmsQueue.front().delivered) return;  // one outstanding newSms at a time
        mSmsQueue.front().delivered = true;
        d = mSmsQueue.front();
    }
    each([&](Listener* l) { l->onNewSms(d.pdu, d.statusReport); });
}

void ModemCore::smsClientReconnected() {
    {
        std::lock_guard<std::mutex> l(mSmsLock);
        if (!mSmsQueue.empty() && mSmsQueue.front().delivered && mSmsQueue.front().gen == mSmsGen)
            mSmsQueue.front().delivered = false;
    }
    post([this] { pumpSms(); });
}

size_t ModemCore::pendingSmsCount() {
    std::lock_guard<std::mutex> l(mSmsLock);
    return mSmsQueue.size();
}

bool ModemCore::ackLastSms(bool success, uint8_t rpCause, uint8_t tpCause) {
    SmsDelivery a;
    bool stale;
    {
        std::lock_guard<std::mutex> l(mSmsLock);
        if (mSmsQueue.empty() || !mSmsQueue.front().delivered) return false;
        a = std::move(mSmsQueue.front());
        mSmsQueue.pop_front();
        stale = a.gen != mSmsGen;
    }
    post([this] { pumpSms(); });  // next message only after this acknowledgement
    if (stale) {
        LOG(INFO) << "SMS ack for a message of a lost modem instance: nothing sent";
        return true;
    }
    if (a.stored) {
        if (!success) {
            LOG(WARNING) << "stored MT SMS " << int(a.stored->first) << "/" << a.stored->second
                         << " rejected by Android (rp=" << int(rpCause) << "): kept in modem storage";
            return true;
        }
        auto r = wms::deleteMessage(*mCtl, a.stored->first, a.stored->second);  // Android has its own copy now
        if (!r.ok()) LOG(WARNING) << "WMS delete " << int(a.stored->first) << "/" << a.stored->second << ": " << r.describe();
        return true;
    }
    if (!a.needed) return true;  // ack indicator DO_NOT_SEND: the modem acks
    int cause = -1;
    auto r = wms::sendAck(*mCtl, a.txn, success, rpCause, tpCause, a.opts, &cause);
    if (r.ok()) return true;
    LOG(WARNING) << "WMS send ack txn=" << a.txn << ": " << r.describe() << " cause=" << wms::ackFailureCauseName(cause);
    // ACK_NOT_SENT: the modem could not deliver the RP-ACK (link released / no network response).
    // The SMSC retransmits; Android's copy is already stored, so the duplicate filter handles it.
    return r.status == Result::QmiFailure && r.qmiError == kErrAckNotSent;
}

// r5 review F29 (28 Sep 2026): each refresh records the cache generation before its request and stores the answer only
// if no indication/reset changed the cache meanwhile (the newer observation wins). Presence checks hold the lock.
std::optional<uim::CardStatus> ModemCore::cardStatus(bool refresh) {
    uint64_t gen;
    bool need;
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        need = refresh || !mCard;
        gen = mCardGen;
    }
    if (need) {
        uim::CardStatus cs;
        auto r = uim::getCardStatus(*mCtl, &cs);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (!r.ok()) {
            LOG(WARNING) << "UIM get card status: " << r.describe();
        } else if (gen == mCardGen) {
            mCard = cs;
            mCardGen++;
        }
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mCard;
}

// r5 round5 F43: every subscription (the primary too) reads EF_ICCID through the card session of the physical card
// it is provisioned from (multisim::viewFor), not card 1. The DMS fallback is only used where its identity is the
// same card (slot 1 on physical card 1 / a DMS bound to this subscription). A failed read after the mapping moved to
// another card clears the old identity instead of keeping it; unknown card status keeps the cache as it is.
std::string ModemCore::iccid(bool refresh) {
    uint64_t gen;
    bool need;
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        need = refresh || mIccid.empty();
        gen = mIccidGen;
    }
    if (need) {
        auto sim = slotSim();
        if (sim.valid) {
            if (!sim.cardInfo || sim.cardInfo->state != uim::kCardPresent) {
                std::lock_guard<std::mutex> l(mCacheLock);
                mIccid.clear();
                mIccidCard = -1;
                mIccidGen++;
                return mIccid;
            }
            std::string id;
            const bool dmsFallback = mSub == 0 ? sim.card == 0 : mBound;
            Result r = uim::readIccid(*mCtl, &id, sim.cardSession, dmsFallback);
            std::lock_guard<std::mutex> l(mCacheLock);
            if (gen == mIccidGen) {
                if (r.ok()) {
                    mIccid = id;
                    mIccidCard = sim.card;
                    mIccidGen++;
                } else if (mIccidCard != sim.card) {
                    mIccid.clear();
                    mIccidCard = -1;
                    mIccidGen++;
                }
            }
        }
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mIccid;
}

std::optional<nas::ServingSystem> ModemCore::serving(bool refresh) {
    if (mSub > 0 && !mBound) return std::nullopt;
    uint64_t gen;
    bool need;
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        need = refresh || !mServing;
        gen = mServingGen;
    }
    if (need) {
        nas::ServingSystem s;
        auto r = nas::getServingSystem(*mCtl, &s);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok() && gen == mServingGen) {
            mServing = s;
            mServingGen++;
        } else if (r.ok()) {
            LOG(INFO) << "serving system query answer dropped: a newer indication arrived meanwhile";
        }
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mServing;
}

nas::SignalInfo ModemCore::signal(bool refresh) {
    if (mSub > 0 && !mBound) return {};
    if (refresh) {
        nas::SignalInfo s;
        auto r = nas::getSignalInfo(*mCtl, &s);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok()) mSignal = s;
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mSignal;
}

nas::OperatorName ModemCore::operatorName(bool refresh) {
    uint64_t gen;
    bool need;
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        need = refresh || !mOpName;
        gen = mOpNameGen;
    }
    if (need) {
        nas::OperatorName o;
        auto r = nas::getOperatorName(*mCtl, &o);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (gen == mOpNameGen) {
            mOpName = r.ok() ? o : nas::OperatorName{};
            mOpNameGen++;
        }
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mOpName.value_or(nas::OperatorName{});  // a serving indication may have reset it meanwhile
}

std::vector<voice::CallInfo> ModemCore::calls(bool refresh) {
    if (mSub > 0 && !mBound) return {};
    if (refresh) {
        uint64_t gen;
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            gen = mCallsGen;
        }
        std::vector<voice::CallInfo> c;
        auto r = voice::getAllCalls(*mCtl, &c);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok() && gen == mCallsGen) {
            recordCallEnds(c, {});  // r5 round7 F58: a call gone from the list without an END report
            mCalls = c;
            mCallsGen++;
            publishVoiceActive(mSub, c);
        }
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mCalls;
}

dms::Ids ModemCore::ids() {
    std::lock_guard<std::mutex> l(mCacheLock);
    return mIds.value_or(dms::Ids{});
}

std::string ModemCore::revision() {
    std::lock_guard<std::mutex> l(mCacheLock);
    return mRevision;
}

// Per-slot radio state = modem ONLINE and this slot's own vote (see multisim::PowerVote).
bool ModemCore::radioOn() {
    const bool mine = powerVote().wants(mSub);
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        if (mRadioOn) return *mRadioOn && mine;
    }
    uint8_t mode = dms::kUnknownMode;
    if (dms::getOperatingMode(*mCtl, &mode).ok()) {
        std::lock_guard<std::mutex> l(mCacheLock);
        mRadioOn = (mode == dms::kOnline);
        return *mRadioOn && mine;
    }
    return false;
}

void ModemCore::publishModemMode(bool online) {
    for (int s = 0; s < slotCount(); s++) {
        auto& c = get(s + 1);
        bool changed;
        {
            std::lock_guard<std::mutex> l(c.mCacheLock);
            changed = !c.mRadioOn || *c.mRadioOn != online;
            c.mRadioOn = online;
        }
        if (!changed) continue;
        bool v = online && powerVote().wants(s);
        c.post([&c, v] { c.each([v](Listener* l) { l->onRadioPowerChanged(v); }); });
    }
}

// r5 review F18 (28 Sep 2026): the slot's vote is committed only once the modem applied the mode. Before, the vote
// changed first and a rejected power-off still made radioOn() report OFF with the modem ONLINE. Mode transitions are
// serialized across slots (one DMS operating mode). A failure/timeout is reconciled by reading the mode back: if the
// modem is in the target mode the request counts as applied, otherwise every slot's cache follows the readback.
bool ModemCore::setRadioPower(bool on) {
    static std::mutex sPowerLock;
    std::lock_guard<std::mutex> pl(sPowerLock);
    // The modem has one operating mode for both subscriptions: ONLINE while any slot wants the
    // radio, LOW_POWER when every slot voted off (airplane mode). Single SIM: identical to v1.
    const bool modemOn = powerVote().preview(mSub, on);
    auto r = dms::setOperatingMode(*mCtl, modemOn ? dms::kOnline : dms::kLowPower);
    LOG(INFO) << "radio power slot" << mSub + 1 << " " << (on ? "ON" : "OFF") << " -> modem "
              << (modemOn ? "online" : "low power") << ": " << r.describe();
    if (!r.ok() && !(r.status == Result::QmiFailure && r.qmiError == kErrNoEffect)) {
        uint8_t mode = dms::kUnknownMode;
        auto rb = dms::getOperatingMode(*mCtl, &mode);
        const bool target = modemOn ? mode == dms::kOnline : mode == dms::kLowPower;
        LOG(WARNING) << "radio power slot" << mSub + 1 << " not applied; readback " << rb.describe() << " mode "
                     << int(mode) << (rb.ok() && target ? " = target (counted as applied)" : " (vote kept)");
        if (!rb.ok()) return false;  // unknown: keep the vote and the cache, Android sees the failure
        if (!target) {
            publishModemMode(mode == dms::kOnline);
            return false;
        }
    }
    powerVote().vote(mSub, on);
    for (int s = 0; s < slotCount(); s++) {
        auto& c = get(s + 1);
        {
            std::lock_guard<std::mutex> l(c.mCacheLock);
            c.mRadioOn = modemOn;
        }
        if (&c == this) continue;
        // the other slot's state only changes if the modem mode flipped under its own vote
        bool other = modemOn && powerVote().wants(s);
        c.post([&c, other] { c.each([other](Listener* l) { l->onRadioPowerChanged(other); }); });
    }
    post([this, on, modemOn] { each([v = on && modemOn](Listener* l) { l->onRadioPowerChanged(v); }); });
    // pinsafe (27 Sep 2026): LOW_POWER -> ONLINE powers the SIM down and up again (PIN needed again).
    // Re-arm provisioning and re-read the card after the SIM restarted, so Android sees the new PIN
    // state even when the modem sends no card status indication. The HAL never re-sends a PIN itself.
    for (int s = 0; s < slotCount(); s++) {
        auto& c = get(s + 1);
        {
            std::lock_guard<std::mutex> l(c.mCacheLock);
            c.mProvisionTries = 0;
        }
        for (int ms : {2000, 6000})
            c.postDelayed(ms, [&c] {
                if (!c.ready()) return;
                c.cardStatus(true);
                c.provisionIfNeeded();
                c.each([](Listener* l) { l->onSimChanged(); });
            });
    }
    return true;
}

}  // namespace android::hardware::radio::a6l
