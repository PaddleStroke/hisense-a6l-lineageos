// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): `a6l-qmi` - QMI-over-QRTR test tool for attended sessions.
// Runs in the V74 recovery RAM session next to the radio2 daemons (static NDK build) and in the ROM.
//
// Read-only commands: lookup, info, sim, reg, watch, mode get
// RF commands (need A6L_RF_APPROVED=1, and Pierre's explicit go):
//   mode online, sms-send (+ A6L_SMS_TO=<same number>), dial (+ A6L_DIAL_TO=<same number>), data, scan
#include <a6lqmi/client.h>
#include <a6lqmi/datacall.h>
#include <a6lqmi/ims.h>
#include <a6lqmi/log.h>
#include <a6lqmi/multisim.h>
#include <a6lqmi/rmnet.h>
#include <a6lqmi/services.h>
#include <a6lqmi/sms.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
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

// 27 Sep 2026 (pinsafe): the modem does not always auto-activate the primary GW session (e.g. after
// a SIM swap): the USIM app then stays DETECTED and VERIFY PIN fails with INTERNAL before reaching
// the card. Activate it the way qcril does (UIM Change Provisioning Session, primary GW, slot 1).
// Not RF: it only initialises the SIM app; registration still needs "mode online".
int provisionPrimary(Client& c, uim::CardStatus* cs) {
    uim::getCardStatus(c, cs);
    if (cs->indexGwPrimary != 0xFFFF) return 0;
    auto cand = multisim::provisioningCandidate(*cs, 0);
    if (!cand) {
        out("A6L_QMI_PROVISION1 no candidate (card absent or app unknown)");
        return 1;
    }
    auto r = multisim::changeProvisioning(c, multisim::kSessionPrimaryGw, true, cand->first, cand->second);
    out("A6L_QMI_PROVISION1 %s uim_slot=%u aid=%s", r.describe().c_str(), cand->first, hex(cand->second).c_str());
    for (int i = 0; i < 30; i++) {  // wait up to 15 s for the app to leave DETECTED
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        uim::getCardStatus(c, cs);
        const uim::App* a = cs->primaryGwApp();
        if (cs->indexGwPrimary != 0xFFFF && a && a->state != uim::kAppStateDetected &&
            a->state != uim::kAppStateUnknown) break;
    }
    return 0;
}

int cmdProvision1(Client& c) {
    uim::CardStatus cs;
    provisionPrimary(c, &cs);
    const uim::App* a = cs.primaryGwApp();
    out("A6L_QMI_PRIMARY gw_primary=0x%04x app_state=%d (2 pin 3 puk 5 blocked 7 ready) pin1=%d "
        "pin1_retries=%d puk1_retries=%d", cs.indexGwPrimary, a ? a->state : -1, a ? a->pin1State : -1,
        a ? a->pin1Retries : -1, a ? a->puk1Retries : -1);
    return a ? 0 : 1;
}

