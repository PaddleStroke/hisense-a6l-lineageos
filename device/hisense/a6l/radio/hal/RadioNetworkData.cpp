// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): IRadioNetwork, IRadioData.
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"
#include "DataPolicy.h"
#include "SimNetPolicy.h"  // r5 round5 F45/F46
#include "TimePolicy.h"    // r5 round8 F61

#include <a6lqmi/message.h>
#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/RadioAccessFamily.h>
#include <aidl/android/hardware/radio/RadioConst.h>
#include <aidl/android/hardware/radio/RadioTechnology.h>
#include <aidl/android/hardware/radio/RadioTechnologyFamily.h>  // volte2
#include <aidl/android/hardware/radio/data/ApnTypes.h>  // r5 F9
#include <libminradio/network/structs.h>
#include <libminradio/response.h>
#include <utils/SystemClock.h>

#include <algorithm>

namespace android::hardware::radio::a6l {

using ::aidl::android::hardware::radio::AccessNetwork;
using ::aidl::android::hardware::radio::RadioAccessFamily;
using ::aidl::android::hardware::radio::RadioConst;
using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::aidl::android::hardware::radio::RadioTechnology;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace qmi = ::a6l::qmi;
namespace nas = ::a6l::qmi::nas;
namespace aidlNet = ::aidl::android::hardware::radio::network;
namespace aidlData = ::aidl::android::hardware::radio::data;
constexpr auto ok = &ScopedAStatus::ok;
constexpr int32_t kUnavail = RadioConst::VALUE_UNAVAILABLE;

namespace {
bool hasCap(const nas::ServingSystem& s, uint8_t c) {
    return std::find(s.dataCaps.begin(), s.dataCaps.end(), c) != s.dataCaps.end();
}
RadioTechnology ratOf(const nas::ServingSystem& s) {
    switch (s.primaryRat()) {
        case nas::kRifLte: return RadioTechnology::LTE;
        case nas::kRifUmts:
            if (hasCap(s, nas::kCapHsdpaPlus) || hasCap(s, nas::kCapDcHsdpaPlus)) return RadioTechnology::HSPAP;
            if (hasCap(s, nas::kCapHsdpa) && hasCap(s, nas::kCapHsupa)) return RadioTechnology::HSPA;
            if (hasCap(s, nas::kCapHsdpa)) return RadioTechnology::HSDPA;
            return RadioTechnology::UMTS;
        case nas::kRifGsm:
            if (hasCap(s, nas::kCapEdge)) return RadioTechnology::EDGE;
            if (hasCap(s, nas::kCapGprs)) return RadioTechnology::GPRS;
            return RadioTechnology::GSM;
        case nas::kRifTdscdma: return RadioTechnology::TD_SCDMA;
        case nas::kRif5gnr: return RadioTechnology::NR;
        default: return RadioTechnology::UNKNOWN;
    }
}
int32_t asuFromRssi(int dbm) {
    if (dbm >= 0 || dbm < -113) return 99;
    return std::clamp((dbm + 113) / 2, 0, 31);
}
}  // namespace

// ======================================================================== network
aidlr::RadioTechnology A6lRadioNetwork::currentRat() {
    auto s = slotCore().serving();
    return s ? ratOf(*s) : RadioTechnology::UNKNOWN;
}

aidlNet::RegStateResult A6lRadioNetwork::buildRegState(bool voice) {
    auto& core = slotCore();
    aidlNet::RegStateResult res{};
    res.regState = aidlNet::RegState::NOT_REG_MT_NOT_SEARCHING_OP;
    res.rat = RadioTechnology::UNKNOWN;
    res.reasonForDenial = aidlNet::RegistrationFailCause::NONE;
    auto s = core.serving();
    if (!s) return res;
    uint8_t attach = voice ? s->csAttach : s->psAttach;
    switch (s->regState) {
        case nas::kRegistered:
            if (attach == nas::kDetached) {
                res.regState = aidlNet::RegState::NOT_REG_MT_NOT_SEARCHING_OP;
            } else {
                res.regState = (s->roaming && *s->roaming) ? aidlNet::RegState::REG_ROAMING
                                                           : aidlNet::RegState::REG_HOME;
            }
            break;
        case nas::kSearching: res.regState = aidlNet::RegState::NOT_REG_MT_SEARCHING_OP; break;
        case nas::kDenied: res.regState = aidlNet::RegState::REG_DENIED; break;
        case nas::kRegUnknown: res.regState = aidlNet::RegState::UNKNOWN; break;
        default: res.regState = aidlNet::RegState::NOT_REG_MT_NOT_SEARCHING_OP; break;
    }
    res.rat = ratOf(*s);
    // telephony-flows (29 Sep 2026): not registered but camped (limited service: no SIM, PIN-locked, SIM rejected)
    // -> the *_EM state, so Android shows "emergency calls only" (flows::emergencyOnlyVariant). Voice only.
    if (voice && s->regState != nas::kRegistered) {
        res.regState = static_cast<aidlNet::RegState>(
                flows::emergencyOnlyVariant(static_cast<int32_t>(res.regState), !s->radioIfs.empty()));
    }
    if (!s->hasPlmn) return res;
    std::string mcc = s->mccStr(), mnc = s->mncStr();
    res.registeredPlmn = mcc + mnc;
    auto on = core.operatorName();
    std::string alphaLong = !on.longName.empty() ? on.longName : !s->description.empty() ? s->description : res.registeredPlmn;
    std::string alphaShort = !on.shortName.empty() ? on.shortName : alphaLong;
    aidlNet::OperatorInfo op{.alphaLong = alphaLong,
                             .alphaShort = alphaShort,
                             .operatorNumeric = res.registeredPlmn,
                             .status = aidlNet::OperatorInfo::STATUS_CURRENT};
    switch (s->primaryRat()) {
        case nas::kRifLte: {
            qmi::nas::LteCell lc;
            nas::getLteCell(core.ctl(), &lc);
            aidlNet::CellIdentityLte id{};
            id.mcc = mcc;
            id.mnc = mnc;
            id.ci = s->cid ? static_cast<int32_t>(*s->cid) : (lc.valid ? static_cast<int32_t>(lc.globalCellId) : kUnavail);
            id.pci = lc.valid ? lc.pci : kUnavail;
            id.tac = s->tac ? *s->tac : (lc.valid ? lc.tac : kUnavail);
            id.earfcn = lc.valid ? lc.earfcn : kUnavail;
            id.operatorNames = op;
            id.bandwidth = kUnavail;
            res.cellIdentity = id;
            res.accessTechnologySpecificInfo = aidlNet::EutranRegistrationInfo{};
            break;
        }
        case nas::kRifUmts: {
            aidlNet::CellIdentityWcdma id{};
            id.mcc = mcc;
            id.mnc = mnc;
            id.lac = s->lac ? *s->lac : kUnavail;
            id.cid = s->cid ? static_cast<int32_t>(*s->cid) : kUnavail;
            id.psc = kUnavail;
            id.uarfcn = kUnavail;
            id.operatorNames = op;
            res.cellIdentity = id;
            break;
        }
        case nas::kRifGsm: {
            aidlNet::CellIdentityGsm id{};
            id.mcc = mcc;
            id.mnc = mnc;
            id.lac = s->lac ? *s->lac : kUnavail;
            id.cid = s->cid ? static_cast<int32_t>(*s->cid) : kUnavail;
            id.arfcn = kUnavail;
            id.bsic = static_cast<int8_t>(0xFF);
            id.operatorNames = op;
            res.cellIdentity = id;
            break;
        }
        default: break;
    }
    return res;
}

aidlNet::SignalStrength A6lRadioNetwork::buildSignal(const nas::SignalInfo& s) {
    auto sig = minimal::structs::makeSignalStrength();
    if (s.gsmRssi) {
        sig.gsm.signalStrength = asuFromRssi(*s.gsmRssi);
        sig.gsm.bitErrorRate = 99;
    }
    if (s.wcdmaRssi) {
        sig.wcdma.signalStrength = asuFromRssi(*s.wcdmaRssi);
        sig.wcdma.bitErrorRate = 99;
        if (s.wcdmaRscp) sig.wcdma.rscp = std::clamp(*s.wcdmaRscp + 120, 0, 96);
        if (s.wcdmaEcio) sig.wcdma.ecno = std::clamp(49 - *s.wcdmaEcio, 0, 49);
    }
    if (s.hasLte) {
        sig.lte.signalStrength = asuFromRssi(s.lteRssi);
        sig.lte.rsrp = (s.lteRsrp <= -44 && s.lteRsrp >= -140) ? -s.lteRsrp : kUnavail;
        sig.lte.rsrq = (s.lteRsrq <= -3 && s.lteRsrq >= -20) ? -s.lteRsrq : kUnavail;
        sig.lte.rssnr = (s.lteSnr >= -200 && s.lteSnr <= 300) ? s.lteSnr : kUnavail;
    }
    return sig;
}

ScopedAStatus A6lRadioNetwork::getDataRegistrationState(int32_t serial) {
    if (!slotCore().ready()) {
        respond()->getDataRegistrationStateResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    auto r = buildRegState(false);
    LOG(DEBUG) << "[" << serial << "] Network.getDataRegistrationState " << toString(r.regState) << " "
               << toString(r.rat) << " " << r.registeredPlmn;
    respond()->getDataRegistrationStateResponse(noError(serial), r);
    return ok();
}

ScopedAStatus A6lRadioNetwork::getVoiceRegistrationState(int32_t serial) {
    if (!slotCore().ready()) {
        respond()->getVoiceRegistrationStateResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    respond()->getVoiceRegistrationStateResponse(noError(serial), buildRegState(true));
    return ok();
}

ScopedAStatus A6lRadioNetwork::getSignalStrength(int32_t serial) {
    auto& core = slotCore();
    if (!core.ready()) {
        respond()->getSignalStrengthResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    respond()->getSignalStrengthResponse(noError(serial), buildSignal(core.signal(true)));
    return ok();
}

ScopedAStatus A6lRadioNetwork::getOperator(int32_t serial) {
    auto r = buildRegState(false);
    if (r.registeredPlmn.empty()) {
        respond()->getOperatorResponse(noError(serial), "", "", "");
        return ok();
    }
    auto on = slotCore().operatorName();
    auto s = slotCore().serving();
    std::string l = !on.longName.empty() ? on.longName : (s && !s->description.empty()) ? s->description : r.registeredPlmn;
    std::string sh = !on.shortName.empty() ? on.shortName : l;
    respond()->getOperatorResponse(noError(serial), l, sh, r.registeredPlmn);
    return ok();
}

ScopedAStatus A6lRadioNetwork::getVoiceRadioTechnology(int32_t serial) {
    respond()->getVoiceRadioTechnologyResponse(noError(serial), currentRat());
    return ok();
}

ScopedAStatus A6lRadioNetwork::getNetworkSelectionMode(int32_t serial) {
    if (!slotCore().bound()) {  // r5 bug hunt round2 R3: an unbound slot 2's NAS client is SIM 1's
        respond()->getNetworkSelectionModeResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), false);
        return ok();
    }
    auto r = slotCore().ctl().request(qmi::kSvcNas, qmi::Message::request(nas::kGetSystemSelectionPreference));
    bool manual = false;
    if (r.ok())
        if (auto* v = r.msg.get(0x16); v && !v->empty()) manual = (*v)[0] == 1;
    respond()->getNetworkSelectionModeResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)), manual);
    return ok();
}

