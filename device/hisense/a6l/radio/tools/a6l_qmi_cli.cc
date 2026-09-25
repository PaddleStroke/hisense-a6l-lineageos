// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): `a6l-qmi` - QMI-over-QRTR test tool for attended sessions.
// Runs in the V74 recovery RAM session next to the radio2 daemons (static NDK build) and in the ROM.
//
// Read-only commands: lookup, info, sim, reg, watch, mode get
// RF commands (need A6L_RF_APPROVED=1, and Pierre's explicit go):
//   mode online, sms-send (+ A6L_SMS_TO=<same number>), dial (+ A6L_DIAL_TO=<same number>), data, scan
#include <a6lqmi/client.h>
#include <a6lqmi/datacall.h>
#include <a6lqmi/log.h>
#include <a6lqmi/rmnet.h>
#include <a6lqmi/services.h>
#include <a6lqmi/sms.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace a6l;
using namespace a6l::qmi;

namespace {

const std::vector<uint32_t> kAll = {kSvcDms, kSvcUim, kSvcNas, kSvcWms, kSvcVoice, kSvcWds, kSvcWda};

void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void out(const char* fmt, ...) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

bool rfApproved(const char* what) {
    const char* e = getenv("A6L_RF_APPROVED");
    if (e && !strcmp(e, "1")) return true;
    out("A6L_QMI_REFUSED %s: RF action, needs A6L_RF_APPROVED=1 (Pierre's explicit go)", what);
    return false;
}

bool envEquals(const char* var, const std::string& v, const char* what) {
    const char* e = getenv(var);
    if (e && v == e) return true;
    out("A6L_QMI_REFUSED %s: set %s=%s to confirm the destination", what, var, v.c_str());
    return false;
}

const char* regName(uint8_t s) {
    switch (s) {
        case nas::kNotRegistered: return "not-registered";
        case nas::kRegistered: return "registered";
        case nas::kSearching: return "searching";
        case nas::kDenied: return "denied";
        default: return "unknown";
    }
}
const char* ratName(int8_t r) {
    switch (r) {
        case nas::kRifGsm: return "gsm";
        case nas::kRifUmts: return "umts";
        case nas::kRifLte: return "lte";
        case nas::kRifTdscdma: return "tdscdma";
        case nas::kRif5gnr: return "nr";
        case nas::kRifNone: return "none";
        default: return "other";
    }
}
const char* callStateName(uint8_t s) {
    static const char* n[] = {"unknown", "origination", "incoming", "conversation", "cc-in-progress",
                              "alerting", "hold", "waiting", "disconnecting", "end", "setup"};
    return s < 11 ? n[s] : "?";
}

void printServing(const nas::ServingSystem& s, const char* tag) {
    out("%s reg=%s cs=%u ps=%u rat=%s roaming=%s plmn=%s%s '%s' lac=%d tac=%d cid=%ld", tag,
        regName(s.regState), s.csAttach, s.psAttach, ratName(s.primaryRat()),
        s.roaming ? (*s.roaming ? "yes" : "no") : "?", s.hasPlmn ? s.mccStr().c_str() : "-",
        s.hasPlmn ? s.mncStr().c_str() : "", s.description.c_str(), s.lac ? *s.lac : -1,
        s.tac ? *s.tac : -1, s.cid ? static_cast<long>(*s.cid) : -1L);
}

void printSignal(const nas::SignalInfo& s, const char* tag) {
    std::string o = tag;
    char b[96];
    if (s.gsmRssi) { snprintf(b, sizeof b, " gsm_rssi=%d", *s.gsmRssi); o += b; }
    if (s.wcdmaRssi) { snprintf(b, sizeof b, " wcdma_rssi=%d ecio=%.1f", *s.wcdmaRssi, s.wcdmaEcio ? -0.5 * *s.wcdmaEcio : 0.0); o += b; }
    if (s.hasLte) { snprintf(b, sizeof b, " lte_rssi=%d rsrq=%d rsrp=%d snr=%.1f", s.lteRssi, s.lteRsrq, s.lteRsrp, s.lteSnr / 10.0); o += b; }
    if (o == tag) o += " none";
    out("%s", o.c_str());
}

int cmdLookup(Client& c) {
    c.waitForServices(kAll, 3000);
    for (auto& [svc, v] : c.servers())
        for (auto& [a, inst] : v)
            out("A6L_QMI_SERVICE %s(%u) node=%u port=%u instance=%u version=%u", serviceName(svc), svc,
                a.node, a.port, inst >> 8, inst & 0xff);
    auto missing = c.waitForServices(kAll, 0);
    for (auto s : missing) out("A6L_QMI_MISSING %s(%u)", serviceName(s), s);
    return missing.empty() ? 0 : 2;
}

int cmdInfo(Client& c) {
    dms::Ids ids;
    auto r = dms::getIds(c, &ids);
    out("A6L_QMI_IDS %s imei=%s imeisv=%s meid=%s", r.describe().c_str(), ids.imei.c_str(),
        ids.imeisv.c_str(), ids.meid.c_str());
    std::string rev;
    r = dms::getRevision(c, &rev);
    out("A6L_QMI_REVISION %s '%s'", r.describe().c_str(), rev.c_str());
    uint8_t mode = 0xff;
    r = dms::getOperatingMode(c, &mode);
    out("A6L_QMI_OPMODE %s mode=%u (0 online, 1 low-power, 3 offline)", r.describe().c_str(), mode);
    std::string msisdn;
    r = dms::getMsisdn(c, &msisdn);
    out("A6L_QMI_MSISDN %s '%s'", r.describe().c_str(), msisdn.c_str());
    return 0;
}

int cmdSim(Client& c) {
    uim::CardStatus cs;
    auto r = uim::getCardStatus(c, &cs);
    out("A6L_QMI_CARD_STATUS %s cards=%zu gw_primary=0x%04x", r.describe().c_str(), cs.cards.size(),
        cs.indexGwPrimary);
    for (size_t i = 0; i < cs.cards.size(); i++) {
        auto& k = cs.cards[i];
        out("A6L_QMI_CARD %zu state=%u (0 absent 1 present 2 error) error=%u upin=%u", i, k.state,
            k.error, k.upinState);
        for (size_t j = 0; j < k.apps.size(); j++) {
            auto& a = k.apps[j];
            out("A6L_QMI_APP %zu.%zu type=%u (1 sim 2 usim 5 isim) state=%u (7 ready 2 pin 3 puk) "
                "pin1=%u retries=%u/%u aid=%s", i, j, a.type, a.state, a.pin1State, a.pin1Retries,
                a.puk1Retries, hex(a.aid).c_str());
        }
    }
    std::string iccid, imsi;
    r = uim::readIccid(c, &iccid);
    out("A6L_QMI_ICCID %s %s", r.describe().c_str(), iccid.c_str());
    const uim::App* app = cs.primaryGwApp();
    if (app && app->state == uim::kAppStateReady) {
        r = uim::readImsi(c, app->aid, &imsi);
        // print MCC/MNC + masked rest (the log may be shared)
        std::string masked = imsi.size() > 6 ? imsi.substr(0, 6) + std::string(imsi.size() - 6, '*') : imsi;
        out("A6L_QMI_IMSI %s %s (masked; A6L_SHOW_IMSI=1 for full)", r.describe().c_str(),
            getenv("A6L_SHOW_IMSI") ? imsi.c_str() : masked.c_str());
    } else {
        out("A6L_QMI_IMSI skipped: primary app not ready (PIN?)");
    }
    return 0;
}

int cmdPin(Client& c, const std::string& pin) {
    uim::CardStatus cs;
    uim::getCardStatus(c, &cs);
    const uim::App* app = cs.primaryGwApp();
    uim::Session s{uim::kSessionPrimaryGw, app ? app->aid : std::vector<uint8_t>{}};
    uim::PinResult pr;
    auto r = uim::verifyPin(c, s, uim::kPin1, pin, &pr);
    out("A6L_QMI_VERIFY_PIN %s retries_left=%d puk_left=%d", r.describe().c_str(), pr.verifyLeft,
        pr.unblockLeft);
    return r.ok() ? 0 : 1;
}

int cmdMode(Client& c, const std::string& m) {
    if (m == "get") {
        uint8_t mode = 0xff;
        auto r = dms::getOperatingMode(c, &mode);
        out("A6L_QMI_OPMODE %s mode=%u", r.describe().c_str(), mode);
        return 0;
    }
    uint8_t target;
    if (m == "online") {
        if (!rfApproved("mode online")) return 3;
        target = dms::kOnline;
    } else if (m == "lowpower") {
        target = dms::kLowPower;  // reduces RF: always allowed
    } else {
        out("usage: mode get|online|lowpower");
        return 1;
    }
    auto r = dms::setOperatingMode(c, target);
    out("A6L_QMI_SET_OPMODE %s -> %u", r.describe().c_str(), target);
    return r.ok() ? 0 : 1;
}

int cmdReg(Client& c) {
    nas::ServingSystem s;
    auto r = nas::getServingSystem(c, &s);
    if (r.ok()) printServing(s, "A6L_QMI_SERVING");
    else out("A6L_QMI_SERVING %s", r.describe().c_str());
    nas::SignalInfo si;
    r = nas::getSignalInfo(c, &si);
    if (r.ok()) printSignal(si, "A6L_QMI_SIGNAL");
    else out("A6L_QMI_SIGNAL %s", r.describe().c_str());
    nas::OperatorName on;
    r = nas::getOperatorName(c, &on);
    out("A6L_QMI_OPERATOR %s spn='%s' long='%s' short='%s' str='%s'", r.describe().c_str(),
        on.spn.c_str(), on.longName.c_str(), on.shortName.c_str(), on.operatorString.c_str());
    nas::LteCell lc;
    r = nas::getLteCell(c, &lc);
    if (lc.valid)
        out("A6L_QMI_LTE_CELL earfcn=%u pci=%u tac=%u gci=%u rsrp=%d", lc.earfcn, lc.pci, lc.tac,
            lc.globalCellId, lc.rsrp);
    else
        out("A6L_QMI_LTE_CELL %s none", r.describe().c_str());
    uint16_t mm = 0;
    r = nas::getModePreference(c, &mm);
    out("A6L_QMI_MODE_PREF %s mask=0x%x (4 gsm, 8 umts, 16 lte)", r.describe().c_str(), mm);
    return 0;
}

void registerWatchers(Client& c, bool sms, bool calls) {
    c.onIndication(kSvcNas, nas::kGetServingSystem, [](const Message& m) {
        if (auto s = nas::parseServingSystem(m, true)) printServing(*s, "A6L_QMI_IND_SERVING");
    });
    c.onIndication(kSvcNas, nas::kSignalInfoInd,
                   [](const Message& m) { printSignal(nas::parseSignalInfo(m), "A6L_QMI_IND_SIGNAL"); });
    c.onIndication(kSvcNas, nas::kSystemInfoInd, [](const Message&) { out("A6L_QMI_IND_SYSINFO"); });
    c.onIndication(kSvcNas, nas::kNetworkTimeInd, [](const Message& m) {
        Reader r(m.get(0x01));
        unsigned y = r.u16(), mo = r.u8(), d = r.u8(), h = r.u8(), mi = r.u8(), s = r.u8();
        out("A6L_QMI_IND_NITZ %04u-%02u-%02u %02u:%02u:%02u UTC", y, mo, d, h, mi, s);
    });
    c.onIndication(kSvcUim, uim::kCardStatusInd, [](const Message& m) {
        auto* t = m.get(0x10);
        auto cs = t ? uim::parseCardStatus(*t) : std::nullopt;
        auto* a = cs ? cs->primaryGwApp() : nullptr;
        out("A6L_QMI_IND_CARD app_state=%d", a ? a->state : -1);
    });
    if (calls) {
        c.onIndication(kSvcVoice, voice::kAllCallStatusInd, [](const Message& m) {
            for (auto& ci : voice::parseCalls(m, true))
                out("A6L_QMI_IND_CALL id=%u state=%s dir=%s number=%s", ci.id, callStateName(ci.state),
                    ci.direction == voice::kDirMt ? "MT" : "MO", ci.hasNumber ? ci.number.c_str() : "?");
        });
    }
    (void)sms;
}

int cmdWatch(Client& c, int secs) {
    registerWatchers(c, false, true);
    out("A6L_QMI_NAS_REGISTER %s", nas::registerIndications(c).describe().c_str());
    out("A6L_QMI_NAS_SIGNAL_CFG %s", nas::configSignalInfo(c).describe().c_str());
    out("A6L_QMI_UIM_EVENTS %s", uim::registerEvents(c).describe().c_str());
    out("A6L_QMI_VOICE_IND %s", voice::indicationRegister(c).describe().c_str());
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    return 0;
}

int cmdSmsListen(Client& c, int secs) {
    std::atomic<int> got{0};
    c.onIndication(kSvcWms, wms::kSetEventReport, [&c, &got](const Message& m) {
        auto e = wms::parseEventReport(m);
        std::vector<uint8_t> pdu;
        if (e.hasTransfer) {
            pdu = e.data;
            if (e.ackIndicator == 0) {
                // ack from another thread: requests must not run on the dispatch thread of `c`
                uint32_t txn = e.txn;
                std::thread([&c, txn] {
                    out("A6L_QMI_SMS_ACK %s", wms::sendAck(c, txn, true, 0, 0).describe().c_str());
                }).detach();
            }
        } else if (e.stored) {
            out("A6L_QMI_SMS_STORED storage=%u index=%u (reading)", e.stored->first, e.stored->second);
            auto st = *e.stored;
            std::thread([&c, st] {
                uint8_t fmt;
                std::vector<uint8_t> d;
                auto r = wms::rawRead(c, st.first, st.second, &fmt, &d);
                out("A6L_QMI_SMS_READ %s fmt=%u pdu=%s", r.describe().c_str(), fmt, hex(d).c_str());
                sms::SmscForm f;
                if (auto p = sms::parseDeliver(d, sms::SmscForm::Auto, &f))
                    out("A6L_QMI_SMS_RX from=%s form=%s text='%s'", p->originator.c_str(),
                        f == sms::SmscForm::Bare ? "bare-tpdu" : "smsc-prefixed", p->text.c_str());
                else
                    out("A6L_QMI_SMS_UNPARSED fmt=%u", fmt);
                wms::deleteMessage(c, st.first, st.second);
            }).detach();
            return;
        }
        got++;
        out("A6L_QMI_SMS_PDU %s", hex(pdu).c_str());
        sms::SmscForm f;
        if (auto p = sms::parseDeliver(pdu, sms::SmscForm::Auto, &f))
            out("A6L_QMI_SMS_RX from=%s smsc=%s ts=%s status_report=%d form=%s fmt=%u text='%s'",
                p->originator.c_str(), p->smsc.c_str(), p->timestamp.c_str(), p->statusReport,
                f == sms::SmscForm::Bare ? "bare-tpdu" : "smsc-prefixed", e.format, p->text.c_str());
        else
            out("A6L_QMI_SMS_UNPARSED fmt=%u (6 = GW point-to-point)", e.format);
    });
    auto r = wms::setRoutes(c, false);
    out("A6L_QMI_SMS_ROUTES transfer-only %s", r.describe().c_str());
    if (!r.ok()) out("A6L_QMI_SMS_ROUTES store-and-notify %s", wms::setRoutes(c, true).describe().c_str());
    out("A6L_QMI_SMS_EVENTS %s", wms::setEventReport(c, true).describe().c_str());
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    out("A6L_QMI_SMS_LISTEN_DONE received=%d", got.load());
    return 0;
}

int cmdSmsSend(Client& c, const std::string& number, const std::string& text) {
    if (!rfApproved("sms-send") || !envEquals("A6L_SMS_TO", number, "sms-send")) return 3;
    auto pdu = sms::buildSubmit(number, text);
    if (!pdu) {
        out("A6L_QMI_SMS_SEND bad number or text (ASCII letters/digits/punctuation, <=160)");
        return 1;
    }
    out("A6L_QMI_SMS_PDU %s", hex(*pdu).c_str());
    wms::SendResult sr;
    auto r = wms::rawSend(c, *pdu, false, &sr);
    out("A6L_QMI_SMS_SEND %s mr=%d rp=%d tp=%d failure_type=%d", r.describe().c_str(),
        sr.messageRef ? *sr.messageRef : -1, sr.rpCause ? *sr.rpCause : -1, sr.tpCause ? *sr.tpCause : -1,
        sr.failureType ? *sr.failureType : -1);
    return r.ok() ? 0 : 1;
}

int cmdDial(Client& c, const std::string& number, int secs) {
    if (!rfApproved("dial") || !envEquals("A6L_DIAL_TO", number, "dial")) return 3;
    registerWatchers(c, false, true);
    voice::indicationRegister(c);
    uint8_t id = 0;
    auto r = voice::dial(c, number, false, &id);
    out("A6L_QMI_DIAL %s call_id=%u (no call audio on this kernel yet)", r.describe().c_str(), id);
    if (!r.ok()) return 1;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::vector<voice::CallInfo> calls;
        voice::getAllCalls(c, &calls);
        bool alive = false;
        for (auto& ci : calls)
            if (ci.id == id && ci.state != voice::kStateEnd) alive = true;
        if (!alive) {
            out("A6L_QMI_DIAL_ENDED by network/remote");
            return 0;
        }
    }
    out("A6L_QMI_HANGUP %s", voice::endCall(c, id).describe().c_str());
    return 0;
}

