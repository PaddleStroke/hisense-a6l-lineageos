// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): IRadioNetwork, IRadioData.
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"

#include <a6lqmi/message.h>
#include <aidl/android/hardware/radio/AccessNetwork.h>
#include <aidl/android/hardware/radio/RadioAccessFamily.h>
#include <aidl/android/hardware/radio/RadioConst.h>
#include <aidl/android/hardware/radio/RadioTechnology.h>
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
    auto s = ModemCore::get().serving();
    return s ? ratOf(*s) : RadioTechnology::UNKNOWN;
}

aidlNet::RegStateResult A6lRadioNetwork::buildRegState(bool voice) {
    auto& core = ModemCore::get();
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
    if (!ModemCore::get().ready()) {
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
    if (!ModemCore::get().ready()) {
        respond()->getVoiceRegistrationStateResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    respond()->getVoiceRegistrationStateResponse(noError(serial), buildRegState(true));
    return ok();
}

ScopedAStatus A6lRadioNetwork::getSignalStrength(int32_t serial) {
    auto& core = ModemCore::get();
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
    auto on = ModemCore::get().operatorName();
    auto s = ModemCore::get().serving();
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
    auto r = ModemCore::get().ctl().request(qmi::kSvcNas, qmi::Message::request(nas::kGetSystemSelectionPreference));
    bool manual = false;
    if (r.ok())
        if (auto* v = r.msg.get(0x16); v && !v->empty()) manual = (*v)[0] == 1;
    respond()->getNetworkSelectionModeResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)), manual);
    return ok();
}

ScopedAStatus A6lRadioNetwork::setNetworkSelectionModeAutomatic(int32_t serial) {
    auto r = nas::setNetworkSelection(ModemCore::get().ctl(), false, 0, 0, 0);
    if (r.status == qmi::Result::QmiFailure && r.qmiError == qmi::kErrNoEffect) r.status = qmi::Result::Ok;
    respond()->setNetworkSelectionModeAutomaticResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    return ok();
}