ScopedAStatus A6lRadioNetwork::setNetworkSelectionModeAutomatic(int32_t serial) {
    if (!slotCore().bound()) {  // r5 bug hunt round2 R3: an unbound slot 2's NAS client is SIM 1's
        respond()->setNetworkSelectionModeAutomaticResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
        return ok();
    }
    auto r = nas::setNetworkSelection(slotCore().ctl(), false, 0, 0, 0);
    if (r.status == qmi::Result::QmiFailure && r.qmiError == qmi::kErrNoEffect) r.status = qmi::Result::Ok;
    respond()->setNetworkSelectionModeAutomaticResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    return ok();
}

// r5 round5 F46: exactly 5/6 digits; the MNC width goes to NAS (TLV 0x12) so 00101 and 001001 stay distinct.
ScopedAStatus A6lRadioNetwork::setNetworkSelectionModeManual(int32_t serial, const std::string& opNumeric,
                                                             AccessNetwork ran) {
    if (!slotCore().bound()) {  // r5 bug hunt round2 R3: an unbound slot 2's NAS client is SIM 1's
        respond()->setNetworkSelectionModeManualResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
        return ok();
    }
    auto plmn = policy::parsePlmn(opNumeric);
    if (!plmn) {
        LOG(WARNING) << "[" << serial << "] Network.setNetworkSelectionModeManual bad operator '" << opNumeric << "'";
        respond()->setNetworkSelectionModeManualResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    int8_t rat = -1;
    if (ran == AccessNetwork::EUTRAN) rat = nas::kRifLte;
    else if (ran == AccessNetwork::UTRAN) rat = nas::kRifUmts;
    else if (ran == AccessNetwork::GERAN) rat = nas::kRifGsm;
    auto r = nas::setNetworkSelection(slotCore().ctl(), true, plmn->mcc, plmn->mnc, rat, plmn->mncThreeDigits);
    LOG(INFO) << "[" << serial << "] Network.setNetworkSelectionModeManual " << opNumeric << " rat=" << int(rat) << ": "
              << r.describe();
    respond()->setNetworkSelectionModeManualResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    return ok();
}

// r5 round5 F45: the RadioAccessFamily bits mirrored in SimNetPolicy.h (host-tested) must match AIDL
static_assert(policy::raf::kGsm == static_cast<int32_t>(RadioAccessFamily::GSM) &&
              policy::raf::kGprs == static_cast<int32_t>(RadioAccessFamily::GPRS) &&
              policy::raf::kEdge == static_cast<int32_t>(RadioAccessFamily::EDGE) &&
              policy::raf::kUmts == static_cast<int32_t>(RadioAccessFamily::UMTS) &&
              policy::raf::kHsdpa == static_cast<int32_t>(RadioAccessFamily::HSDPA) &&
              policy::raf::kHsupa == static_cast<int32_t>(RadioAccessFamily::HSUPA) &&
              policy::raf::kHspa == static_cast<int32_t>(RadioAccessFamily::HSPA) &&
              policy::raf::kHspap == static_cast<int32_t>(RadioAccessFamily::HSPAP) &&
              policy::raf::kLte == static_cast<int32_t>(RadioAccessFamily::LTE) &&
              policy::raf::kLteCa == static_cast<int32_t>(RadioAccessFamily::LTE_CA),
              "RadioAccessFamily mirror out of date");

// r5 round5 F45: the getter reports what this HAL applied (the supported subset of the accepted request), else the
// modem's mode preference; the cache is dropped when the modem restarts.
ScopedAStatus A6lRadioNetwork::getAllowedNetworkTypesBitmap(int32_t serial) {
    if (!slotCore().bound()) {  // r5 bug hunt round2 R3: an unbound slot 2's NAS client is SIM 1's
        respond()->getAllowedNetworkTypesBitmapResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), 0);
        return ok();
    }
    int32_t bm = mAllowedBitmap.load();
    if (bm == 0) {
        uint16_t mm = 0;
        auto r = nas::getModePreference(slotCore().ctl(), &mm);
        if (!r.ok()) {
            respond()->getAllowedNetworkTypesBitmapResponse(errorResponse(serial, toRadioError(r)), 0);
            return ok();
        }
        bm = policy::bitmapForModes(mm);
    }
    respond()->getAllowedNetworkTypesBitmapResponse(noError(serial), bm);
    return ok();
}