int cmdHangupAll(Client& c) {
    std::vector<voice::CallInfo> calls;
    auto r = voice::getAllCalls(c, &calls);
    out("A6L_QMI_CALLS %s n=%zu", r.describe().c_str(), calls.size());
    for (auto& ci : calls) out("A6L_QMI_END %u %s", ci.id, voice::endCall(c, ci.id).describe().c_str());
    return 0;
}

int cmdCallWait(Client& c, int secs) {
    registerWatchers(c, false, true);
    out("A6L_QMI_VOICE_IND %s", voice::indicationRegister(c).describe().c_str());
    bool answer = getenv("A6L_ANSWER") && !strcmp(getenv("A6L_ANSWER"), "1");
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::vector<voice::CallInfo> calls;
        voice::getAllCalls(c, &calls);
        for (auto& ci : calls)
            if (ci.state == voice::kStateIncoming && answer) {
                out("A6L_QMI_ANSWER %u %s", ci.id, voice::answer(c, ci.id).describe().c_str());
                answer = false;
            }
    }
    return cmdHangupAll(c);
}

int cmdCallList(Client& c) {
    std::vector<voice::CallInfo> calls;
    auto r = voice::getAllCalls(c, &calls);
    out("A6L_QMI_CALL_LIST %s n=%zu", r.describe().c_str(), calls.size());
    for (auto& ci : calls)
        out("A6L_QMI_CALL id=%u state=%s(%u) dir=%s type=%u mode=%u mpty=%d number=%s", ci.id,
            callStateName(ci.state), ci.state, ci.direction == voice::kDirMt ? "MT" : "MO", ci.type,
            ci.mode, ci.multiparty, ci.hasNumber ? ci.number.c_str() : "?");
    return r.ok() ? 0 : 1;
}

