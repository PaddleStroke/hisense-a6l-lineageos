// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): IRadioMessaging (GSM/WCDMA/LTE SMS over WMS), IRadioVoice
// (circuit-switched calls over VOICE). Call audio: q6voice/CVD (kvoice), held by a6l-q6voiced while
// vendor.a6l.voice.active=1; call mute goes to the same daemon (r5 F6, VoiceMute.h).
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"
#include "VoiceMute.h"

#include <a6lqmi/message.h>
#include <libminradio/response.h>

#include <chrono>
#include <thread>

namespace android::hardware::radio::a6l {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace qmi = ::a6l::qmi;
namespace wms = ::a6l::qmi::wms;
namespace qv = ::a6l::qmi::voice;
namespace aidlMsg = ::aidl::android::hardware::radio::messaging;
namespace aidlVoice = ::aidl::android::hardware::radio::voice;
constexpr auto ok = &ScopedAStatus::ok;

// ======================================================================== messaging
void A6lRadioMessaging::send(int32_t serial, const aidlMsg::GsmSmsMessage& m, bool more, bool ims) {
    auto smsc = m.smscPdu.empty() ? std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{0x00})
                                  : qmi::unhex(m.smscPdu);
    auto tpdu = qmi::unhex(m.pdu);
    if (!smsc || !tpdu || tpdu->empty()) {
        auto info = errorResponse(serial, RadioError::INVALID_SMS_FORMAT);
        if (ims) respond()->sendImsSmsResponse(info, {});
        else more ? respond()->sendSmsExpectMoreResponse(info, {}) : respond()->sendSmsResponse(info, {});
        return;
    }
    std::vector<uint8_t> raw = *smsc;
    raw.insert(raw.end(), tpdu->begin(), tpdu->end());
    mExec.post([this, serial, raw, more, ims] {
        auto& core = slotCore();
        aidlMsg::SendSmsResult res{.messageRef = 0, .ackPDU = "", .errorCode = -1};
        RadioError err = RadioError::NONE;
        if (!core.ready() || !core.bound()) {  // unbound slot 2 must never send on SIM 1
            err = RadioError::RADIO_NOT_AVAILABLE;
        } else {
            wms::SendResult sr;
            auto r = wms::rawSend(core.ctl(), raw, more, &sr);
            if (sr.messageRef) res.messageRef = *sr.messageRef;
            if (!r.ok()) {
                res.errorCode = sr.rpCause ? *sr.rpCause : -1;
                // temporary failures (failure type 0) and no-service are retried by the framework
                bool temporary = (sr.failureType && *sr.failureType == 0) ||
                                 r.status == qmi::Result::Timeout ||
                                 (r.status == qmi::Result::QmiFailure &&
                                  (r.qmiError == qmi::kErrNetworkNotReady || r.qmiError == qmi::kErrDeviceNotReady));
                err = temporary ? RadioError::SMS_SEND_FAIL_RETRY : RadioError::GENERIC_FAILURE;
                if (r.status == qmi::Result::QmiFailure && r.qmiError == qmi::kErrFdnRestrict)
                    err = RadioError::FDN_CHECK_FAILURE;
            }
            LOG(INFO) << "[" << serial << "] Messaging.sendSms " << r.describe() << " mr=" << res.messageRef
                      << " rp=" << (sr.rpCause ? *sr.rpCause : -1) << " tp=" << (sr.tpCause ? *sr.tpCause : -1);
        }
        auto info = err == RadioError::NONE ? noError(serial) : errorResponse(serial, err);
        if (ims) respond()->sendImsSmsResponse(info, res);
        else more ? respond()->sendSmsExpectMoreResponse(info, res) : respond()->sendSmsResponse(info, res);
    });
}

ScopedAStatus A6lRadioMessaging::sendSms(int32_t serial, const aidlMsg::GsmSmsMessage& message) {
    send(serial, message, false);
    return ok();
}

ScopedAStatus A6lRadioMessaging::sendSmsExpectMore(int32_t serial, const aidlMsg::GsmSmsMessage& message) {
    send(serial, message, true);
    return ok();
}