// r5 round5 F45: IRadioNetwork: "*only* accept the types of network provided". A request with no GSM/UMTS/LTE bit
// (empty, NR-only, CDMA-only) is rejected without touching the modem; it is never widened to GSM+UMTS+LTE.
ScopedAStatus A6lRadioNetwork::setAllowedNetworkTypesBitmap(int32_t serial, int32_t bm) {
    if (!slotCore().bound()) {  // r5 bug hunt round2 R3: an unbound slot 2's NAS client is SIM 1's
        respond()->setAllowedNetworkTypesBitmapResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
        return ok();
    }
    auto modes = policy::allowedModes(bm);
    if (!modes) {
        LOG(WARNING) << "[" << serial << "] Network.setAllowedNetworkTypesBitmap 0x" << std::hex << bm
                     << ": no GSM/UMTS/LTE bit, rejected (modem unchanged)";
        respond()->setAllowedNetworkTypesBitmapResponse(
                errorResponse(serial, bm == 0 ? RadioError::INVALID_ARGUMENTS : RadioError::MODE_NOT_SUPPORTED));
        return ok();
    }
    auto r = nas::setModePreference(slotCore().ctl(), modes->modeMask);
    LOG(INFO) << "[" << serial << "] Network.setAllowedNetworkTypesBitmap 0x" << std::hex << bm << " -> mode 0x"
              << modes->modeMask << " (applied 0x" << modes->applied << "): " << r.describe();
    if (r.ok()) mAllowedBitmap = modes->applied;
    respond()->setAllowedNetworkTypesBitmapResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    return ok();
}

