// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): IRadioMessaging (GSM/WCDMA/LTE SMS over WMS), IRadioVoice
// (circuit-switched calls over VOICE). No call audio yet: the mainline kernel has no q6voice/CVD
// path for SDM660, so calls connect but are silent until that exists.
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"

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
void A6lRadioMessaging::send(int32_t serial, const aidlMsg::GsmSmsMessage& m, bool more) {
    auto smsc = m.smscPdu.empty() ? std::optional<std::vector<uint8_t>>(std::vector<uint8_t>{0x00})
                                  : qmi::unhex(m.smscPdu);
    auto tpdu = qmi::unhex(m.pdu);
    if (!smsc || !tpdu || tpdu->empty()) {
        auto info = errorResponse(serial, RadioError::INVALID_SMS_FORMAT);
        more ? respond()->sendSmsExpectMoreResponse(info, {}) : respond()->sendSmsResponse(info, {});
        return;
    }
    std::vector<uint8_t> raw = *smsc;
    raw.insert(raw.end(), tpdu->begin(), tpdu->end());
    mExec.post([this, serial, raw, more] {
        auto& core = ModemCore::get();
        aidlMsg::SendSmsResult res{.messageRef = 0, .ackPDU = "", .errorCode = -1};
        RadioError err = RadioError::NONE;
        if (!core.ready()) {
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
        more ? respond()->sendSmsExpectMoreResponse(info, res) : respond()->sendSmsResponse(info, res);
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

ScopedAStatus A6lRadioMessaging::acknowledgeLastIncomingGsmSms(int32_t serial, bool success,
                                                               aidlMsg::SmsAcknowledgeFailCause cause) {
    uint8_t rp = 0x6F, tp = 0xFF;  // protocol error unspecified / unspecified TP error
    if (!success && cause == aidlMsg::SmsAcknowledgeFailCause::MEMORY_CAPACITY_EXCEEDED) {
        rp = 0x16;  // memory capacity exceeded
        tp = 0xD3;
    }
    mExec.post([this, serial, success, rp, tp] {
        bool okAck = ModemCore::get().ackLastSms(success, rp, tp);
        respond()->acknowledgeLastIncomingGsmSmsResponse(okAck ? noError(serial)
                                                               : errorResponse(serial, RadioError::NO_SMS_TO_ACK));
    });
    return ok();
}

ScopedAStatus A6lRadioMessaging::getSmscAddress(int32_t serial) {
    std::string number, type;
    auto r = wms::getSmscAddress(ModemCore::get().ctl(), &number, &type);
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

ScopedAStatus A6lRadioMessaging::setGsmBroadcastActivation(int32_t serial, bool activate) {
    auto r = wms::setBroadcastActivation(ModemCore::get().ctl(), activate);
    LOG(INFO) << "[" << serial << "] Messaging.setGsmBroadcastActivation " << activate << ": " << r.describe();
    respond()->setGsmBroadcastActivationResponse(noError(serial));  // best effort (cell broadcast optional)
    return ok();
}

ScopedAStatus A6lRadioMessaging::setGsmBroadcastConfig(
        int32_t serial, const std::vector<aidlMsg::GsmBroadcastSmsConfigInfo>& configInfo) {
    LOG(INFO) << "[" << serial << "] Messaging.setGsmBroadcastConfig " << configInfo.size()
              << " ranges (accepted, not programmed)";
    respond()->setGsmBroadcastConfigResponse(noError(serial));
    return ok();
}

ScopedAStatus A6lRadioMessaging::getGsmBroadcastConfig(int32_t serial) {
    respond()->getGsmBroadcastConfigResponse(noError(serial), {});
    return ok();
}

void A6lRadioMessaging::onNewSms(const std::vector<uint8_t>& pdu, bool statusReport) {
    LOG(INFO) << "new " << (statusReport ? "SMS status report" : "SMS") << " (" << pdu.size() << " bytes)";
    if (statusReport)
        indicate()->newSmsStatusReport(RadioIndicationType::UNSOLICITED_ACK_EXP, pdu);
    else
        indicate()->newSms(RadioIndicationType::UNSOLICITED_ACK_EXP, pdu);
}

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
    for (auto& c : ModemCore::get().calls(true))
        for (auto s : states)
            if (c.state == s) return c.id;
    return std::nullopt;
}

void A6lRadioVoice::onUpdatedResponseFunctions() {
    // Basic emergency numbers from "modem config"; the framework merges them with its database.
    std::vector<aidlVoice::EmergencyNumber> list;
    for (const char* n : {"112", "911"}) {
        list.push_back({.number = n, .mcc = "", .mnc = "", .categories = 0, .urns = {},
                        .sources = aidlVoice::EmergencyNumber::SOURCE_MODEM_CONFIG});
    }
    indicate()->currentEmergencyNumberList(RadioIndicationType::UNSOLICITED, list);
}

ScopedAStatus A6lRadioVoice::getCurrentCalls(int32_t serial) {
    auto& core = ModemCore::get();
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

ScopedAStatus A6lRadioVoice::dial(int32_t serial, const aidlVoice::Dial& d) {
    LOG(INFO) << "[" << serial << "] Voice.dial (number not logged) clir=" << d.clir;
    mExec.post([this, serial, number = d.address] {
        uint8_t id = 0;
        auto r = qv::dial(ModemCore::get().ctl(), number, false, &id);
        LOG(INFO) << "[" << serial << "] dial -> " << r.describe() << " call id " << int(id);
        respond()->dialResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::emergencyDial(int32_t serial, const aidlVoice::Dial& d, int32_t categories,
                                           const std::vector<std::string>& urns,
                                           aidlVoice::EmergencyCallRouting routing,
                                           bool hasKnownUserIntentEmergency, bool isTesting) {
    (void)urns;
    LOG(WARNING) << "[" << serial << "] Voice.emergencyDial " << d.address << " categories=" << categories
                 << " routing=" << toString(routing) << " intent=" << hasKnownUserIntentEmergency
                 << " testing=" << isTesting;
    mExec.post([this, serial, number = d.address] {
        uint8_t id = 0;
        auto r = qv::dial(ModemCore::get().ctl(), number, true, &id);
        if (!r.ok()) r = qv::dial(ModemCore::get().ctl(), number, false, &id);  // older VOICE: no call type TLV
        respond()->emergencyDialResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::hangup(int32_t serial, int32_t gsmIndex) {
    mExec.post([this, serial, gsmIndex] {
        auto r = qv::endCall(ModemCore::get().ctl(), static_cast<uint8_t>(gsmIndex));
        respond()->hangupConnectionResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

#define A6L_SUPS(method, response, sups)                                                            \
    ScopedAStatus A6lRadioVoice::method(int32_t serial) {                                           \
        mExec.post([this, serial] {                                                                 \
            auto r = qv::manageCalls(ModemCore::get().ctl(), sups);                                 \
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
        auto r = qv::manageCalls(ModemCore::get().ctl(), qv::kSupsHoldAllExceptSpecified,
                                 static_cast<uint8_t>(gsmIndex));
        respond()->separateConnectionResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::acceptCall(int32_t serial) {
    mExec.post([this, serial] {
        qmi::Result r;
        if (auto id = findCall({qv::kStateIncoming})) {
            r = qv::answer(ModemCore::get().ctl(), *id);
        } else if (findCall({qv::kStateWaiting})) {
            r = qv::manageCalls(ModemCore::get().ctl(), qv::kSupsHoldActiveAcceptWaitingOrHeld);
        } else {
            respond()->acceptCallResponse(errorResponse(serial, RadioError::INVALID_STATE));
            return;
        }
        LOG(INFO) << "[" << serial << "] Voice.acceptCall -> " << r.describe();
        respond()->acceptCallResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::rejectCall(int32_t serial) {
    mExec.post([this, serial] {
        qmi::Result r;
        if (auto id = findCall({qv::kStateIncoming})) {
            r = qv::endCall(ModemCore::get().ctl(), *id);
        } else {
            r = qv::manageCalls(ModemCore::get().ctl(), qv::kSupsReleaseHeldOrWaiting);
        }
        respond()->rejectCallResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::sendDtmf(int32_t serial, const std::string& s) {
    mExec.post([this, serial, s] {
        uint8_t id = findCall({qv::kStateConversation}).value_or(0xFF);
        qmi::Result r;
        for (char c : s) {
            r = qv::startDtmf(ModemCore::get().ctl(), id, c);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            qv::stopDtmf(ModemCore::get().ctl(), id);
            if (!r.ok()) break;
        }
        respond()->sendDtmfResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::startDtmf(int32_t serial, const std::string& s) {
    mExec.post([this, serial, s] {
        uint8_t id = findCall({qv::kStateConversation}).value_or(0xFF);
        auto r = s.empty() ? qmi::Result{} : qv::startDtmf(ModemCore::get().ctl(), id, s[0]);
        respond()->startDtmfResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::stopDtmf(int32_t serial) {
    mExec.post([this, serial] {
        uint8_t id = findCall({qv::kStateConversation}).value_or(0xFF);
        auto r = qv::stopDtmf(ModemCore::get().ctl(), id);
        respond()->stopDtmfResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    });
    return ok();
}

ScopedAStatus A6lRadioVoice::getLastCallFailCause(int32_t serial) {
    aidlVoice::LastCallFailCauseInfo info{
            .causeCode = static_cast<aidlVoice::LastCallFailCause>(ModemCore::get().lastCallFailCause()),
            .vendorCause = ""};
    respond()->getLastCallFailCauseResponse(noError(serial), info);
    return ok();
}

ScopedAStatus A6lRadioVoice::getMute(int32_t serial) {
    respond()->getMuteResponse(noError(serial), mMute);
    return ok();
}
ScopedAStatus A6lRadioVoice::setMute(int32_t serial, bool enable) {
    mMute = enable;  // no uplink audio path yet (q6voice missing)
    respond()->setMuteResponse(noError(serial));
    return ok();
}
ScopedAStatus A6lRadioVoice::getTtyMode(int32_t serial) {
    respond()->getTtyModeResponse(noError(serial), mTty);
    return ok();
}
ScopedAStatus A6lRadioVoice::setTtyMode(int32_t serial, aidlVoice::TtyMode mode) {
    mTty = mode;
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
ScopedAStatus A6lRadioVoice::exitEmergencyCallbackMode(int32_t serial) {
    respond()->exitEmergencyCallbackModeResponse(noError(serial));
    return ok();
}

void A6lRadioVoice::onModemLost() { indicate()->callStateChanged(RadioIndicationType::UNSOLICITED); }

void A6lRadioVoice::onCallsChanged(bool incomingRinging) {
    indicate()->callStateChanged(RadioIndicationType::UNSOLICITED);
    if (incomingRinging) indicate()->callRing(RadioIndicationType::UNSOLICITED, true, {});
}

}  // namespace android::hardware::radio::a6l