// Sends the PIN only when the card itself says: PIN required with all 3 attempts left.
// Anything else (not provisioned, PUK, blocked, fewer attempts) refuses without touching the card.
int cmdPin(Client& c, const std::string& pin) {
    uim::CardStatus cs;
    provisionPrimary(c, &cs);
    const uim::App* app = cs.primaryGwApp();
    if (!app) {
        out("A6L_QMI_VERIFY_PIN refused: no primary SIM app (gw_primary=0x%04x), PIN not sent", cs.indexGwPrimary);
        return 1;
    }
    if (app->state == uim::kAppStateReady) {
        out("A6L_QMI_VERIFY_PIN skipped: SIM already ready (PIN disabled or already verified), PIN not sent");
        return 0;
    }
    int need = getenv("A6L_PIN_MIN_RETRIES") ? atoi(getenv("A6L_PIN_MIN_RETRIES")) : 3;
    if (need < 2) need = 2;
    if (app->state != uim::kAppStatePin || app->pin1Retries < need) {
        out("A6L_QMI_VERIFY_PIN refused: app_state=%u pin1=%u retries=%u puk=%u (need state 2 and >= %d "
            "retries), PIN not sent", app->state, app->pin1State, app->pin1Retries, app->puk1Retries, need);
        return 1;
    }
    if (pin.size() < 4 || pin.size() > 8 || pin.find_first_not_of("0123456789") != std::string::npos) {
        out("A6L_QMI_VERIFY_PIN refused: PIN must be 4-8 digits (got %zu chars), PIN not sent", pin.size());
        return 1;
    }
    uim::Session s{uim::kSessionPrimaryGw, app->aid};
    uim::PinResult pr;
    auto r = uim::verifyPin(c, s, uim::kPin1, pin, &pr);
    out("A6L_QMI_VERIFY_PIN %s retries_left=%d puk_left=%d (before: %u)", r.describe().c_str(), pr.verifyLeft,
        pr.unblockLeft, app->pin1Retries);
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
                out("A6L_QMI_IND_CALL id=%u state=%s dir=%s number=%s type=%s mode=%s domain=%s", ci.id,
                    callStateName(ci.state), ci.direction == voice::kDirMt ? "MT" : "MO",
                    ci.hasNumber ? ci.number.c_str() : "?", voice::callTypeName(ci.type),
                    voice::callModeName(ci.mode), voice::callDomain(ci));  // volte3: domain for MO and MT
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
            out("A6L_QMI_SMS_MT txn=%u ack_ind=%u (0 send, 1 do-not-send) fmt=%u ims=%d mode=%d", e.txn,
                e.ackIndicator, e.format, e.smsOnIms ? (*e.smsOnIms ? 1 : 0) : -1,
                e.messageMode ? *e.messageMode : -1);
            if (e.needsAck() && !getenv("A6L_SMS_NO_ACK")) {
                // ack from another thread: requests must not run on the dispatch thread of `c`.
                // ril3: protocol from the format, SMS-on-IMS echoed, failure cause printed (ERR_84 =
                // ACK_NOT_SENT on 25 Sep). A6L_SMS_ACK_DELAY_MS delays it (timing experiment).
                uint32_t txn = e.txn;
                wms::AckOptions ao;
                ao.protocol = wms::messageProtocolFor(e.format);
                ao.smsOnIms = e.smsOnIms;
                int delay = getenv("A6L_SMS_ACK_DELAY_MS") ? atoi(getenv("A6L_SMS_ACK_DELAY_MS")) : 0;
                auto t0 = std::chrono::steady_clock::now();
                std::thread([&c, txn, ao, delay, t0] {
                    if (delay > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay));
                    int cause = -1;
                    auto r = wms::sendAck(c, txn, true, 0, 0, ao, &cause);
                    long ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                        std::chrono::steady_clock::now() - t0).count());
                    out("A6L_QMI_SMS_ACK %s txn=%u proto=%u cause=%s after_ms=%ld", r.describe().c_str(), txn,
                        ao.protocol, wms::ackFailureCauseName(cause), ms);
                }).detach();
            } else if (e.hasTransfer) {
                out("A6L_QMI_SMS_ACK skipped (%s)", e.needsAck() ? "A6L_SMS_NO_ACK" : "ack indicator: modem acks");
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
        if (auto p = sms::parseDeliver(pdu, sms::SmscForm::Auto, &f)) {
            if (p->statusReport)
                out("A6L_QMI_SMS_STATUS_REPORT mr=%u to=%s st=0x%02x (%s)", p->srMessageRef, p->originator.c_str(),
                    p->srStatus, p->srStatus == 0 ? "delivered" : p->srStatus < 0x20 ? "completed" :
                                 p->srStatus < 0x40 ? "pending" : "failed");
            else
                out("A6L_QMI_SMS_RX from=%s smsc=%s ts=%s form=%s fmt=%u dcs=0x%02x part=%u/%u ref=%u text='%s'",
                    p->originator.c_str(), p->smsc.c_str(), p->timestamp.c_str(),
                    f == sms::SmscForm::Bare ? "bare-tpdu" : "smsc-prefixed", e.format, p->dcs, p->concatSeq,
                    p->concatTotal, p->concatRef, p->text.c_str());
        }
        else
            out("A6L_QMI_SMS_UNPARSED fmt=%u (6 = GW point-to-point)", e.format);
    });
    const char* route = getenv("A6L_SMS_ROUTE");  // "ack" = transfer-and-ack (the modem acks, A/B test)
    Result r;
    if (route && !strcmp(route, "ack")) {
        r = wms::setRoutesAction(c, wms::kStorageNone, wms::kReceiptTransferAndAck);
        out("A6L_QMI_SMS_ROUTES transfer-and-ack %s", r.describe().c_str());
    } else {
        r = wms::setRoutes(c, false);
        out("A6L_QMI_SMS_ROUTES transfer-only %s", r.describe().c_str());
    }
    if (!r.ok()) out("A6L_QMI_SMS_ROUTES store-and-notify %s", wms::setRoutes(c, true).describe().c_str());
    out("A6L_QMI_SMS_EVENTS %s", wms::setEventReport(c, true).describe().c_str());
    std::this_thread::sleep_for(std::chrono::seconds(secs));
    out("A6L_QMI_SMS_LISTEN_DONE received=%d", got.load());
    return 0;
}