// volte2 (25 Sep 2026): transparent modem-centric VoLTE. The modem IMS registers by itself once
// the AP hosts IMSDCM (a6l-imsdcm) and brings up the `ims` PDN; we only report what IMSA says.
ScopedAStatus A6lRadioNetwork::getImsRegistrationState(int32_t serial) {
    auto& core = slotCore();
    if (!core.ready()) {
        respond()->getImsRegistrationStateResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), false,
                                                   ::aidl::android::hardware::radio::RadioTechnologyFamily::THREE_GPP);
        return ok();
    }
    auto ims = core.imsState();
    LOG(DEBUG) << "[" << serial << "] Network.getImsRegistrationState registered=" << ims.registered()
               << " " << ims.reg.summary();
    respond()->getImsRegistrationStateResponse(noError(serial), ims.registered(),
                                               ::aidl::android::hardware::radio::RadioTechnologyFamily::THREE_GPP);
    return ok();
}

void A6lRadioNetwork::onImsChanged() { indicate()->imsNetworkStateChanged(RadioIndicationType::UNSOLICITED); }

void A6lRadioNetwork::onModemReady() { indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioNetwork::onModemLost() {
    mAllowedBitmap = 0;  // r5 round5 F45: a restarted modem reports its own mode preference again
    indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED);
}
void A6lRadioNetwork::onNetworkChanged() {
    indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED);
    indicate()->voiceRadioTechChanged(RadioIndicationType::UNSOLICITED, currentRat());
}
void A6lRadioNetwork::onSignalChanged() {
    indicate()->currentSignalStrength(RadioIndicationType::UNSOLICITED, buildSignal(slotCore().signal()));
}
// r5 review round8 F61 (28 Sep 2026): receivedTimeMs = now (sent), ageMs = time the sample waited in the RIL since the
// QMI indication (same CLOCK_BOOTTIME base), so Android's reference time (received - age) is the modem's delivery time.
void A6lRadioNetwork::onNitz(const std::string& nitz, int64_t receivedMs) {
    int64_t now = ::android::elapsedRealtime();
    auto age = nitzAgeMs(receivedMs, now);
    if (!age) {
        LOG(WARNING) << "NITZ sample with invalid receipt time " << receivedMs << " (now " << now << "): dropped";
        return;
    }
    indicate()->nitzTimeReceived(RadioIndicationType::UNSOLICITED, nitz, now, *age);
}

