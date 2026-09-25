// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): IRadioConfig, IRadioModem, IRadioSim.
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"

#include <a6lqmi/message.h>
#include <aidl/android/hardware/radio/RadioTechnology.h>
#include <libminradio/debug.h>
#include <libminradio/response.h>
#include <pthread.h>

#include <cstdlib>

namespace android::hardware::radio::a6l {

using ::aidl::android::hardware::radio::RadioError;
using ::aidl::android::hardware::radio::RadioIndicationType;
using ::aidl::android::hardware::radio::RadioTechnology;
using ::android::hardware::radio::minimal::errorResponse;
using ::android::hardware::radio::minimal::noError;
using ::ndk::ScopedAStatus;
namespace qmi = ::a6l::qmi;
namespace uim = ::a6l::qmi::uim;
namespace aidlSim = ::aidl::android::hardware::radio::sim;
namespace aidlModem = ::aidl::android::hardware::radio::modem;
namespace aidlConfig = ::aidl::android::hardware::radio::config;
constexpr auto ok = &ScopedAStatus::ok;

// ======================================================================== Executor
Executor::Executor(const char* name) {
    mThread = std::thread([this, n = std::string(name)] {
        pthread_setname_np(pthread_self(), n.substr(0, 15).c_str());
        while (true) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> l(mLock);
                mCv.wait(l, [this] { return !mQ.empty(); });
                fn = std::move(mQ.front());
                mQ.pop_front();
            }
            fn();
        }
    });
    mThread.detach();
}

void Executor::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> l(mLock);
    mQ.push_back(std::move(fn));
    mCv.notify_one();
}

// ======================================================================== helpers
namespace {
aidlSim::PinState pinState(uint8_t s) {
    switch (s) {
        case uim::kPinEnabledNotVerified: return aidlSim::PinState::ENABLED_NOT_VERIFIED;
        case uim::kPinEnabledVerified: return aidlSim::PinState::ENABLED_VERIFIED;
        case uim::kPinDisabled: return aidlSim::PinState::DISABLED;
        case uim::kPinBlocked: return aidlSim::PinState::ENABLED_BLOCKED;
        case uim::kPinPermBlocked: return aidlSim::PinState::ENABLED_PERM_BLOCKED;
        default: return aidlSim::PinState::UNKNOWN;
    }
}
int32_t appState(uint8_t s) {
    switch (s) {
        case uim::kAppStateDetected: return aidlSim::AppStatus::APP_STATE_DETECTED;
        case uim::kAppStatePin: return aidlSim::AppStatus::APP_STATE_PIN;
        case uim::kAppStatePuk:
        case uim::kAppStatePinBlocked: return aidlSim::AppStatus::APP_STATE_PUK;
        case uim::kAppStatePerso: return aidlSim::AppStatus::APP_STATE_SUBSCRIPTION_PERSO;
        case uim::kAppStateReady: return aidlSim::AppStatus::APP_STATE_READY;
        default: return aidlSim::AppStatus::APP_STATE_UNKNOWN;
    }
}
uint16_t hexU16(const std::string& s) { return static_cast<uint16_t>(strtoul(s.c_str(), nullptr, 16)); }

std::vector<uint16_t> parsePath(const std::string& path) {
    std::vector<uint16_t> v;
    for (size_t i = 0; i + 4 <= path.size(); i += 4) v.push_back(hexU16(path.substr(i, 4)));
    if (v.empty()) v.push_back(0x3F00);
    return v;
}
RadioError qmiToRadioError(const qmi::Result& r) {
    switch (r.status) {
        case qmi::Result::Ok: return RadioError::NONE;
        case qmi::Result::NoService:
        case qmi::Result::Stopped: return RadioError::RADIO_NOT_AVAILABLE;
        case qmi::Result::Timeout:
        case qmi::Result::TransportError: return RadioError::MODEM_ERR;
        case qmi::Result::QmiFailure: break;
    }
    switch (r.qmiError) {
        case qmi::kErrIncorrectPin: return RadioError::PASSWORD_INCORRECT;
        case qmi::kErrPinBlocked:
        case qmi::kErrPinAlwaysBlocked: return RadioError::PASSWORD_INCORRECT;
        case qmi::kErrNoSim:
        case qmi::kErrUimUninitialized: return RadioError::SIM_ABSENT;
        case qmi::kErrInvalidArgument:
        case qmi::kErrMissingArgument: return RadioError::INVALID_ARGUMENTS;
        case qmi::kErrNotSupported:
        case qmi::kErrInvalidQmiCommand: return RadioError::REQUEST_NOT_SUPPORTED;
        case qmi::kErrNoRadio: return RadioError::RADIO_NOT_AVAILABLE;
        case qmi::kErrFdnRestrict: return RadioError::FDN_CHECK_FAILURE;
        case qmi::kErrNoNetworkFound: return RadioError::NO_NETWORK_FOUND;
        case qmi::kErrNetworkNotReady: return RadioError::NETWORK_NOT_READY;
        case qmi::kErrDeviceNotReady: return RadioError::INVALID_MODEM_STATE;
        default: return RadioError::GENERIC_FAILURE;
    }
}
}  // namespace