// ril3: UTF-8 text -> GSM7 or UCS2, concatenated parts (8-bit ref), optional delivery report
// (A6L_SMS_SRR=1: waits A6L_SMS_SR_WAIT s, default 90, for the SMS-STATUS-REPORT).
int cmdSmsSend(Client& c, const std::string& number, const std::string& text) {
    if (!rfApproved("sms-send") || !envEquals("A6L_SMS_TO", number, "sms-send")) return 3;
    sms::SubmitOptions so;
    so.concatRef = static_cast<uint8_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 0xff);
    so.statusReport = getenv("A6L_SMS_SRR") && !strcmp(getenv("A6L_SMS_SRR"), "1");
    so.forceUcs2 = getenv("A6L_SMS_UCS2") && !strcmp(getenv("A6L_SMS_UCS2"), "1");
    auto parts = sms::buildSubmitParts(number, text, so);
    if (!parts) {
        out("A6L_QMI_SMS_SEND bad number/text (digits with optional +; valid UTF-8; <= %zu parts)", so.maxParts);
        return 1;
    }
    std::atomic<int> reports{0};
    if (so.statusReport) {
        c.onIndication(kSvcWms, wms::kSetEventReport, [&c, &reports](const Message& m) {
            auto e = wms::parseEventReport(m);
            if (!e.hasTransfer) return;
            if (e.needsAck()) {
                uint32_t txn = e.txn;
                wms::AckOptions ao;
                ao.protocol = wms::messageProtocolFor(e.format);
                ao.smsOnIms = e.smsOnIms;
                std::thread([&c, txn, ao] {
                    int cause = -1;
                    auto r = wms::sendAck(c, txn, true, 0, 0, ao, &cause);
                    out("A6L_QMI_SMS_ACK %s txn=%u cause=%s", r.describe().c_str(), txn, wms::ackFailureCauseName(cause));
                }).detach();
            }
            auto p = sms::parseDeliver(e.data);
            if (p && p->statusReport) {
                reports++;
                out("A6L_QMI_SMS_STATUS_REPORT mr=%u to=%s st=0x%02x (%s)", p->srMessageRef, p->originator.c_str(),
                    p->srStatus, p->srStatus == 0 ? "delivered" : p->srStatus < 0x40 ? "pending/other" : "failed");
            } else {
                out("A6L_QMI_SMS_MT (not a status report) %s", hex(e.data).c_str());
            }
        });
        out("A6L_QMI_SMS_ROUTES transfer-only %s", wms::setRoutes(c, false).describe().c_str());
        out("A6L_QMI_SMS_EVENTS %s", wms::setEventReport(c, true).describe().c_str());
    }
    out("A6L_QMI_SMS_ENCODING %s units=%zu parts=%zu ref=%u srr=%d",
        parts->coding == sms::Coding::Gsm7 ? "gsm7" : "ucs2", parts->units, parts->pdus.size(), so.concatRef,
        so.statusReport);
    int fails = 0;
    for (size_t i = 0; i < parts->pdus.size(); i++) {
        const auto& pdu = parts->pdus[i];
        bool more = i + 1 < parts->pdus.size();
        out("A6L_QMI_SMS_PDU %zu/%zu %s", i + 1, parts->pdus.size(), hex(pdu).c_str());
        wms::SendResult sr;
        auto r = wms::rawSend(c, pdu, more, &sr);
        out("A6L_QMI_SMS_SEND %zu/%zu %s mr=%d rp=%d tp=%d failure_type=%d", i + 1, parts->pdus.size(),
            r.describe().c_str(), sr.messageRef ? *sr.messageRef : -1, sr.rpCause ? *sr.rpCause : -1,
            sr.tpCause ? *sr.tpCause : -1, sr.failureType ? *sr.failureType : -1);
        if (!r.ok()) {
            fails++;
            break;
        }
    }
    if (so.statusReport && !fails) {
        int waitS = getenv("A6L_SMS_SR_WAIT") ? atoi(getenv("A6L_SMS_SR_WAIT")) : 90;
        for (int t = 0; t < waitS && reports.load() < static_cast<int>(parts->pdus.size()); t++)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        out("A6L_QMI_SMS_STATUS_REPORTS %d/%zu", reports.load(), parts->pdus.size());
    }
    return fails ? 1 : 0;
}

// ril3: voice RAT during calls (Astra H25; CSFB LTE -> UMTS seen on 25 Sep). Prints on change.
void callRatPoll(Client& c, std::string* last, const char* phase) {
    nas::ServingSystem s;
    auto r = nas::getServingSystem(c, &s);
    std::string rat = r.ok() ? nas::radioIfList(s) : "?";
    if (rat == *last) return;
    out("A6L_QMI_CALL_RAT %s rat=%s%s cs=%u ps=%u (was %s)", phase, rat.c_str(),
        (*last == "lte" && (rat == "umts" || rat == "gsm")) ? " CSFB" : "", s.csAttach, s.psAttach,
        last->empty() ? "-" : last->c_str());
    *last = rat;
}