// ======================================================================== data
namespace {
aidlData::SetupDataCallResult toResult(const ::a6l::radio::DataCall& c) {
    aidlData::SetupDataCallResult r{};
    r.cause = aidlData::DataCallFailCause::NONE;
    r.suggestedRetryTime = RadioConst::VALUE_UNAVAILABLE_LONG;
    r.cid = c.cid;
    r.active = aidlData::SetupDataCallResult::DATA_CONNECTION_STATUS_ACTIVE;
    r.type = (c.v4 && c.v6) ? aidlData::PdpProtocolType::IPV4V6
             : c.v6         ? aidlData::PdpProtocolType::IPV6
                            : aidlData::PdpProtocolType::IP;
    r.ifname = c.ifname;
    for (auto& a : c.addresses)
        r.addresses.push_back({.address = a,
                               .addressProperties = aidlData::LinkAddress::ADDRESS_PROPERTY_NONE,
                               .deprecationTime = RadioConst::VALUE_UNAVAILABLE_LONG,
                               .expirationTime = RadioConst::VALUE_UNAVAILABLE_LONG});
    r.dnses = c.dnses;
    r.gateways = c.gateways;
    r.pcscf = c.pcscf;
    r.mtuV4 = c.mtuV4;
    r.mtuV6 = c.mtuV6;
    r.handoverFailureMode = aidlData::SetupDataCallResult::HANDOVER_FAILURE_MODE_LEGACY;
    r.pduSessionId = 0;
    return r;
}
}  // namespace