RadioError toRadioError(const qmi::Result& r) { return qmiToRadioError(r); }

// ======================================================================== config
ScopedAStatus A6lRadioConfig::getSimSlotsStatus(int32_t serial) {
    LOG(DEBUG) << "[" << serial << "] Config.getSimSlotsStatus";
    auto& core = ModemCore::get();
    if (!core.ready()) {
        respond()->getSimSlotsStatusResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    auto cs = core.cardStatus();
    const uim::Card* card = cs ? cs->primaryCard() : nullptr;
    aidlConfig::SimSlotStatus st{
            .cardState = card ? card->state : aidlSim::CardStatus::STATE_ABSENT,
            .atr = "",
            .eid = "",
            .portInfo = {{
                    .iccId = card && card->state == uim::kCardPresent ? core.iccid() : "",
                    .logicalSlotId = 0,
                    .portActive = true,
            }},
    };
    respond()->getSimSlotsStatusResponse(noError(serial), {st});
    return ok();
}

// ======================================================================== modem
A6lRadioModem::A6lRadioModem(std::shared_ptr<minimal::SlotContext> context)
    : minimal::RadioModem(context, {RadioTechnology::GSM, RadioTechnology::GPRS, RadioTechnology::EDGE,
                                    RadioTechnology::UMTS, RadioTechnology::HSDPA, RadioTechnology::HSUPA,
                                    RadioTechnology::HSPA, RadioTechnology::HSPAP, RadioTechnology::LTE}) {}

void A6lRadioModem::onUpdatedResponseFunctions() {
    auto& core = ModemCore::get();
    indicate()->rilConnected(RadioIndicationType::UNSOLICITED);
    indicate()->radioStateChanged(
            RadioIndicationType::UNSOLICITED,
            !core.ready() ? aidlModem::RadioState::UNAVAILABLE
                          : core.radioOn() ? aidlModem::RadioState::ON : aidlModem::RadioState::OFF);
}

ScopedAStatus A6lRadioModem::getImei(int32_t serial) {
    auto ids = ModemCore::get().ids();
    if (ids.imei.empty()) {
        respond()->getImeiResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    aidlModem::ImeiInfo info{
            .type = aidlModem::ImeiInfo::ImeiType::PRIMARY,
            .imei = ids.imei,
            .svn = ids.imeisv,
    };
    respond()->getImeiResponse(noError(serial), info);
    return ok();
}

ScopedAStatus A6lRadioModem::getBasebandVersion(int32_t serial) {
    auto rev = ModemCore::get().revision();
    respond()->getBasebandVersionResponse(noError(serial), rev.empty() ? "a6l-qmi (modem down)" : rev);
    return ok();
}

ScopedAStatus A6lRadioModem::setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                           bool preferredForEmergencyCall) {
    LOG(INFO) << "[" << serial << "] Modem.setRadioPower " << powerOn << " emergency=" << forEmergencyCall
              << "/" << preferredForEmergencyCall;
    auto& core = ModemCore::get();
    if (!core.ready()) {
        respond()->setRadioPowerResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
        return ok();
    }
    bool okp = core.setRadioPower(powerOn);
    respond()->setRadioPowerResponse(okp ? noError(serial) : errorResponse(serial, RadioError::MODEM_ERR));
    return ok();
}

void A6lRadioModem::onModemReady() {
    auto& core = ModemCore::get();
    indicate()->radioStateChanged(RadioIndicationType::UNSOLICITED,
                                  core.radioOn() ? aidlModem::RadioState::ON : aidlModem::RadioState::OFF);
}
void A6lRadioModem::onModemLost() {
    indicate()->radioStateChanged(RadioIndicationType::UNSOLICITED, aidlModem::RadioState::UNAVAILABLE);
}
void A6lRadioModem::onRadioPowerChanged(bool on) {
    indicate()->radioStateChanged(RadioIndicationType::UNSOLICITED,
                                  on ? aidlModem::RadioState::ON : aidlModem::RadioState::OFF);
}

// ======================================================================== sim
A6lRadioSim::A6lRadioSim(std::shared_ptr<minimal::SlotContext> context) : minimal::RadioSim(context) {}

uim::Session A6lRadioSim::sessionFor(const std::string& aidHex) {
    if (aidHex.empty()) return uim::Session{uim::kSessionCardSlot1, {}};
    auto aid = qmi::unhex(aidHex).value_or(std::vector<uint8_t>{});
    auto cs = ModemCore::get().cardStatus();
    const uim::App* primary = cs ? cs->primaryGwApp() : nullptr;
    if (primary && primary->aid == aid) return uim::Session{uim::kSessionPrimaryGw, {}};
    return uim::Session{4 /* non-provisioning slot 1 */, aid};
}

ScopedAStatus A6lRadioSim::getIccCardStatus(int32_t serial) {
    auto& core = ModemCore::get();
    if (!core.ready()) {
        respond()->getIccCardStatusResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    auto cs = core.cardStatus();
    aidlSim::CardStatus out{};
    out.cardState = aidlSim::CardStatus::STATE_ABSENT;
    out.universalPinState = aidlSim::PinState::UNKNOWN;
    out.gsmUmtsSubscriptionAppIndex = -1;
    out.cdmaSubscriptionAppIndex = -1;
    out.imsSubscriptionAppIndex = -1;
    out.slotMap = {.physicalSlotId = 0, .portId = 0};
    const uim::Card* card = cs ? cs->primaryCard() : nullptr;
    if (card) {
        out.cardState = card->state;
        out.universalPinState = pinState(card->upinState);
        for (size_t i = 0; i < card->apps.size(); i++) {
            const auto& a = card->apps[i];
            aidlSim::AppStatus as{};
            as.appType = a.type;
            as.appState = appState(a.state);
            as.persoSubstate = a.state == uim::kAppStateReady ? aidlSim::PersoSubstate::READY
                                                              : aidlSim::PersoSubstate::UNKNOWN;
            as.aidPtr = qmi::hex(a.aid);
            as.appLabelPtr = "";
            as.pin1Replaced = a.upinReplacesPin1;
            as.pin1 = pinState(a.pin1State);
            as.pin2 = pinState(a.pin2State);
            out.applications.push_back(as);
            if (a.type == uim::kAppIsim && out.imsSubscriptionAppIndex < 0)
                out.imsSubscriptionAppIndex = static_cast<int32_t>(i);
        }
        if (cs->indexGwPrimary != 0xFFFF && (cs->indexGwPrimary & 0xff) < card->apps.size())
            out.gsmUmtsSubscriptionAppIndex = cs->indexGwPrimary & 0xff;
        else
            for (size_t i = 0; i < card->apps.size(); i++)
                if (card->apps[i].type == uim::kAppUsim || card->apps[i].type == uim::kAppSim) {
                    out.gsmUmtsSubscriptionAppIndex = static_cast<int32_t>(i);
                    break;
                }
        if (card->state == uim::kCardPresent) out.iccid = core.iccid();
    }
    LOG(INFO) << "[" << serial << "] Sim.getIccCardStatus state=" << out.cardState
              << " apps=" << out.applications.size() << " gw=" << out.gsmUmtsSubscriptionAppIndex;
    respond()->getIccCardStatusResponse(noError(serial), out);
    return ok();
}

ScopedAStatus A6lRadioSim::getImsiForApp(int32_t serial, const std::string& aid) {
    auto& core = ModemCore::get();
    std::string imsi;
    auto r = uim::readImsi(core.ctl(), sessionFor(aid).type == uim::kSessionPrimaryGw
                                               ? std::vector<uint8_t>{}
                                               : qmi::unhex(aid).value_or(std::vector<uint8_t>{}),
                           &imsi);
    LOG(INFO) << "[" << serial << "] Sim.getImsiForApp " << r.describe()
              << " mccmnc=" << imsi.substr(0, imsi.size() > 5 ? 5 : imsi.size());
    respond()->getImsiForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)), imsi);
    return ok();
}