int cmdDial(Client& c, const std::string& number, int secs) {
    if (!rfApproved("dial") || !envEquals("A6L_DIAL_TO", number, "dial")) return 3;
    registerWatchers(c, false, true);
    voice::indicationRegister(c);
    uint8_t id = 0;
    std::string rat;
    callRatPoll(c, &rat, "before-dial");
    auto r = voice::dial(c, number, false, &id);
    out("A6L_QMI_DIAL %s call_id=%u (no call audio on this kernel yet)", r.describe().c_str(), id);
    if (!r.ok()) return 1;
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        callRatPoll(c, &rat, "in-call");
        std::vector<voice::CallInfo> calls;
        voice::getAllCalls(c, &calls);
        bool alive = false;
        for (auto& ci : calls)
            if (ci.id == id && ci.state != voice::kStateEnd) alive = true;
        if (!alive) {
            out("A6L_QMI_DIAL_ENDED by network/remote");
            std::this_thread::sleep_for(std::chrono::seconds(3));
            callRatPoll(c, &rat, "after-call");
            return 0;
        }
    }
    out("A6L_QMI_HANGUP %s", voice::endCall(c, id).describe().c_str());
    std::this_thread::sleep_for(std::chrono::seconds(3));
    callRatPoll(c, &rat, "after-call");
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
    std::string rat;
    callRatPoll(c, &rat, "idle");
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::vector<voice::CallInfo> calls;
        voice::getAllCalls(c, &calls);
        if (!calls.empty()) callRatPoll(c, &rat, "in-call");
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
        out("A6L_QMI_CALL id=%u state=%s(%u) dir=%s type=%u mode=%u mpty=%d number=%s domain=%s", ci.id,
            callStateName(ci.state), ci.state, ci.direction == voice::kDirMt ? "MT" : "MO", ci.type,
            ci.mode, ci.multiparty, ci.hasNumber ? ci.number.c_str() : "?", voice::callDomain(ci));
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

uint32_t envU(const char* k, uint32_t def) {
    const char* v = getenv(k);
    return v && *v ? static_cast<uint32_t>(strtoul(v, nullptr, 0)) : def;
}

// data2 (25 Sep): data-path knobs for attended variants (defaults = what the HAL does).
//   A6L_DPM=0 skip DPM Open Port | A6L_DPM_RX_EP / A6L_DPM_TX_EP (default sysfs, else 4/5)
//   A6L_WDA_UL / A6L_WDA_DL aggregation (5 QMAP, 7 QMAPv3, 0 off) | A6L_WDA_DL_PKTS / A6L_WDA_DL_SIZE
//   A6L_EP_IFACE (1) | A6L_RMNET_FLAGS (0x01)
radio::DataConfig dataConfigFromEnv() {
    radio::DataConfig cfg;
    cfg.requireNetdev = rmnet::exists(cfg.parentIface);
    cfg.setDataFormat = cfg.requireNetdev;
    cfg.dpmOpenPort = envU("A6L_DPM", 1) != 0;
    cfg.dpmRxEp = envU("A6L_DPM_RX_EP", 0);
    cfg.dpmTxEp = envU("A6L_DPM_TX_EP", 0);
    cfg.wdaUlAgg = envU("A6L_WDA_UL", cfg.wdaUlAgg);
    cfg.wdaDlAgg = envU("A6L_WDA_DL", cfg.wdaDlAgg);
    cfg.wdaDlMaxDatagrams = envU("A6L_WDA_DL_PKTS", cfg.wdaDlMaxDatagrams);
    cfg.wdaDlMaxSize = envU("A6L_WDA_DL_SIZE", cfg.wdaDlMaxSize);
    cfg.epIface = envU("A6L_EP_IFACE", cfg.epIface);
    cfg.rmnetFlags = envU("A6L_RMNET_FLAGS", cfg.rmnetFlags);
    return cfg;
}

void printReport(const std::string& rep) {
    size_t a = 0;
    while (a < rep.size()) {
        size_t b = rep.find('\n', a);
        if (b == std::string::npos) b = rep.size();
        out("A6L_QMI_DATAPATH %s", rep.substr(a, b - a).c_str());
        a = b + 1;
    }
}