// volte3: with IMS registered (modem-centric IMS, no ImsService) GsmSMSDispatcher calls sendImsGsmSms. The modem picks
// the SMS domain itself (MCFG sms_domain_pref = SMS over IMS preferred), so the same WMS raw send is used.
ScopedAStatus A6lRadioMessaging::sendImsSms(int32_t serial, const aidlMsg::ImsSmsMessage& message) {
    if (message.tech != ::aidl::android::hardware::radio::RadioTechnologyFamily::THREE_GPP ||
        message.gsmMessage.empty()) {
        LOG(WARNING) << "[" << serial << "] Messaging.sendImsSms: only 3GPP messages are supported";
        respond()->sendImsSmsResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS), {});
        return ok();
    }
    LOG(INFO) << "[" << serial << "] Messaging.sendImsSms retry=" << message.retry << " (sent via WMS; modem picks IMS/CS)";
    send(serial, message.gsmMessage[0], false, true);
    return ok();
}

ScopedAStatus A6lRadioMessaging::acknowledgeLastIncomingGsmSms(int32_t serial, bool success,
                                                               aidlMsg::SmsAcknowledgeFailCause cause) {
    uint8_t rp = 0x6F, tp = 0xFF;  // protocol error unspecified / unspecified TP error
    if (!success && cause == aidlMsg::SmsAcknowledgeFailCause::MEMORY_CAPACITY_EXCEEDED) {
        rp = 0x16;  // memory capacity exceeded
        tp = 0xD3;
    }
    mExec.post([this, serial, success, rp, tp] {
        bool okAck = slotCore().ackLastSms(success, rp, tp);
        respond()->acknowledgeLastIncomingGsmSmsResponse(okAck ? noError(serial)
                                                               : errorResponse(serial, RadioError::NO_SMS_TO_ACK));
    });
    return ok();
}

ScopedAStatus A6lRadioMessaging::getSmscAddress(int32_t serial) {
    if (!slotCore().bound()) {  // r5 bug hunt round2 R3: never SIM 1's SMSC for an unbound slot 2
        respond()->getSmscAddressResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), "");
        return ok();
    }
    std::string number, type;
    auto r = wms::getSmscAddress(slotCore().ctl(), &number, &type);
    std::string out;
    if (r.ok()) {
        // +CSCA style, as rild reference implementations return it: "\"+33609001390\",145"
        std::string t;
        for (char c : type)
            if (c >= '0' && c <= '9') t += c;
        out = "\"" + number + "\"," + (t.empty() ? std::string(number.rfind('+', 0) == 0 ? "145" : "129") : t);
    }
    respond()->getSmscAddressResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)), out);
    return ok();
}

ScopedAStatus A6lRadioMessaging::reportSmsMemoryStatus(int32_t serial, bool available) {
    LOG(INFO) << "[" << serial << "] Messaging.reportSmsMemoryStatus " << available << " (not forwarded)";
    respond()->reportSmsMemoryStatusResponse(noError(serial));
    return ok();
}