ScopedAStatus A6lRadioSim::iccIoForApp(int32_t serial, const aidlSim::IccIo& io) {
    auto& core = ModemCore::get();
    uim::Session s = sessionFor(io.aid);
    uim::FilePath f{static_cast<uint16_t>(io.fileId), parsePath(io.path)};
    aidlSim::IccIoResult res{};
    qmi::Result r;
    switch (io.command) {
        case 0xB0: {  // READ BINARY
            uim::IoResult ior;
            r = uim::readTransparent(core.ctl(), s, f, static_cast<uint16_t>((io.p1 << 8) | io.p2),
                                     static_cast<uint16_t>(io.p3), &ior);
            res.sw1 = ior.hasSw ? ior.sw1 : (r.ok() ? 0x90 : 0x6F);
            res.sw2 = ior.hasSw ? ior.sw2 : 0x00;
            res.simResponse = qmi::hex(ior.data);
            break;
        }
        case 0xB2: {  // READ RECORD
            uim::IoResult ior;
            r = uim::readRecord(core.ctl(), s, f, static_cast<uint16_t>(io.p1), static_cast<uint16_t>(io.p3), &ior);
            res.sw1 = ior.hasSw ? ior.sw1 : (r.ok() ? 0x90 : 0x6F);
            res.sw2 = ior.hasSw ? ior.sw2 : 0x00;
            res.simResponse = qmi::hex(ior.data);
            break;
        }
        case 0xC0: {  // GET RESPONSE -> TS 51.011 format expected by IccFileHandler
            uim::FileAttributes fa;
            r = uim::getFileAttributes(core.ctl(), s, f, &fa);
            if (r.ok()) {
                res.sw1 = 0x90;
                res.sw2 = 0x00;
                res.simResponse = qmi::hex(uim::toGsmGetResponse(fa));
            } else {
                res.sw1 = fa.hasSw ? fa.sw1 : 0x6A;
                res.sw2 = fa.hasSw ? fa.sw2 : 0x82;  // file not found
            }
            break;
        }
        case 0xD6:    // UPDATE BINARY
        case 0xDC: {  // UPDATE RECORD
            auto data = qmi::unhex(io.data).value_or(std::vector<uint8_t>{});
            uim::IoResult ior;
            r = io.command == 0xD6
                        ? uim::writeTransparent(core.ctl(), s, f, static_cast<uint16_t>((io.p1 << 8) | io.p2), data, &ior)
                        : uim::writeRecord(core.ctl(), s, f, static_cast<uint16_t>(io.p1), data, &ior);
            res.sw1 = ior.hasSw ? ior.sw1 : (r.ok() ? 0x90 : 0x6F);
            res.sw2 = ior.hasSw ? ior.sw2 : 0x00;
            break;
        }
        default:
            LOG(WARNING) << "[" << serial << "] Sim.iccIoForApp unsupported command 0x" << std::hex << io.command;
            respond()->iccIoForAppResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED), {});
            return ok();
    }
    LOG(DEBUG) << "[" << serial << "] Sim.iccIo cmd=" << io.command << " fid=" << std::hex << io.fileId
               << " path=" << io.path << " -> " << r.describe() << " sw=" << res.sw1 << res.sw2;
    if (r.status != qmi::Result::Ok && r.status != qmi::Result::QmiFailure) {
        respond()->iccIoForAppResponse(errorResponse(serial, qmiToRadioError(r)), res);
    } else {
        respond()->iccIoForAppResponse(noError(serial), res);  // card status words carry the verdict
    }
    return ok();
}