// No RF: DPM Open Port + WDA Get/Set Data Format only (no network attach, no call).
int cmdDataFormat(Client& c) {
    auto cfg = dataConfigFromEnv();
    cfg.setDataFormat = true;
    out("A6L_QMI_DATAFORMAT netdev %s: %s dpm=%d ul=%u dl=%u", cfg.parentIface.c_str(),
        cfg.requireNetdev ? "present" : "absent", cfg.dpmOpenPort, cfg.wdaUlAgg, cfg.wdaDlAgg);
    radio::DataCallManager dm(cfg, [](const char* tag) {
        return std::make_unique<Client>(makeQrtrTransport(), tag);
    }, &c);
    bool ok = dm.prepareDataPath();
    printReport(dm.formatReport());
    out("A6L_QMI_DATAFORMAT_%s", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int cmdData(Client& c, const std::string& apn, const std::string& proto, int keep) {
    if (!rfApproved("data")) return 3;
    auto cfg = dataConfigFromEnv();
    out("A6L_QMI_DATA netdev %s: %s", cfg.parentIface.c_str(),
        cfg.requireNetdev ? "present (full path)" : "absent (modem-only: IP/DNS check, no traffic)");
    radio::DataCallManager dm(cfg, [](const char* tag) {
        return std::make_unique<Client>(makeQrtrTransport(), tag);
    }, &c);
    if (cfg.setDataFormat) {
        bool fok = dm.prepareDataPath();
        printReport(dm.formatReport());
        out("A6L_QMI_DATAPATH_%s", fok ? "READY" : "FAIL");
    }
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

// ril3: read-only DSDS state: per-subscription card view, dual standby pref, IMEI per
// subscription through a second client bound to the secondary subscription.
int cmdDsds(Client& c) {
    uim::CardStatus cs;
    auto r = uim::getCardStatus(c, &cs);
    out("A6L_QMI_CARD_STATUS %s cards=%zu gw_primary=0x%04x gw_secondary=0x%04x", r.describe().c_str(),
        cs.cards.size(), cs.indexGwPrimary, cs.indexGwSecondary);
    for (int sub = 0; sub < multisim::kMaxSlots; sub++) {
        auto v = multisim::viewFor(cs, sub);
        out("A6L_QMI_SLOT %d card=%d state=%d (0 absent 1 present 2 error) error=%d provisioned=%d app=%d "
            "type=%d app_state=%d", sub + 1, v.card, v.cardPtr ? v.cardPtr->state : -1,
            v.cardPtr ? v.cardPtr->error : -1, v.provisioned, v.gwAppIndex, v.gwApp ? v.gwApp->type : -1,
            v.gwApp ? v.gwApp->state : -1);
        if (auto cand = multisim::provisioningCandidate(cs, sub))
            out("A6L_QMI_SLOT %d needs provisioning: uim slot %u (a6l-qmi provision2, RF)", sub + 1, cand->first);
    }
    multisim::DualStandbyPref p;
    r = multisim::getDualStandbyPref(c, &p);
    auto o8 = [](const std::optional<uint8_t>& v) { return v ? static_cast<int>(*v) : -1; };
    out("A6L_QMI_DSB_PREF %s standby=%d priority=%d active=%d dds=%d voice=%d mask=0x%llx", r.describe().c_str(),
        o8(p.standbyPref), o8(p.prioritySubs), o8(p.activeSubs), o8(p.defaultDataSubs), o8(p.defaultVoiceSubs),
        static_cast<unsigned long long>(p.activeSubsMask.value_or(0)));
    Client c2(makeQrtrTransport(), "cli-sub2");
    if (!c2.start({kSvcDms, kSvcNas, kSvcWms, kSvcVoice}) ||
        !c2.waitForServices({kSvcDms, kSvcNas, kSvcWms, kSvcVoice}, 3000).empty()) {
        out("A6L_QMI_SUB2 cannot open a second client");
        return 1;
    }
    std::string log;
    r = multisim::bindAll(c2, 1, true, &log);
    out("A6L_QMI_SUB2_BIND %s [%s]", r.describe().c_str(), log.c_str());
    dms::Ids ids;
    r = dms::getIds(c2, &ids);
    out("A6L_QMI_SUB2_IDS %s imei=%s", r.describe().c_str(), ids.imei.c_str());
    nas::ServingSystem s;
    r = nas::getServingSystem(c2, &s);
    if (r.ok()) printServing(s, "A6L_QMI_SUB2_SERVING");
    else out("A6L_QMI_SUB2_SERVING %s", r.describe().c_str());
    c2.stop();
    return 0;
}

// ril3 (RF: starts registration of SIM 2): UIM Change Provisioning Session (secondary GW).
int cmdProvision2(Client& c) {
    if (!rfApproved("provision2")) return 3;
    uim::CardStatus cs;
    uim::getCardStatus(c, &cs);
    auto cand = multisim::provisioningCandidate(cs, 1);
    if (!cand) {
        out("A6L_QMI_PROVISION2 nothing to do (slot 2 empty, not ready or already provisioned)");
        return 0;
    }
    auto r = multisim::changeProvisioning(c, multisim::kSessionSecondaryGw, true, cand->first, cand->second);
    out("A6L_QMI_PROVISION2 %s uim_slot=%u aid=%s", r.describe().c_str(), cand->first, hex(cand->second).c_str());
    return r.ok() ? 0 : 1;
}

// ---------------------------------------------------------------- volte3 (26 Sep 2026): VoLTE steps 3-4
// IMS voice availability + the modem's voice session ids (VSID) for CS / VoLTE / VoWiFi. Read-only.
struct ImsSnapshot {
    bool imsa = false, registered = false, voiceAvail = false;
    std::string reg = "absent", svc = "absent", nas = "?", sub = "?";
    std::optional<uint32_t> lteVsid, csVsid;
};
// volte5 (28 Sep): IMSA answers INVALID_OPERATION until the client binds to a subscription (stock qcril
// ImsaModemEndPointModule::handleQmiBinding: 0x0033 {0x10 u32 sub}). Once per client socket.
static void imsaBindOnce(Client& c) {
    static std::mutex m;
    static std::vector<Client*> done;
    std::lock_guard<std::mutex> l(m);
    for (auto* p : done)
        if (p == &c) return;
    done.push_back(&c);
    const char* e = getenv("A6L_IMSDCM_SUB");
    if (e && !strcmp(e, "none")) return;
    uint32_t sub = e && *e ? static_cast<uint32_t>(atoi(e)) : 0;
    out("A6L_QMI_IMSA_BIND sub=%u %s", sub, imsa::bindSubscription(c, sub).describe().c_str());
}

ImsSnapshot imsSnapshot(Client& c, const char* tag) {
    ImsSnapshot s;
    if (c.hasService(kSvcImsa)) {
        s.imsa = true;
        imsaBindOnce(c);
        imsa::RegStatus rs;
        imsa::ServicesStatus ss;
        auto r1 = imsa::getRegStatus(c, &rs);
        auto r2 = imsa::getServicesStatus(c, &ss);
        s.reg = r1.ok() ? rs.summary() : r1.describe();
        s.svc = r2.ok() ? ss.summary() : r2.describe();
        s.registered = r1.ok() && rs.registered();
        s.voiceAvail = r2.ok() && ss.voiceAvailable();
    }
    nas::ImsVoiceSupport iv;
    auto rn = nas::getImsVoiceSupport(c, &iv);
    char b[128];
    snprintf(b, sizeof b, "%s lte_ims_voice=%d lte_voice_domain=%d(0 none,1 IMS,2 1X,3 CS)", rn.describe().c_str(),
             iv.lteImsVoice ? *iv.lteImsVoice : -1, iv.lteVoiceDomain ? static_cast<int>(*iv.lteVoiceDomain) : -1);
    s.nas = b;
    nas::SubscriptionInfo si;
    auto rs = nas::getSubscriptionInfo(c, &si);
    s.sub = rs.ok() ? si.summary() : rs.describe();
    s.lteVsid = si.lteVoiceVsid;
    s.csVsid = si.voiceVsid;
    out("A6L_VOLTE3_IMSA %s imsa=%s registered=%d voice=%d reg=[%s] services=[%s]", tag, s.imsa ? "present" : "absent",
        s.registered, s.voiceAvail, s.reg.c_str(), s.svc.c_str());
    out("A6L_VOLTE3_NAS %s %s", tag, s.nas.c_str());
    out("A6L_VOLTE3_VSID %s %s", tag, s.sub.c_str());
    // Which q6voice MVM session the VoLTE call audio needs (q6mvm.mmode1_session / a6l-q6voiced -s):
    const char* sess = "unknown";
    if (s.lteVsid) {
        switch (*s.lteVsid) {
            case 0x11C05000: sess = "mmode1"; break;  // same multimode session as the proven CS call
            case 0x10C02000: sess = "volte"; break;   // "default volte voice"
            case 0x10C01000: sess = "cs"; break;      // "default modem voice"
            case 0x11DC5000: sess = "mmode2"; break;
            default: sess = "hex"; break;
        }
    }
    out("A6L_VOLTE3_AUDIO_SESSION %s lte=%s cs=%s", tag, sess,
        !s.csVsid ? "unknown" : *s.csVsid == 0x11C05000 ? "mmode1" : *s.csVsid == 0x10C01000 ? "cs" : "other");
    return s;
}

int cmdImsStatus(Client& c) {
    c.waitForServices({kSvcImsa}, 2000);
    imsSnapshot(c, "now");
    return 0;
}

// Prints a call's domain whenever (state, type, mode) changes; detects IMS -> CS (SRVCC) per call id.
struct DomainTracker {
    std::mutex m;
    std::map<uint8_t, std::string> last;
    std::map<uint8_t, std::string> firstDomain;
    std::string best = "none";  // domain of the call once connected (conversation)
    bool srvcc = false, connected = false;
    void update(const std::vector<voice::CallInfo>& calls, const char* src) {
        std::lock_guard<std::mutex> l(m);
        for (auto& ci : calls) {
            std::string d = voice::callDomain(ci);
            char k[96];
            snprintf(k, sizeof k, "%s/%s/%s/%s", callStateName(ci.state), voice::callTypeName(ci.type),
                     voice::callModeName(ci.mode), d.c_str());
            if (last[ci.id] == k) continue;
            last[ci.id] = k;
            out("A6L_QMI_CALL_DOMAIN id=%u dir=%s state=%s type=%s(%u) mode=%s(%u) domain=%s src=%s", ci.id,
                ci.direction == voice::kDirMt ? "MT" : "MO", callStateName(ci.state), voice::callTypeName(ci.type),
                ci.type, voice::callModeName(ci.mode), ci.mode, d.c_str(), src);
            if (d != "unknown") {
                auto& f = firstDomain[ci.id];
                if (f.empty()) f = d;
                else if (f.rfind("ims", 0) == 0 && d == "cs" && !srvcc) {
                    srvcc = true;
                    out("A6L_QMI_SRVCC id=%u %s -> cs (IMS call handed over to CS)", ci.id, f.c_str());
                }
            }
            if (ci.state == voice::kStateConversation) {
                connected = true;
                if (d != "unknown") best = d;
            }
            if (best == "none" && d != "unknown" && ci.state != voice::kStateEnd) best = d;
        }
    }
};

void registerVolteWatchers(Client& c, DomainTracker* dt) {
    c.onIndication(kSvcVoice, voice::kAllCallStatusInd,
                   [dt](const Message& m) { dt->update(voice::parseCalls(m, true), "ind"); });
    c.onIndication(kSvcVoice, voice::kAudioRatChangeInd, [](const Message& m) {
        auto a = voice::parseAudioRatChange(m);
        out("A6L_QMI_AUDIO_RAT session_info=%d rat=%s(%d)", a.sessionInfo ? static_cast<int>(*a.sessionInfo) : -1,
            a.rat ? voice::callModeName(*a.rat) : "absent", a.rat ? *a.rat : -1);
    });
    // Everything else the VOICE service indicates during the test (handover/SRVCC, speech codec = HD voice,
    // sups, info rec, ...) as hex: the TLV layouts are decoded offline from these logs.
    for (uint16_t id = 0x0030; id <= 0x0080; id++) {
        if (id == voice::kAllCallStatusInd || id == voice::kAudioRatChangeInd) continue;
        c.onIndication(kSvcVoice, id, [id](const Message& m) { out("A6L_QMI_VOICE_IND 0x%04x %s", id, m.dump().c_str()); });
    }
    c.onIndication(kSvcNas, nas::kSubscriptionInfoInd, [](const Message& m) {
        out("A6L_QMI_IND_SUBINFO %s", nas::parseSubscriptionInfo(m).summary().c_str());
    });
    if (c.hasService(kSvcImsa)) {
        imsaBindOnce(c);
        c.onIndication(kSvcImsa, imsa::kRegStatusInd,
                       [](const Message& m) { out("A6L_QMI_IND_IMSA_REG %s", imsa::parseRegStatus(m).summary().c_str()); });
        c.onIndication(kSvcImsa, imsa::kServicesStatusInd, [](const Message& m) {
            out("A6L_QMI_IND_IMSA_SVC %s", imsa::parseServicesStatus(m).summary().c_str());
        });
        out("A6L_QMI_IMSA_IND_REG %s", imsa::registerIndications(c).describe().c_str());
    }
}

// dial-ims <number> [secs]: dial with the normal call type (VOICE: the modem chooses IMS when IMS voice is registered
// and voice_domain_pref = IMS preferred) or A6L_CALL_TYPE=ip (VOICE_IP: force IMS; debug). Reports the domain.
int cmdDialIms(Client& c, const std::string& number, int secs) {
    if (!rfApproved("dial-ims") || !envEquals("A6L_DIAL_TO", number, "dial-ims")) return 3;
    c.waitForServices({kSvcImsa}, 2000);
    DomainTracker dt;
    registerWatchers(c, false, false);
    registerVolteWatchers(c, &dt);
    out("A6L_QMI_VOICE_IND_REG %s", voice::indicationRegisterVolte(c).describe().c_str());
    auto pre = imsSnapshot(c, "before-dial");
    const char* ct = getenv("A6L_CALL_TYPE");
    uint8_t type = (ct && !strcmp(ct, "ip")) ? voice::kTypeVoiceIp : voice::kTypeVoice;
    std::string rat;
    callRatPoll(c, &rat, "before-dial");
    uint8_t id = 0;
    auto r = voice::dialTyped(c, number, type, &id);
    out("A6L_QMI_DIAL_IMS %s call_id=%u call_type=%s ims_registered=%d", r.describe().c_str(), id,
        voice::callTypeName(type), pre.registered);
    std::string ratDuring;
    if (r.ok()) {
        auto t0 = std::chrono::steady_clock::now();
        bool ended = false;
        while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(secs)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            callRatPoll(c, &rat, "in-call");
            if (ratDuring.find(rat) == std::string::npos) ratDuring += (ratDuring.empty() ? "" : ",") + rat;
            std::vector<voice::CallInfo> calls;
            voice::getAllCalls(c, &calls);
            dt.update(calls, "poll");
            bool alive = false;
            for (auto& ci : calls)
                if (ci.id == id && ci.state != voice::kStateEnd) alive = true;
            if (!alive) {
                out("A6L_QMI_DIAL_ENDED by network/remote");
                ended = true;
                break;
            }
        }
        if (!ended) out("A6L_QMI_HANGUP %s", voice::endCall(c, id).describe().c_str());
        std::this_thread::sleep_for(std::chrono::seconds(3));
        callRatPoll(c, &rat, "after-call");
    }
    std::lock_guard<std::mutex> l(dt.m);
    out("A6L_VOLTE3_CALL_RESULT dial=%s domain=%s connected=%d srvcc=%d rat_during=%s ims_registered_before=%d "
        "lte_vsid=0x%08X",
        r.ok() ? "ok" : "fail", dt.best.c_str(), dt.connected, dt.srvcc, ratDuring.empty() ? "-" : ratDuring.c_str(),
        pre.registered, pre.lteVsid ? *pre.lteVsid : 0u);
    return r.ok() ? 0 : 1;
}

void usage() {
    puts("a6l-qmi [-v|-vv] <command>\n"
         "  lookup | info | sim | reg | watch <s> | mode get|lowpower\n"
         "  pin <PIN1>                      verify PIN1: only if the card says PIN required, 3 tries left\n"
         "  provision1                      activate SIM 1 app (primary GW session) + show PIN state\n"
         "  sms-listen <s>                  print (and ack) incoming SMS (A6L_SMS_ROUTE=ack: modem acks;\n"
         "                                  A6L_SMS_NO_ACK=1; A6L_SMS_ACK_DELAY_MS=<ms>)\n"
         "  dsds                            slots, provisioning, dual standby pref, sub 2 IMEI/serving\n"
         "  A6L_SUB=1 <cmd>                 run <cmd> with the client bound to subscription 2\n"
         "RF (A6L_RF_APPROVED=1):\n"
         "  mode online | scan\n"
         "  sms-send <+number> <text>       also A6L_SMS_TO=<+number>; UTF-8 (GSM7 or UCS2), multipart;\n"
         "                                  A6L_SMS_SRR=1 delivery report, A6L_SMS_UCS2=1\n"
         "  provision2                      activate SIM 2 (UIM change provisioning, secondary GW)\n"
         "  dial <number> [seconds]         also A6L_DIAL_TO=<number>; hangs up after seconds (30)\n"
         "  call-wait <s>                   print incoming calls; A6L_ANSWER=1 answers\n"
         "  hangup-all | call-list\n"
         "  dial-ims <number> [seconds]     volte3: like dial + IMS status/VSIDs before, call domain (ims/cs),\n"
         "                                  SRVCC, audio RAT; A6L_CALL_TYPE=ip forces VOICE_IP\n"
         "  dtmf <digits> [wait_s]          DTMF on the call in conversation (start/stop per digit;\n"
         "                                  A6L_DTMF_MODE=burst, A6L_DTMF_ON_MS/OFF_MS)\n"
         "  data <apn> [v4|v6|v4v6] [keep_s]  start a data call, print IP/GW/DNS/MTU, stop\n"
         "no RF: ims-status                volte3: IMSA reg/services, NAS IMS voice support, modem VSIDs\n"
         "no RF: dataformat                 DPM open port + WDA data format only (env knobs: see source)\n");
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
    auto svcs = kAll;  // volte3: IMSA (33) is optional, only looked up for the VoLTE commands
    if (cmd == "dial-ims" || cmd == "ims-status" || cmd == "call-wait") svcs.push_back(kSvcImsa);
    if (!c.start(svcs)) {
        out("A6L_QMI_FAIL cannot open AF_QIPCRTR socket (qrtr module? modem running?)");
        return 2;
    }
    auto missing = c.waitForServices({kSvcDms, kSvcUim, kSvcNas}, 5000);
    if (!missing.empty() && cmd != "lookup") {
        out("A6L_QMI_FAIL core services missing (modem not up?)");
        return 2;
    }
    c.waitForServices(kAll, 1000);
    if (const char* sub = getenv("A6L_SUB"); sub && atoi(sub) > 0) {
        std::string log;
        auto rb = multisim::bindAll(c, atoi(sub), true, &log);
        out("A6L_QMI_BIND sub=%d %s [%s]", atoi(sub), rb.describe().c_str(), log.c_str());
    }
    int rc = 1;
    if (cmd == "lookup") rc = cmdLookup(c);
    else if (cmd == "info") rc = cmdInfo(c);
    else if (cmd == "sim") rc = cmdSim(c);
    else if (cmd == "pin") rc = cmdPin(c, arg(0, ""));
    else if (cmd == "provision1") rc = cmdProvision1(c);
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
    else if (cmd == "dsds") rc = cmdDsds(c);
    else if (cmd == "provision2") rc = cmdProvision2(c);
    else if (cmd == "dataformat") rc = cmdDataFormat(c);
    else if (cmd == "ims-status") rc = cmdImsStatus(c);
    else if (cmd == "dial-ims") rc = cmdDialIms(c, arg(0, ""), atoi(arg(1, "30").c_str()));
    else usage();
    c.stop();
    out("A6L_QMI_DONE %s rc=%d", cmd.c_str(), rc);
    return rc;
}
