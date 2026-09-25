// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): shared modem state.
#define LOG_TAG "a6l-radio"
#include "ModemCore.h"

#include <a6lqmi/log.h>
#include <a6lqmi/sms.h>
#include <android-base/logging.h>
#include <android-base/properties.h>

#include <chrono>
#include <cstdio>

namespace android::hardware::radio::a6l {

using namespace ::a6l::qmi;
using ::android::base::GetBoolProperty;
using ::android::base::GetIntProperty;
using ::android::base::GetProperty;
using namespace std::chrono_literals;

namespace {
const std::vector<uint32_t> kCore = {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice};
const std::vector<uint32_t> kAll = {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda};

bool isStatusReport(const std::vector<uint8_t>& pdu) {
    if (pdu.empty()) return false;
    size_t smsc = pdu[0];
    if (smsc + 1 >= pdu.size()) return false;
    return (pdu[smsc + 1] & 0x03) == 0x02;
}
}  // namespace

ModemCore& ModemCore::get() {
    static ModemCore* sCore = new ModemCore();  // never destroyed (service lives forever)
    return *sCore;
}

ModemCore::ModemCore() {
    mCfg.dataParent = GetProperty("ro.vendor.a6l.ril.data_parent", "rmnet_ipa0");
    mCfg.rmnetFlags = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.rmnet_flags", 0x01));
    mCfg.epType = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.ep_type", 4));
    mCfg.epIface = static_cast<uint32_t>(GetIntProperty("ro.vendor.a6l.ril.ep_iface", 1));
    mCfg.smsStoreRoute = GetBoolProperty("persist.vendor.a6l.ril.sms_store_route", false);
    mCfg.logLevel = GetIntProperty("persist.vendor.a6l.ril.qmi_log", 2);
    ::a6l::qmi::gLogLevel = mCfg.logLevel;
}

void ModemCore::start() {
    mCtl = std::make_unique<Client>(makeQrtrTransport(), "ctl");
    mCtl->onServiceChange([this](uint32_t svc, bool up) { onServiceChange(svc, up); });
    wireIndications();
    ::a6l::radio::DataConfig dc;
    dc.parentIface = mCfg.dataParent;
    dc.rmnetFlags = mCfg.rmnetFlags;
    dc.epType = mCfg.epType;
    dc.epIface = mCfg.epIface;
    mData = std::make_unique<::a6l::radio::DataCallManager>(
            dc, [](const char* tag) { return std::make_unique<Client>(makeQrtrTransport(), tag); },
            mCtl.get());
    mData->onLost([this](int cid) {
        post([this, cid] { each([cid](Listener* l) { l->onDataCallLost(cid); }); });
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
    std::lock_guard<std::mutex> g(mLock);
    mListeners.push_back(l);
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
        bool needInit;
        {
            std::lock_guard<std::mutex> l(mLock);
            needInit = mNeedsInit;
        }
        if (!needInit) {
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
        if (!initModem()) {
            std::this_thread::sleep_for(3s);
            continue;
        }
        std::vector<std::function<void()>> first;
        {
            std::lock_guard<std::mutex> l(mLock);
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
    auto r = nas::registerIndications(*mCtl);
    LOG(INFO) << "NAS register indications: " << r.describe();
    r = nas::configSignalInfo(*mCtl);
    LOG(INFO) << "NAS config signal info: " << r.describe();
    r = uim::registerEvents(*mCtl);
    LOG(INFO) << "UIM register events: " << r.describe();
    r = voice::indicationRegister(*mCtl);
    LOG(INFO) << "VOICE indication register: " << r.describe();
    r = wms::setRoutes(*mCtl, mCfg.smsStoreRoute);
    LOG(INFO) << "WMS routes (" << (mCfg.smsStoreRoute ? "store" : "transfer-only") << "): " << r.describe();
    if (!r.ok() && !mCfg.smsStoreRoute) {
        r = wms::setRoutes(*mCtl, true);
        LOG(INFO) << "WMS routes fallback store-and-notify: " << r.describe();
    }
    r = wms::setEventReport(*mCtl, true);
    LOG(INFO) << "WMS event report: " << r.describe();

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
    iccid(true);
    serving(true);
    signal(true);
    calls(true);
    return true;
}

void ModemCore::onServiceChange(uint32_t svc, bool up) {
    if (up) return;  // the connect loop notices new servers
    if (svc != kSvcUim && svc != kSvcNas && svc != kSvcDms) return;
    bool was;
    {
        std::lock_guard<std::mutex> l(mLock);
        was = mReady;
        mReady = false;
        mNeedsInit = true;
    }
    if (!was) return;
    LOG(WARNING) << "modem lost (" << serviceName(svc) << " gone): modem crash/SSR or stop";
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        mCard.reset();
        mServing.reset();
        mCalls.clear();
        mRadioOn.reset();
    }
    {
        std::lock_guard<std::mutex> l(mSmsLock);
        mSmsAcks.clear();
    }
    post([this] {
        mData->modemReset();
        each([](Listener* l) { l->onModemLost(); });
    });
}

void ModemCore::wireIndications() {
    mCtl->onIndication(kSvcUim, uim::kCardStatusInd, [this](const Message& m) {
        auto* t = m.get(0x10);
        if (auto cs = t ? uim::parseCardStatus(*t) : std::nullopt) {
            std::lock_guard<std::mutex> l(mCacheLock);
            mCard = cs;
        }
        post([this] {
            iccid(true);
            each([](Listener* l) { l->onSimChanged(); });
        });
    });
    mCtl->onIndication(kSvcNas, nas::kGetServingSystem, [this](const Message& m) {
        if (auto s = nas::parseServingSystem(m, true)) {
            std::lock_guard<std::mutex> l(mCacheLock);
            mServing = s;
            mOpName.reset();
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
        int tz = 0, dst = 0;
        if (auto* t = m.get(0x10); t && !t->empty()) tz = static_cast<int8_t>((*t)[0]);
        if (auto* t = m.get(0x11); t && !t->empty()) dst = (*t)[0];
        // Android NITZ: "yy/mm/dd,hh:mm:ss(+/-)tz,dt" (tz in quarter hours, dt in hours)
        char buf[64];
        snprintf(buf, sizeof buf, "%02u/%02u/%02u,%02u:%02u:%02u%c%d,%d", y % 100, mo, d, h, mi, s,
                 tz < 0 ? '-' : '+', tz < 0 ? -tz : tz, dst);
        std::string nitz = buf;
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now().time_since_epoch()).count();
        post([this, nitz, now] { each([&](Listener* l) { l->onNitz(nitz, now); }); });
    });
    mCtl->onIndication(kSvcVoice, voice::kAllCallStatusInd, [this](const Message& m) {
        auto calls = voice::parseCalls(m, true);
        bool ringing = false;
        for (auto& c : calls)
            if (c.state == voice::kStateIncoming) ringing = true;
        {
            std::lock_guard<std::mutex> l(mCacheLock);
            mCalls = calls;
        }
        post([this, ringing] { each([ringing](Listener* l) { l->onCallsChanged(ringing); }); });
    });
    mCtl->onIndication(kSvcWms, wms::kSetEventReport, [this](const Message& m) {
        auto e = wms::parseEventReport(m);
        if (e.hasTransfer) {
            if (e.format != wms::kFormatGwPp) {
                LOG(WARNING) << "ignoring MT SMS in format " << int(e.format);
                if (e.ackIndicator == 0) {
                    uint32_t txn = e.txn;
                    post([this, txn] { wms::sendAck(*mCtl, txn, false, 0x6F /*unspecified*/, 0xFF); });
                }
                return;
            }
            {
                std::lock_guard<std::mutex> l(mSmsLock);
                mSmsAcks.emplace_back(e.txn, e.ackIndicator == 0);
            }
            // the modem sends the bare TPDU (no SMSC prefix, real capture 24 Sep); Android wants SMSC+TPDU
            auto pdu = ::a6l::sms::toSmscPrefixed(e.data);
            bool sr = isStatusReport(pdu);
            post([this, pdu, sr] { each([&](Listener* l) { l->onNewSms(pdu, sr); }); });
        } else if (e.stored) {
            auto st = *e.stored;
            post([this, st] {
                uint8_t fmt = 0;
                std::vector<uint8_t> d;
                auto r = wms::rawRead(*mCtl, st.first, st.second, &fmt, &d);
                if (!r.ok()) {
                    LOG(ERROR) << "WMS raw read " << int(st.first) << "/" << st.second << ": " << r.describe();
                    return;
                }
                wms::deleteMessage(*mCtl, st.first, st.second);  // Android keeps its own copy
                {
                    std::lock_guard<std::mutex> l(mSmsLock);
                    mSmsAcks.emplace_back(0, false);  // stored messages are acked by the modem
                }
                d = ::a6l::sms::toSmscPrefixed(d);
                bool sr = isStatusReport(d);
                each([&](Listener* l) { l->onNewSms(d, sr); });
            });
        }
    });
}

bool ModemCore::ackLastSms(bool success, uint8_t rpCause, uint8_t tpCause) {
    std::pair<uint32_t, bool> a;
    {
        std::lock_guard<std::mutex> l(mSmsLock);
        if (mSmsAcks.empty()) return false;
        a = mSmsAcks.front();
        mSmsAcks.pop_front();
    }
    if (!a.second) return true;
    auto r = wms::sendAck(*mCtl, a.first, success, rpCause, tpCause);
    if (!r.ok()) LOG(WARNING) << "WMS send ack: " << r.describe();
    return r.ok();
}

std::optional<uim::CardStatus> ModemCore::cardStatus(bool refresh) {
    if (refresh || !mCard) {
        uim::CardStatus cs;
        auto r = uim::getCardStatus(*mCtl, &cs);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok()) mCard = cs;
        else LOG(WARNING) << "UIM get card status: " << r.describe();
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mCard;
}

std::string ModemCore::iccid(bool refresh) {
    if (refresh || mIccid.empty()) {
        std::string id;
        auto r = uim::readIccid(*mCtl, &id);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok()) mIccid = id;
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mIccid;
}

std::optional<nas::ServingSystem> ModemCore::serving(bool refresh) {
    if (refresh || !mServing) {
        nas::ServingSystem s;
        auto r = nas::getServingSystem(*mCtl, &s);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok()) mServing = s;
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return mServing;
}

nas::SignalInfo ModemCore::signal(bool refresh) {
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
    if (refresh || !mOpName) {
        nas::OperatorName o;
        auto r = nas::getOperatorName(*mCtl, &o);
        std::lock_guard<std::mutex> l(mCacheLock);
        mOpName = r.ok() ? o : nas::OperatorName{};
    }
    std::lock_guard<std::mutex> l(mCacheLock);
    return *mOpName;
}

std::vector<voice::CallInfo> ModemCore::calls(bool refresh) {
    if (refresh) {
        std::vector<voice::CallInfo> c;
        auto r = voice::getAllCalls(*mCtl, &c);
        std::lock_guard<std::mutex> l(mCacheLock);
        if (r.ok()) mCalls = c;
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

bool ModemCore::radioOn() {
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        if (mRadioOn) return *mRadioOn;
    }
    uint8_t mode = dms::kUnknownMode;
    if (dms::getOperatingMode(*mCtl, &mode).ok()) {
        std::lock_guard<std::mutex> l(mCacheLock);
        mRadioOn = (mode == dms::kOnline);
        return *mRadioOn;
    }
    return false;
}

bool ModemCore::setRadioPower(bool on) {
    auto r = dms::setOperatingMode(*mCtl, on ? dms::kOnline : dms::kLowPower);
    LOG(INFO) << "radio power " << (on ? "ON (online)" : "OFF (low power)") << ": " << r.describe();
    if (!r.ok() && r.qmiError != kErrNoEffect) return false;
    {
        std::lock_guard<std::mutex> l(mCacheLock);
        mRadioOn = on;
    }
    post([this, on] { each([on](Listener* l) { l->onRadioPowerChanged(on); }); });
    return true;
}

}  // namespace android::hardware::radio::a6l