ScopedAStatus A6lRadioSim::supplyIccPinForApp(int32_t serial, const std::string& pin, const std::string& aid) {
    uim::PinResult pr;
    auto r = uim::verifyPin(ModemCore::get().ctl(), sessionFor(aid), uim::kPin1, pin, &pr);
    LOG(INFO) << "[" << serial << "] Sim.supplyIccPin " << r.describe() << " left=" << pr.verifyLeft;
    respond()->supplyIccPinForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                          pr.verifyLeft);
    return ok();
}

ScopedAStatus A6lRadioSim::supplyIccPukForApp(int32_t serial, const std::string& puk, const std::string& pin,
                                              const std::string& aid) {
    uim::PinResult pr;
    auto r = uim::unblockPin(ModemCore::get().ctl(), sessionFor(aid), uim::kPin1, puk, pin, &pr);
    LOG(INFO) << "[" << serial << "] Sim.supplyIccPuk " << r.describe() << " left=" << pr.unblockLeft;
    respond()->supplyIccPukForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                          pr.unblockLeft);
    return ok();
}

ScopedAStatus A6lRadioSim::changeIccPinForApp(int32_t serial, const std::string& oldPin,
                                              const std::string& newPin, const std::string& aid) {
    uim::PinResult pr;
    auto r = uim::changePin(ModemCore::get().ctl(), sessionFor(aid), uim::kPin1, oldPin, newPin, &pr);
    respond()->changeIccPinForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                          pr.verifyLeft);
    return ok();
}