// DTMF on the active (conversation) call. Waits up to waitS seconds for the call to be answered.
// Default: continuous start/stop per digit (GSM/UMTS/LTE, like Android's startDtmf/stopDtmf);
// A6L_DTMF_MODE=burst uses BURST_DTMF (CDMA-style, the modem may reject it on LTE/GSM).
int cmdDtmf(Client& c, const std::string& digits, int waitS) {
    for (char d : digits)
        if (!((d >= '0' && d <= '9') || d == '*' || d == '#' || (d >= 'A' && d <= 'D'))) {
            out("A6L_QMI_DTMF bad digit '%c' (0-9 * # A-D)", d);
            return 1;
        }
    if (digits.empty()) {
        out("usage: dtmf <digits> [wait_s]");
        return 1;
    }
    int id = -1;
    auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        std::vector<voice::CallInfo> calls;
        voice::getAllCalls(c, &calls);
        for (auto& ci : calls)
            if (ci.state == voice::kStateConversation) id = ci.id;
        if (id >= 0 || std::chrono::steady_clock::now() - t0 > std::chrono::seconds(waitS)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (id < 0) {
        out("A6L_QMI_DTMF_FAIL no call in conversation state after %d s (call-list shows the calls)", waitS);
        return 1;
    }
    const char* mode = getenv("A6L_DTMF_MODE");
    int onMs = getenv("A6L_DTMF_ON_MS") ? atoi(getenv("A6L_DTMF_ON_MS")) : 250;
    int offMs = getenv("A6L_DTMF_OFF_MS") ? atoi(getenv("A6L_DTMF_OFF_MS")) : 150;
    if (mode && !strcmp(mode, "burst")) {
        auto r = voice::burstDtmf(c, static_cast<uint8_t>(id), digits);
        out("A6L_QMI_DTMF_BURST call=%d '%s' %s", id, digits.c_str(), r.describe().c_str());
        return r.ok() ? 0 : 1;
    }
    int fails = 0;
    for (char d : digits) {
        auto r1 = voice::startDtmf(c, static_cast<uint8_t>(id), d);
        std::this_thread::sleep_for(std::chrono::milliseconds(onMs));
        auto r2 = voice::stopDtmf(c, static_cast<uint8_t>(id));
        out("A6L_QMI_DTMF call=%d digit=%c start=%s stop=%s", id, d, r1.describe().c_str(),
            r2.describe().c_str());
        if (!r1.ok() || !r2.ok()) fails++;
        std::this_thread::sleep_for(std::chrono::milliseconds(offMs));
    }
    out("A6L_QMI_DTMF_%s digits=%zu failures=%d", fails ? "FAIL" : "PASS", digits.size(), fails);
    return fails ? 1 : 0;
}

int cmdData(Client& c, const std::string& apn, const std::string& proto, int keep) {
    if (!rfApproved("data")) return 3;
    radio::DataConfig cfg;
    cfg.requireNetdev = rmnet::exists(cfg.parentIface);
    cfg.setDataFormat = cfg.requireNetdev;
    out("A6L_QMI_DATA netdev %s: %s", cfg.parentIface.c_str(),
        cfg.requireNetdev ? "present (full path)" : "absent (modem-only: IP/DNS check, no traffic)");
    radio::DataCallManager dm(cfg, [](const char* tag) {
        return std::make_unique<Client>(makeQrtrTransport(), tag);
    }, &c);
    radio::DataRequest rq;
    rq.apn = apn;
    rq.protocol = proto == "v4" ? radio::Protocol::V4 : proto == "v6" ? radio::Protocol::V6 : radio::Protocol::V4V6;
    auto o = dm.setup(rq);
    if (!o.ok) {
        out("A6L_QMI_DATA_FAIL cause=0x%x %s", o.failCause, o.detail.c_str());
        return 1;
    }
    std::string a, g, d;
    for (auto& x : o.call.addresses) a += x + " ";
    for (auto& x : o.call.gateways) g += x + " ";
    for (auto& x : o.call.dnses) d += x + " ";
    out("A6L_QMI_DATA_UP if=%s mux=%u addr=[%s] gw=[%s] dns=[%s] mtu4=%d mtu6=%d", o.call.ifname.c_str(),
        o.call.muxId, a.c_str(), g.c_str(), d.c_str(), o.call.mtuV4, o.call.mtuV6);
    out("A6L_QMI_DATA_DNS_%s", o.call.dnses.empty() ? "MISSING" : "PASS");
    std::this_thread::sleep_for(std::chrono::seconds(keep));
    dm.deactivateAll();
    out("A6L_QMI_DATA_DOWN");
    return 0;
}

int cmdScan(Client& c) {
    if (!rfApproved("scan")) return 3;
    std::vector<nas::ScanEntry> v;
    auto r = nas::networkScan(c, &v);
    out("A6L_QMI_SCAN %s n=%zu", r.describe().c_str(), v.size());
    for (auto& e : v)
        out("A6L_QMI_NET %03u-%02u status=0x%x rat=%s '%s'", e.mcc, e.mnc, e.status, ratName(e.rat),
            e.description.c_str());
    return 0;
}

void usage() {
    puts("a6l-qmi [-v|-vv] <command>\n"
         "  lookup | info | sim | reg | watch <s> | mode get|lowpower\n"
         "  pin <PIN1>                      verify PIN1 (only if the SIM asks for it)\n"
         "  sms-listen <s>                  print (and ack) incoming SMS\n"
         "RF (A6L_RF_APPROVED=1):\n"
         "  mode online | scan\n"
         "  sms-send <+number> <text>       also A6L_SMS_TO=<+number>\n"
         "  dial <number> [seconds]         also A6L_DIAL_TO=<number>; hangs up after seconds (30)\n"
         "  call-wait <s>                   print incoming calls; A6L_ANSWER=1 answers\n"
         "  hangup-all | call-list\n"
         "  dtmf <digits> [wait_s]          DTMF on the call in conversation (start/stop per digit;\n"
         "                                  A6L_DTMF_MODE=burst, A6L_DTMF_ON_MS/OFF_MS)\n"
         "  data <apn> [v4|v6|v4v6] [keep_s]  start a data call, print IP/GW/DNS/MTU, stop\n");
}

}  // namespace