ScopedAStatus A6lRadioData::setupDataCall(int32_t serial, AccessNetwork accessNetwork,
                                          const aidlData::DataProfileInfo& dp, bool roamingAllowed,
                                          aidlData::DataRequestReason reason,
                                          const std::vector<aidlData::LinkAddress>& addresses,
                                          const std::vector<std::string>& dnses, int32_t pduSessionId,
                                          const std::optional<aidlData::SliceInfo>& sliceInfo,
                                          bool matchAllRuleAllowed) {
    (void)accessNetwork;
    (void)reason;
    (void)addresses;
    (void)dnses;
    (void)pduSessionId;
    (void)sliceInfo;
    (void)matchAllRuleAllowed;
    ::a6l::radio::DataRequest rq;
    rq.apn = dp.apn;
    rq.user = dp.user;
    rq.password = dp.password;
    rq.auth = static_cast<uint8_t>(static_cast<int32_t>(dp.authType) & 3);  // bit0 PAP, bit1 CHAP
    // r5 review F9 (28 Sep 2026): roamingAllowed (the user's data-roaming setting) is kept through the async path and
    // checked against a FRESH serving system on the data executor, right before WDS Start Network. Emergency PDNs
    // are exempt. A prohibited setup never reaches WDS and fails with DATA_ROAMING_SETTINGS_DISABLED.
    const bool emergency =
            (dp.supportedApnTypesBitmap & static_cast<int32_t>(aidlData::ApnTypes::EMERGENCY)) != 0;
    const auto homeProto = dp.protocol, roamProto = dp.roamingProtocol;
    LOG(INFO) << "[" << serial << "] Data.setupDataCall apn='" << rq.apn << "' roamingAllowed=" << roamingAllowed
              << " emergency=" << emergency;
    mExec.post([this, serial, rq, roamingAllowed, emergency, homeProto, roamProto]() mutable {
        auto& core = slotCore();
        aidlData::SetupDataCallResult res{};
        if (!core.ready() || !core.bound()) {
            respond()->setupDataCallResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), res);
            return;
        }
        auto s = core.serving(true);
        bool roaming = servingIsRoaming(s);
        if (checkDataRoaming(s, roamingAllowed, emergency) == RoamingVerdict::RejectRoaming) {
            LOG(WARNING) << "[" << serial << "] setupDataCall refused: roaming and data roaming disabled (no WDS start)";
            res.cause = static_cast<aidlData::DataCallFailCause>(kFailCauseDataRoamingSettingsDisabled);
            res.suggestedRetryTime = RadioConst::VALUE_UNAVAILABLE_LONG;
            res.active = aidlData::SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
            respond()->setupDataCallResponse(noError(serial), res);
            return;
        }
        auto proto = roaming ? roamProto : homeProto;
        rq.protocol = proto == aidlData::PdpProtocolType::IPV6     ? ::a6l::radio::Protocol::V6
                      : proto == aidlData::PdpProtocolType::IPV4V6 ? ::a6l::radio::Protocol::V4V6
                                                                   : ::a6l::radio::Protocol::V4;
        LOG(INFO) << "[" << serial << "] setupDataCall proto=" << toString(proto) << " roaming=" << roaming;
        // DSDS: the IPA port (DPM + WDA data format) belongs to slot 1's manager
        if (core.sub() > 0) ModemCore::get(1).data().prepareFormat();
        auto o = core.data().setup(rq);
        if (!o.ok) {
            LOG(WARNING) << "[" << serial << "] setupDataCall failed cause=0x" << std::hex << o.failCause << " "
                         << o.detail;
            res.cause = static_cast<aidlData::DataCallFailCause>(o.failCause);
            // no IPA netdev: do not let the framework retry every few seconds
            res.suggestedRetryTime = o.failCause == 0x1001 ? 10 * 60 * 1000 : RadioConst::VALUE_UNAVAILABLE_LONG;
            res.active = aidlData::SetupDataCallResult::DATA_CONNECTION_STATUS_INACTIVE;
            respond()->setupDataCallResponse(noError(serial), res);
            return;
        }
        res = toResult(o.call);
        LOG(INFO) << "[" << serial << "] data call up: " << res.toString();
        if (!roamingAllowed && !emergency) {
            std::lock_guard<std::mutex> g(mRoamLock);
            mNoRoamCids.insert(res.cid);
        }
        setupDataCallBase(res);
        respond()->setupDataCallResponse(noError(serial), res);
    });
    return ok();
}