// r5 review F12 (28 Sep 2026): cell broadcast (incl. public warning) is a required capability, so activation and the
// channel configuration are programmed in the modem (WMS 0x003C / 0x003D) and its answer is returned as is.
ScopedAStatus A6lRadioMessaging::setGsmBroadcastActivation(int32_t serial, bool activate) {
    mExec.post([this, serial, activate] {
        auto& core = slotCore();
        if (!core.ready() || !core.bound()) {
            respond()->setGsmBroadcastActivationResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        auto r = wms::setBroadcastActivation(core.ctl(), activate);
        LOG(INFO) << "[" << serial << "] Messaging.setGsmBroadcastActivation " << activate << ": " << r.describe();
        respond()->setGsmBroadcastActivationResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioMessaging::setGsmBroadcastConfig(
        int32_t serial, const std::vector<aidlMsg::GsmBroadcastSmsConfigInfo>& configInfo) {
    std::vector<wms::BroadcastRange> in;
    for (auto& c : configInfo) {
        // fromCodeScheme/toCodeScheme (DCS) cannot be expressed in the WMS 3GPP config: the modem filters on the
        // message identifier (service id) only, Android's CellBroadcastService filters languages itself.
        if (c.fromServiceId < 0 || c.toServiceId > 0xFFFF || c.fromServiceId > c.toServiceId) {
            respond()->setGsmBroadcastConfigResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
            return ok();
        }
        in.push_back({static_cast<uint16_t>(c.fromServiceId), static_cast<uint16_t>(c.toServiceId), c.selected});
    }
    std::vector<wms::BroadcastRange> ranges;
    if (!wms::normalizeBroadcastRanges(in, &ranges)) {
        LOG(WARNING) << "[" << serial << "] Messaging.setGsmBroadcastConfig: " << ranges.size() << " ranges > "
                     << wms::kMaxBroadcastRanges;
        respond()->setGsmBroadcastConfigResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    mExec.post([this, serial, ranges, configInfo] {
        auto& core = slotCore();
        if (!core.ready() || !core.bound()) {
            respond()->setGsmBroadcastConfigResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        auto r = wms::setBroadcastConfig(core.ctl(), ranges);
        LOG(INFO) << "[" << serial << "] Messaging.setGsmBroadcastConfig " << configInfo.size() << " -> "
                  << ranges.size() << " ranges: " << r.describe();
        if (r.ok()) {
            std::lock_guard<std::mutex> g(mCbLock);
            mCbConfig = configInfo;
        }
        respond()->setGsmBroadcastConfigResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioMessaging::getGsmBroadcastConfig(int32_t serial) {
    std::vector<aidlMsg::GsmBroadcastSmsConfigInfo> cfg;
    {
        std::lock_guard<std::mutex> g(mCbLock);
        cfg = mCbConfig;  // what the modem accepted last (it is reprogrammed by the framework after a restart)
    }
    respond()->getGsmBroadcastConfigResponse(noError(serial), cfg);
    return ok();
}

void A6lRadioMessaging::onNewSms(const std::vector<uint8_t>& pdu, bool statusReport) {
    LOG(INFO) << "new " << (statusReport ? "SMS status report" : "SMS") << " (" << pdu.size() << " bytes)";
    if (statusReport)
        indicate()->newSmsStatusReport(RadioIndicationType::UNSOLICITED_ACK_EXP, pdu);
    else
        indicate()->newSms(RadioIndicationType::UNSOLICITED_ACK_EXP, pdu);
}

// r5 deep F25 (28 Sep 2026): cell broadcast pages / ETWS notifications (ModemCore: WMS format 7 or TLV 0x13).
// Language/channel filtering and multipart assembly are done by Android's CellBroadcastService.
void A6lRadioMessaging::onNewBroadcastSms(const std::vector<uint8_t>& data) {
    LOG(INFO) << "new cell broadcast (" << data.size() << " bytes)";
    indicate()->newBroadcastSms(RadioIndicationType::UNSOLICITED, data);
}

void A6lRadioMessaging::onUpdatedResponseFunctions() { slotCore().smsClientReconnected(); }

// ======================================================================== voice
namespace {
int32_t callState(uint8_t s) {
    switch (s) {
        case qv::kStateConversation: return aidlVoice::Call::STATE_ACTIVE;
        case qv::kStateHold: return aidlVoice::Call::STATE_HOLDING;
        case qv::kStateAlerting: return aidlVoice::Call::STATE_ALERTING;
        case qv::kStateIncoming: return aidlVoice::Call::STATE_INCOMING;
        case qv::kStateWaiting: return aidlVoice::Call::STATE_WAITING;
        case qv::kStateOrigination:
        case qv::kStateCcInProgress:
        case qv::kStateSetup: return aidlVoice::Call::STATE_DIALING;
        default: return -1;  // end / disconnecting / unknown: not reported
    }
}
int32_t presentation(uint8_t pi) {
    switch (pi) {
        case 0: return aidlVoice::Call::PRESENTATION_ALLOWED;
        case 1: return aidlVoice::Call::PRESENTATION_RESTRICTED;
        case 4: return aidlVoice::Call::PRESENTATION_PAYPHONE;
        default: return aidlVoice::Call::PRESENTATION_UNKNOWN;
    }
}
}  // namespace

std::optional<uint8_t> A6lRadioVoice::findCall(std::initializer_list<uint8_t> states) {
    for (auto& c : slotCore().calls(true))
        for (auto s : states)
            if (c.state == s) return c.id;
    return std::nullopt;
}

// telephony-flows (29 Sep 2026): emergency numbers "from modem config" (flows::emergencyNumbers: 112/911 always, the
// 3GPP no-SIM list without a card, FR 15/17/18/196 with categories when camped on MCC 208); the framework merges them
// with its ECC database and EF_ECC. Re-sent when SIM presence or the serving MCC changes the list, and to every new
// client. Nothing here dials: emergencyDial below is the only emergency path.
void A6lRadioVoice::publishEmergencyNumbers(bool force) {
    auto& core = slotCore();
    bool simAbsent = true;
    std::string mcc;
    if (core.ready()) {
        auto sim = core.slotSim();
        simAbsent = !sim.valid || !sim.cardInfo || sim.cardInfo->state != qmi::uim::kCardPresent;
        if (auto s = core.serving(); s && s->hasPlmn) mcc = s->mccStr();
    }
    auto want = flows::emergencyNumbers(simAbsent, mcc);
    {
        std::lock_guard<std::mutex> g(mEccLock);
        if (!force && mEccSent && *mEccSent == want) return;
        mEccSent = want;
    }
    std::vector<aidlVoice::EmergencyNumber> list;
    for (auto& e : want) {
        list.push_back({.number = e.number, .mcc = e.mcc, .mnc = "", .categories = e.categories, .urns = {},
                        .sources = aidlVoice::EmergencyNumber::SOURCE_MODEM_CONFIG});
    }
    LOG(INFO) << "emergency numbers (modem config): " << list.size() << " entries, sim_absent=" << simAbsent
              << " mcc=" << (mcc.empty() ? "?" : mcc);
    indicate()->currentEmergencyNumberList(RadioIndicationType::UNSOLICITED, list);
}

void A6lRadioVoice::onUpdatedResponseFunctions() { publishEmergencyNumbers(true); }
void A6lRadioVoice::onSimChanged() { publishEmergencyNumbers(false); }
void A6lRadioVoice::onNetworkChanged() { publishEmergencyNumbers(false); }

ScopedAStatus A6lRadioVoice::getCurrentCalls(int32_t serial) {
    auto& core = slotCore();
    if (!core.ready()) {
        respond()->getCurrentCallsResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    std::vector<aidlVoice::Call> out;
    for (auto& c : core.calls(true)) {
        int32_t st = callState(c.state);
        if (st < 0) continue;
        aidlVoice::Call call{};
        call.state = st;
        call.index = c.id;
        call.toa = (!c.number.empty() && c.number[0] == '+') ? 145 : 129;
        call.isMpty = c.multiparty;
        call.isMT = c.direction == qv::kDirMt;
        call.als = 0;
        call.isVoice = true;
        call.isVoicePrivacy = false;
        call.number = c.number;
        call.numberPresentation = c.hasNumber ? presentation(c.presentation) : aidlVoice::Call::PRESENTATION_UNKNOWN;
        call.name = "";
        call.namePresentation = aidlVoice::Call::PRESENTATION_UNKNOWN;
        out.push_back(call);
    }
    respond()->getCurrentCallsResponse(noError(serial), out);
    return ok();
}

// r5 deep review F24 (28 Sep 2026): the per-call CLIR choice is carried to the QMI dial (TLV 0x11); an unknown value
// is refused instead of silently using the subscription default.
ScopedAStatus A6lRadioVoice::dial(int32_t serial, const aidlVoice::Dial& d) {
    LOG(INFO) << "[" << serial << "] Voice.dial (number not logged) clir=" << d.clir;
    std::optional<uint8_t> clir;
    if (!qv::clirFromAndroid(d.clir, &clir)) {
        respond()->dialResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    mExec.post([this, serial, req = qv::DialRequest{d.address, false, clir, std::nullopt}] {
        uint8_t id = 0;
        if (!slotCore().bound()) {  // unbound slot 2 must never dial on SIM 1
            respond()->dialResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        auto r = qv::dial(slotCore().ctl(), req, &id);
        LOG(INFO) << "[" << serial << "] dial slot" << slotCore().sub() + 1 << " -> " << r.describe() << " call id "
                  << int(id) << " rat_before=" << (slotCore().serving() ? qmi::nas::radioIfList(*slotCore().serving()) : "?");
        respond()->dialResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

// r5 deep review F23 (28 Sep 2026): routing, categories, user intent and isTesting reach the modem request
// (qv::emergencyDial). A test request is never sent as an emergency call; fallbacks only after a definitive refusal.
ScopedAStatus A6lRadioVoice::emergencyDial(int32_t serial, const aidlVoice::Dial& d, int32_t categories,
                                           const std::vector<std::string>& urns,
                                           aidlVoice::EmergencyCallRouting routing,
                                           bool hasKnownUserIntentEmergency, bool isTesting) {
    LOG(WARNING) << "[" << serial << "] Voice.emergencyDial categories=" << categories << " urns=" << urns.size()
                 << " routing=" << toString(routing) << " intent=" << hasKnownUserIntentEmergency
                 << " testing=" << isTesting;  // URNs: no CS/QMI VOICE encoding (optional per IRadioVoice)
    qv::EmergencyDialRequest req{d.address, d.clir, categories, static_cast<qv::EmergencyRouting>(routing),
                                 hasKnownUserIntentEmergency, isTesting};
    mExec.post([this, serial, req] {
        uint8_t id = 0;
        // telephony-flows: no SIM / PIN-locked SIM / limited service are NOT refused here (the modem places the
        // emergency call in limited service); only a modem that is not up at all is RADIO_NOT_AVAILABLE, so
        // Telephony retries after radio power / on the other slot.
        if (!slotCore().ready()) {
            LOG(WARNING) << "[" << serial << "] emergencyDial: modem not ready";
            respond()->emergencyDialResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        auto o = qv::emergencyDial(slotCore().ctl(), req, &id);
        LOG(WARNING) << "[" << serial << "] emergencyDial -> " << o.result.describe() << " attempts=" << o.attempts.size()
                     << (o.refusedTestToEmergencyNumber ? " (test call to an emergency number refused)" : "");
        RadioError err = o.refusedTestToEmergencyNumber ? RadioError::INVALID_ARGUMENTS : toRadioError(o.result);
        respond()->emergencyDialResponse(o.result.ok() ? noError(serial) : errorResponse(serial, err));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::hangup(int32_t serial, int32_t gsmIndex) {
    mExec.post([this, serial, gsmIndex] {
        auto r = qv::endCall(slotCore().ctl(), static_cast<uint8_t>(gsmIndex));
        respond()->hangupConnectionResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

#define A6L_SUPS(method, response, sups)                                                            \
    ScopedAStatus A6lRadioVoice::method(int32_t serial) {                                           \
        mExec.post([this, serial] {                                                                 \
            auto r = qv::manageCalls(slotCore().ctl(), sups);                                 \
            LOG(INFO) << "[" << serial << "] Voice." #method " -> " << r.describe();               \
            respond()->response(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r))); \
        });                                                                                         \
        return ok();                                                                                \
    }
A6L_SUPS(hangupWaitingOrBackground, hangupWaitingOrBackgroundResponse, qv::kSupsReleaseHeldOrWaiting)
A6L_SUPS(hangupForegroundResumeBackground, hangupForegroundResumeBackgroundResponse,
         qv::kSupsReleaseActiveAcceptHeldOrWaiting)
A6L_SUPS(switchWaitingOrHoldingAndActive, switchWaitingOrHoldingAndActiveResponse,
         qv::kSupsHoldActiveAcceptWaitingOrHeld)
A6L_SUPS(conference, conferenceResponse, qv::kSupsMakeConference)
A6L_SUPS(explicitCallTransfer, explicitCallTransferResponse, qv::kSupsExplicitCallTransfer)
#undef A6L_SUPS

ScopedAStatus A6lRadioVoice::separateConnection(int32_t serial, int32_t gsmIndex) {
    mExec.post([this, serial, gsmIndex] {
        auto r = qv::manageCalls(slotCore().ctl(), qv::kSupsHoldAllExceptSpecified,
                                 static_cast<uint8_t>(gsmIndex));
        respond()->separateConnectionResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::acceptCall(int32_t serial) {
    mExec.post([this, serial] {
        qmi::Result r;
        if (auto id = findCall({qv::kStateIncoming})) {
            r = qv::answer(slotCore().ctl(), *id);
        } else if (findCall({qv::kStateWaiting})) {
            r = qv::manageCalls(slotCore().ctl(), qv::kSupsHoldActiveAcceptWaitingOrHeld);
        } else {
            respond()->acceptCallResponse(errorResponse(serial, RadioError::INVALID_STATE));
            return;
        }
        LOG(INFO) << "[" << serial << "] Voice.acceptCall -> " << r.describe();
        respond()->acceptCallResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

// r5 review round7 F57 (28 Sep 2026): UDUB applies to the ringing or waiting call only (qv::rejectRingingOrWaiting):
// INCOMING -> End Call(id), WAITING -> Manage Calls CHLD=0 targeted with the waiting call id. With only held/active
// calls (e.g. the waiting caller hung up while this request was queued) nothing is sent: INVALID_STATE. A failed call
// list query sends nothing either (no untargeted release).
ScopedAStatus A6lRadioVoice::rejectCall(int32_t serial) {
    mExec.post([this, serial] {
        if (!slotCore().bound()) {
            respond()->rejectCallResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        auto o = qv::rejectRingingOrWaiting(slotCore().ctl());
        LOG(INFO) << "[" << serial << "] Voice.rejectCall kind=" << o.kind << " id=" << int(o.callId) << " -> "
                  << o.result.describe();
        RadioError err = RadioError::NONE;
        if (o.kind == qv::RejectOutcome::NoTarget) err = RadioError::INVALID_STATE;
        else if (!o.result.ok()) err = toRadioError(o.result);
        respond()->rejectCallResponse(err == RadioError::NONE ? noError(serial) : errorResponse(serial, err));
    });
    return ok();
}

// r5 review round7 F56 (28 Sep 2026): one of the 12 DTMF keys (IRadioVoice.sendDtmf), START + 150 ms + STOP where both
// results count (qv::finiteDtmf): a refused/timed-out STOP is retried (bounded) while the call is live; success only
// when the tone was started and stopped. The call ending meanwhile (tone gone with it) -> INVALID_CALL_ID.
ScopedAStatus A6lRadioVoice::sendDtmf(int32_t serial, const std::string& s) {
    if (s.size() != 1 || !qv::isDtmfDigit(s[0])) {
        respond()->sendDtmfResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    mExec.post([this, serial, digit = s[0]] {
        uint8_t id = findCall({qv::kStateConversation}).value_or(0xFF);
        auto o = qv::finiteDtmf(slotCore().ctl(), id, digit);
        RadioError err = RadioError::NONE;
        if (!o.start.ok()) err = toRadioError(o.start);
        else if (o.callGone) err = RadioError::INVALID_CALL_ID;
        else if (!o.stop.ok()) err = toRadioError(o.stop);
        if (err != RadioError::NONE)
            LOG(WARNING) << "[" << serial << "] Voice.sendDtmf start=" << o.start.describe()
                         << " stop=" << o.stop.describe() << " attempts=" << o.stopAttempts << " callGone=" << o.callGone;
        respond()->sendDtmfResponse(err == RadioError::NONE ? noError(serial) : errorResponse(serial, err));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::startDtmf(int32_t serial, const std::string& s) {
    mExec.post([this, serial, s] {
        uint8_t id = findCall({qv::kStateConversation}).value_or(0xFF);
        auto r = s.empty() ? qmi::Result{} : qv::startDtmf(slotCore().ctl(), id, s[0]);
        respond()->startDtmfResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::stopDtmf(int32_t serial) {
    mExec.post([this, serial] {
        uint8_t id = findCall({qv::kStateConversation}).value_or(0xFF);
        auto r = qv::stopDtmf(slotCore().ctl(), id);
        respond()->stopDtmfResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

// r5 review round7 F58 (28 Sep 2026): the cause ModemCore recorded for the most recently terminated call (QMI call end
// reason mapped explicitly, qv::lastCallFailCauseFromQmi; VOICE/modem loss -> RADIO_INTERNAL_ERROR; unknown ->
// ERROR_UNSPECIFIED), with the raw QMI reason in vendorCause. Never a constant NORMAL.
ScopedAStatus A6lRadioVoice::getLastCallFailCause(int32_t serial) {
    auto f = slotCore().lastCallFail();
    aidlVoice::LastCallFailCauseInfo info{.causeCode = static_cast<aidlVoice::LastCallFailCause>(f.cause),
                                          .vendorCause = f.vendor};
    respond()->getLastCallFailCauseResponse(noError(serial), info);
    return ok();
}

// r5 review F6 (28 Sep 2026): real uplink mute. a6l-q6voiced writes "VoiceMMode1 TX Mute" (ADSP CVP mute, applied
// live during a call, restored at every call start); the audio HAL's setMicMute uses the same daemon. Errors are real:
// no tx-mute kernel control -> REQUEST_NOT_SUPPORTED, DSP refusal/timeout -> MODEM_ERR, daemon absent -> SYSTEM_ERR.
namespace {
RadioError muteError(const VoiceMuteReply& r) {
    switch (r.kind) {
        case VoiceMuteReply::Ok: return RadioError::NONE;
        case VoiceMuteReply::Unsupported: return RadioError::REQUEST_NOT_SUPPORTED;
        case VoiceMuteReply::DspError: return RadioError::MODEM_ERR;
        case VoiceMuteReply::BadRequest: return RadioError::INVALID_ARGUMENTS;
        case VoiceMuteReply::NoDaemon: return RadioError::SYSTEM_ERR;
        case VoiceMuteReply::Protocol: return RadioError::INTERNAL_ERR;
    }
    return RadioError::INTERNAL_ERR;
}
}  // namespace

ScopedAStatus A6lRadioVoice::getMute(int32_t serial) {
    mExec.post([this, serial] {
        auto r = getVoiceTxMute();
        if (r.ok()) mMute = r.mute;
        else LOG(WARNING) << "[" << serial << "] Voice.getMute: " << r.text << " (last applied " << mMute << ")";
        respond()->getMuteResponse(r.ok() ? noError(serial) : errorResponse(serial, muteError(r)), mMute);
    });
    return ok();
}
ScopedAStatus A6lRadioVoice::setMute(int32_t serial, bool enable) {
    mExec.post([this, serial, enable] {
        auto r = setVoiceTxMute(enable);
        LOG(INFO) << "[" << serial << "] Voice.setMute " << enable << ": " << r.text;
        if (r.ok()) mMute = r.mute;
        respond()->setMuteResponse(r.ok() ? noError(serial) : errorResponse(serial, muteError(r)));
    });
    return ok();
}
// r5 review round7 F55 (28 Sep 2026): no TTY path exists here (no QMI VOICE TTY configuration is sent and the ADSP
// voice path has no TTY mode wired), so only OFF is acknowledged; FULL/HCO/VCO -> REQUEST_NOT_SUPPORTED and the
// readback stays OFF (what is applied). A future implementation must convert Android OFF/FULL/HCO/VCO (0..3) to QMI
// FULL/VCO/HCO/OFF (0..3) explicitly, never by a cast.
ScopedAStatus A6lRadioVoice::getTtyMode(int32_t serial) {
    respond()->getTtyModeResponse(noError(serial), mTty);
    return ok();
}
ScopedAStatus A6lRadioVoice::setTtyMode(int32_t serial, aidlVoice::TtyMode mode) {
    if (mode != aidlVoice::TtyMode::OFF) {
        LOG(WARNING) << "[" << serial << "] Voice.setTtyMode " << toString(mode) << ": REQUEST_NOT_SUPPORTED";
        respond()->setTtyModeResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED));
        return ok();
    }
    mTty = aidlVoice::TtyMode::OFF;
    respond()->setTtyModeResponse(noError(serial));
    return ok();
}
ScopedAStatus A6lRadioVoice::getPreferredVoicePrivacy(int32_t serial) {
    respond()->getPreferredVoicePrivacyResponse(noError(serial), mPrivacy);
    return ok();
}
ScopedAStatus A6lRadioVoice::setPreferredVoicePrivacy(int32_t serial, bool enable) {
    mPrivacy = enable;
    respond()->setPreferredVoicePrivacyResponse(noError(serial));
    return ok();
}
ScopedAStatus A6lRadioVoice::isVoNrEnabled(int32_t serial) {
    respond()->isVoNrEnabledResponse(noError(serial), false);
    return ok();
}
// telephony-flows (29 Sep 2026): GSM/UMTS/LTE CS emergency calls have no modem emergency callback mode (ECBM is a
// CDMA / IMS concept): enterEmergencyCallbackMode is never indicated, so the framework only calls this defensively;
// nothing to exit -> NONE. Documented in docs/telephony-flows-20260929.md.
ScopedAStatus A6lRadioVoice::exitEmergencyCallbackMode(int32_t serial) {
    respond()->exitEmergencyCallbackModeResponse(noError(serial));
    return ok();
}

// ------------------------------------------------------------------ USSD (telephony-flows, 29 Sep 2026)
// sendUssd: idle -> QMI Originate USSD No Wait (the reply comes as the 0x0043 indication -> onUssd NOTIFY); the network
// asked for an answer (USSD indication, user action REQUIRED -> onUssd REQUEST) -> Answer USSD. A modem without No Wait
// (INVALID_QMI_COMMAND / NOT_SUPPORTED) gets the sync Originate USSD (up to 100 s) on its own executor. USSD text is
// never logged. Unbound slot 2 never uses SIM 1's VOICE (R3 rule).
void A6lRadioVoice::reportUssd(const flows::UssdReport& r) {
    LOG(INFO) << "Voice.onUssd mode=" << r.mode << " len=" << r.msg.size();
    indicate()->onUssd(RadioIndicationType::UNSOLICITED, static_cast<aidlVoice::UssdModeType>(r.mode), r.msg);
}

ScopedAStatus A6lRadioVoice::sendUssd(int32_t serial, const std::string& ussd) {
    auto enc = qv::ussd::encode(ussd);
    if (!enc) {
        LOG(WARNING) << "[" << serial << "] Voice.sendUssd: not encodable (len " << ussd.size() << ")";
        respond()->sendUssdResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    mUssdExec.post([this, serial, e = *enc] {
        auto& core = slotCore();
        if (!core.bound() || !core.ready()) {
            respond()->sendUssdResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        const auto how = mUssd.nextSend();
        mUssd.sent(how);  // before the request: a fast network reply (indication) must find the dialogue open
        if (how == flows::UssdSession::Send::Answer) {
            auto r = qv::ussd::answer(core.ctl(), e);
            LOG(INFO) << "[" << serial << "] Voice.sendUssd answer dcs=" << int(e.dcs) << " -> " << r.describe();
            if (!r.ok()) mUssd.sendFailed(how);
            respond()->sendUssdResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
            return;
        }
        auto r = qv::ussd::originateNoWait(core.ctl(), e);
        LOG(INFO) << "[" << serial << "] Voice.sendUssd originate(no-wait) dcs=" << int(e.dcs) << " -> " << r.describe();
        if (r.ok()) {
            respond()->sendUssdResponse(noError(serial));
            return;
        }
        if (!qv::ussd::isUnsupported(r)) {
            mUssd.sendFailed(how);
            respond()->sendUssdResponse(errorResponse(serial, toRadioError(r)));
            return;
        }
        auto rs = qv::ussd::originate(core.ctl(), e);
        LOG(INFO) << "[" << serial << "] Voice.sendUssd originate(sync) -> " << rs.describe();
        if (!rs.ok() && rs.status != qmi::Result::QmiFailure) {  // timeout / transport: dialogue state unknown
            mUssd.sendFailed(how);
            respond()->sendUssdResponse(errorResponse(serial, toRadioError(rs)));
            return;
        }
        respond()->sendUssdResponse(rs.ok() ? noError(serial) : errorResponse(serial, toRadioError(rs)));
        if (rs.ok()) {
            if (auto rep = mUssd.onEvent(qv::ussd::fromOriginateResponse(rs))) reportUssd(*rep);
        } else {
            mUssd.sendFailed(how);
        }
    });
    return ok();
}

// Cancel runs on the call executor, not behind a (possibly blocked) sync originate. A modem answering "no effect"
// (no dialogue any more) counts as cancelled.
ScopedAStatus A6lRadioVoice::cancelPendingUssd(int32_t serial) {
    mExec.post([this, serial] {
        auto& core = slotCore();
        if (!core.bound() || !core.ready()) {
            mUssd.cancelled();
            respond()->cancelPendingUssdResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
            return;
        }
        auto r = qv::ussd::cancel(core.ctl());
        bool okish = r.ok() || (r.status == qmi::Result::QmiFailure && r.qmiError == qmi::kErrNoEffect);
        LOG(INFO) << "[" << serial << "] Voice.cancelPendingUssd -> " << r.describe();
        if (okish) mUssd.cancelled();
        respond()->cancelPendingUssdResponse(okish ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

void A6lRadioVoice::onUssd(const qv::ussd::Event& e) {
    if (auto rep = mUssd.onEvent(e)) reportUssd(*rep);
}

void A6lRadioVoice::onModemLost() {
    indicate()->callStateChanged(RadioIndicationType::UNSOLICITED);
    if (auto rep = mUssd.modemLost()) reportUssd(*rep);
}

void A6lRadioVoice::onCallsChanged(bool incomingRinging) {
    indicate()->callStateChanged(RadioIndicationType::UNSOLICITED);
    if (incomingRinging) indicate()->callRing(RadioIndicationType::UNSOLICITED, true, {});
}

}  // namespace android::hardware::radio::a6l