ScopedAStatus A6lRadioSim::getFacilityLockForApp(int32_t serial, const std::string& facility,
                                                 const std::string& password, int32_t serviceClass,
                                                 const std::string& appId) {
    (void)password;
    (void)serviceClass;
    (void)appId;
    int32_t locked = 0;
    if (facility == "SC") {
        auto cs = ModemCore::get().cardStatus(true);
        const uim::App* a = cs ? cs->primaryGwApp() : nullptr;
        if (!a) {
            respond()->getFacilityLockForAppResponse(errorResponse(serial, RadioError::SIM_ABSENT), 0);
            return ok();
        }
        uint8_t st = a->upinReplacesPin1 && cs->primaryCard() ? cs->primaryCard()->upinState : a->pin1State;
        locked = (st == uim::kPinEnabledNotVerified || st == uim::kPinEnabledVerified ||
                  st == uim::kPinBlocked || st == uim::kPinPermBlocked) ? 1 : 0;
    } else if (facility != "FD") {
        respond()->getFacilityLockForAppResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED), 0);
        return ok();
    }
    respond()->getFacilityLockForAppResponse(noError(serial), locked);
    return ok();
}

ScopedAStatus A6lRadioSim::setFacilityLockForApp(int32_t serial, const std::string& facility, bool lockState,
                                                 const std::string& passwd, int32_t serviceClass,
                                                 const std::string& appId) {
    (void)serviceClass;
    if (facility != "SC") {
        respond()->setFacilityLockForAppResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED), -1);
        return ok();
    }
    uim::PinResult pr;
    auto r = uim::setPinProtection(ModemCore::get().ctl(), sessionFor(appId), uim::kPin1, lockState, passwd, &pr);
    respond()->setFacilityLockForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                             pr.verifyLeft);
    return ok();
}

void A6lRadioSim::onModemReady() { indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioSim::onModemLost() { indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioSim::onSimChanged() { indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED); }

}  // namespace android::hardware::radio::a6l