ScopedAStatus A6lRadioData::deactivateDataCall(int32_t serial, int32_t cid, aidlData::DataRequestReason reason) {
    LOG(INFO) << "[" << serial << "] Data.deactivateDataCall cid=" << cid << " " << toString(reason);
    mExec.post([this, serial, cid] {
        slotCore().data().deactivate(cid);
        deactivateDataCallBase(cid);
        {
            std::lock_guard<std::mutex> g(mRoamLock);
            mNoRoamCids.erase(cid);
        }
        respond()->deactivateDataCallResponse(noError(serial));
    });
    return ok();
}

// r5 review F12 (28 Sep 2026): honest answer. Programming the LTE attach APN means rewriting a persistent modem profile
// (WDS Modify/Create Profile + attach PDN list, as stock qcril/qdp does); the profile TLV layout and the attach profile
// selection of this MPSS are not verified against a stock capture yet, and a wrong write survives reboots and can break
// LTE attach. Until then the modem attaches with its own configured profile and Android is told so.
ScopedAStatus A6lRadioData::setInitialAttachApn(int32_t serial, const std::optional<aidlData::DataProfileInfo>& dp) {
    LOG(INFO) << "[" << serial << "] Data.setInitialAttachApn apn='" << (dp ? dp->apn : "")
              << "': REQUEST_NOT_SUPPORTED (modem keeps its own attach profile)";
    respond()->setInitialAttachApnResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED));
    return ok();
}

void A6lRadioData::onModemLost() {
    mExec.post([this] {
        for (auto& c : getDataCallListBase()) deactivateDataCallBase(c.cid);
        std::lock_guard<std::mutex> g(mRoamLock);
        mNoRoamCids.clear();
    });
}

// r5 F9: a call set up while data roaming was disallowed must not continue on a roaming network (the framework also
// re-evaluates, but the HAL does not rely on it). Teardown is reported like a network-side loss.
void A6lRadioData::onNetworkChanged() {
    mExec.post([this] {
        if (!servingIsRoaming(slotCore().serving())) return;
        std::set<int32_t> cids;
        {
            std::lock_guard<std::mutex> g(mRoamLock);
            cids.swap(mNoRoamCids);
        }
        for (int32_t cid : cids) {
            LOG(WARNING) << "data call cid " << cid << " set up without roaming permission: now roaming, tearing down";
            slotCore().data().deactivate(cid);
            deactivateDataCallBase(cid);
        }
    });
}

// r5 review F28 (28 Sep 2026): the loss names (cid, generation). A loss queued behind an explicit teardown and a new
// setup that reused the cid is ignored instead of destroying the new connection and Android's entry for it.
void A6lRadioData::onDataCallLost(int cid, uint64_t generation) {
    mExec.post([this, cid, generation] {
        if (!slotCore().data().deactivateIfCurrent(cid, generation)) return;
        deactivateDataCallBase(cid);
        std::lock_guard<std::mutex> g(mRoamLock);
        mNoRoamCids.erase(cid);
    });
}

// r5 review round8 F59 (28 Sep 2026): CONNECTED + reconfiguration required. The settings are re-read; changed DNS /
// gateway / P-CSCF / MTU are published again (dataCallListChanged with the updated entry); changed addresses or
// unusable settings tear the call down like a network loss so Android reconnects instead of keeping stale parameters.
void A6lRadioData::onDataCallReconfigured(int cid, uint64_t generation) {
    mExec.post([this, cid, generation] {
        ::a6l::radio::DataCall c;
        auto r = slotCore().data().refresh(cid, generation, &c);
        using R = ::a6l::radio::DataCallManager::Refresh;
        if (r == R::Updated) {
            LOG(INFO) << "data call cid " << cid << " reconfigured by the modem: publishing new parameters";
            setupDataCallBase(toResult(c));
        } else if (r == R::Invalidate) {
            LOG(WARNING) << "data call cid " << cid << " reconfiguration cannot be applied in place: tearing down";
            if (!slotCore().data().deactivateIfCurrent(cid, generation)) return;
            deactivateDataCallBase(cid);
            std::lock_guard<std::mutex> g(mRoamLock);
            mNoRoamCids.erase(cid);
        }
    });
}

}  // namespace android::hardware::radio::a6l