int main(int argc, char** argv) {
    int i = 1;
    gLogLevel = 1;
    while (i < argc && argv[i][0] == '-') {
        if (!strcmp(argv[i], "-v")) gLogLevel = 3;
        else if (!strcmp(argv[i], "-vv")) gLogLevel = 4;
        i++;
    }
    if (i >= argc) {
        usage();
        return 1;
    }
    std::string cmd = argv[i++];
    auto arg = [&](int k, const char* def) -> std::string { return i + k < argc ? argv[i + k] : def; };

    Client c(makeQrtrTransport(), "cli");
    if (!c.start(kAll)) {
        out("A6L_QMI_FAIL cannot open AF_QIPCRTR socket (qrtr module? modem running?)");
        return 2;
    }
    auto missing = c.waitForServices({kSvcDms, kSvcUim, kSvcNas}, 5000);
    if (!missing.empty() && cmd != "lookup") {
        out("A6L_QMI_FAIL core services missing (modem not up?)");
        return 2;
    }
    c.waitForServices(kAll, 1000);
    int rc = 1;
    if (cmd == "lookup") rc = cmdLookup(c);
    else if (cmd == "info") rc = cmdInfo(c);
    else if (cmd == "sim") rc = cmdSim(c);
    else if (cmd == "pin") rc = cmdPin(c, arg(0, ""));
    else if (cmd == "mode") rc = cmdMode(c, arg(0, "get"));
    else if (cmd == "reg") rc = cmdReg(c);
    else if (cmd == "watch") rc = cmdWatch(c, atoi(arg(0, "60").c_str()));
    else if (cmd == "sms-listen") rc = cmdSmsListen(c, atoi(arg(0, "120").c_str()));
    else if (cmd == "sms-send") rc = cmdSmsSend(c, arg(0, ""), arg(1, ""));
    else if (cmd == "dial") rc = cmdDial(c, arg(0, ""), atoi(arg(1, "30").c_str()));
    else if (cmd == "call-wait") rc = cmdCallWait(c, atoi(arg(0, "60").c_str()));
    else if (cmd == "hangup-all") rc = cmdHangupAll(c);
    else if (cmd == "call-list") rc = cmdCallList(c);
    else if (cmd == "dtmf") rc = cmdDtmf(c, arg(0, ""), atoi(arg(1, "30").c_str()));
    else if (cmd == "data") rc = cmdData(c, arg(0, ""), arg(1, "v4v6"), atoi(arg(2, "20").c_str()));
    else if (cmd == "scan") rc = cmdScan(c);
    else usage();
    c.stop();
    out("A6L_QMI_DONE %s rc=%d", cmd.c_str(), rc);
    return rc;
}