ScopedAStatus A6lRadioNetwork::setNetworkSelectionModeManual(int32_t serial, const std::string& opNumeric,
                                                             AccessNetwork ran) {
    if (opNumeric.size() < 5) {
        respond()->setNetworkSelectionModeManualResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    uint16_t mcc = static_cast<uint16_t>(atoi(opNumeric.substr(0, 3).c_str()));
    uint16_t mnc = static_cast<uint16_t>(atoi(opNumeric.substr(3).c_str()));
    int8_t rat = -1;
    if (ran == AccessNetwork::EUTRAN) rat = nas::kRifLte;
    else if (ran == AccessNetwork::UTRAN) rat = nas::kRifUmts;
    else if (ran == AccessNetwork::GERAN) rat = nas::kRifGsm;
    auto r = nas::setNetworkSelection(ModemCore::get().ctl(), true, mcc, mnc, rat);
    respond()->setNetworkSelectionModeManualResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    return ok();
}

ScopedAStatus A6lRadioNetwork::getAllowedNetworkTypesBitmap(int32_t serial) {
    int32_t bm = mAllowedBitmap;
    if (bm == 0) {
        uint16_t mm = 0;
        if (nas::getModePreference(ModemCore::get().ctl(), &mm).ok()) {
            if (mm & nas::kModeGsm) bm |= (int32_t)RadioAccessFamily::GSM | (int32_t)RadioAccessFamily::GPRS | (int32_t)RadioAccessFamily::EDGE;
            if (mm & nas::kModeUmts) bm |= (int32_t)RadioAccessFamily::UMTS | (int32_t)RadioAccessFamily::HSDPA | (int32_t)RadioAccessFamily::HSUPA | (int32_t)RadioAccessFamily::HSPA | (int32_t)RadioAccessFamily::HSPAP;
            if (mm & nas::kModeLte) bm |= (int32_t)RadioAccessFamily::LTE | (int32_t)RadioAccessFamily::LTE_CA;
        }
    }
    respond()->getAllowedNetworkTypesBitmapResponse(noError(serial), bm);
    return ok();
}

ScopedAStatus A6lRadioNetwork::setAllowedNetworkTypesBitmap(int32_t serial, int32_t bm) {
    uint16_t mm = 0;
    auto has = [&](RadioAccessFamily f) { return (bm & static_cast<int32_t>(f)) != 0; };
    if (has(RadioAccessFamily::GSM) || has(RadioAccessFamily::GPRS) || has(RadioAccessFamily::EDGE)) mm |= nas::kModeGsm;
    if (has(RadioAccessFamily::UMTS) || has(RadioAccessFamily::HSDPA) || has(RadioAccessFamily::HSUPA) ||
        has(RadioAccessFamily::HSPA) || has(RadioAccessFamily::HSPAP)) mm |= nas::kModeUmts;
    if (has(RadioAccessFamily::LTE) || has(RadioAccessFamily::LTE_CA)) mm |= nas::kModeLte;
    if (mm == 0) mm = nas::kModeGsm | nas::kModeUmts | nas::kModeLte;
    auto r = nas::setModePreference(ModemCore::get().ctl(), mm);
    LOG(INFO) << "[" << serial << "] Network.setAllowedNetworkTypesBitmap 0x" << std::hex << bm << " -> mode 0x" << mm
              << ": " << r.describe();
    if (r.ok()) mAllowedBitmap = bm;
    respond()->setAllowedNetworkTypesBitmapResponse(r.ok() ? noError(serial) : errorResponse(serial, toRadioError(r)));
    return ok();
}

void A6lRadioNetwork::onModemReady() { indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioNetwork::onModemLost() { indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioNetwork::onNetworkChanged() {
    indicate()->networkStateChanged(RadioIndicationType::UNSOLICITED);
    indicate()->voiceRadioTechChanged(RadioIndicationType::UNSOLICITED, currentRat());
}
void A6lRadioNetwork::onSignalChanged() {
    indicate()->currentSignalStrength(RadioIndicationType::UNSOLICITED, buildSignal(ModemCore::get().signal()));
}
void A6lRadioNetwork::onNitz(const std::string& nitz, int64_t) {
    indicate()->nitzTimeReceived(RadioIndicationType::UNSOLICITED, nitz, ::android::elapsedRealtime(), 0);
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
    auto s = ModemCore::get().serving();
    bool roaming = s && s->roaming && *s->roaming;
    auto proto = roaming ? dp.roamingProtocol : dp.protocol;
    rq.protocol = proto == aidlData::PdpProtocolType::IPV6     ? ::a6l::radio::Protocol::V6
                  : proto == aidlData::PdpProtocolType::IPV4V6 ? ::a6l::radio::Protocol::V4V6
                                                               : ::a6l::radio::Protocol::V4;
    LOG(INFO) << "[" << serial << "] Data.setupDataCall apn='" << rq.apn << "' proto=" << toString(proto)
              << " roaming=" << roaming << "/" << roamingAllowed;
    mExec.post([this, serial, rq] {
        auto& core = ModemCore::get();
        aidlData::SetupDataCallResult res{};
        if (!core.ready()) {
            respond()->setupDataCallResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), res);
            return;
        }
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
        setupDataCallBase(res);
        respond()->setupDataCallResponse(noError(serial), res);
    });
    return ok();
}

ScopedAStatus A6lRadioData::deactivateDataCall(int32_t serial, int32_t cid, aidlData::DataRequestReason reason) {
    LOG(INFO) << "[" << serial << "] Data.deactivateDataCall cid=" << cid << " " << toString(reason);
    mExec.post([this, serial, cid] {
        ModemCore::get().data().deactivate(cid);
        deactivateDataCallBase(cid);
        respond()->deactivateDataCallResponse(noError(serial));
    });
    return ok();
}

ScopedAStatus A6lRadioData::setInitialAttachApn(int32_t serial, const std::optional<aidlData::DataProfileInfo>& dp) {
    // The modem attaches with its own default (profile 1) for now; programming it needs WDS
    // Modify Profile (not implemented yet).
    LOG(INFO) << "[" << serial << "] Data.setInitialAttachApn (not programmed) apn='" << (dp ? dp->apn : "") << "'";
    respond()->setInitialAttachApnResponse(noError(serial));
    return ok();
}

void A6lRadioData::onModemLost() {
    mExec.post([this] {
        for (auto& c : getDataCallListBase()) deactivateDataCallBase(c.cid);
    });
}

void A6lRadioData::onDataCallLost(int cid) {
    mExec.post([this, cid] {
        ModemCore::get().data().deactivate(cid);
        deactivateDataCallBase(cid);
    });
}

}  // namespace android::hardware::radio::a6l
