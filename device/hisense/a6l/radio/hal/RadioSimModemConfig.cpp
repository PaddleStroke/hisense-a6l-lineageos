// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (agent ril): IRadioConfig, IRadioModem, IRadioSim.
#define LOG_TAG "a6l-radio"
#include "RadioImpl.h"
#include "SimNetPolicy.h"  // r5 round5 F42/F48

#include <a6lqmi/message.h>
#include <a6lqmi/multisim.h>
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
// ril3 (DSDS): physical slots come from UIM Get Card Status (card i = physical slot i). Logical
// slot N is the subscription N-1; its physical slot is the card the subscription is provisioned
// from (multisim::viewFor). Single SIM (slotCount()==1): exactly one entry, as in v1.
ScopedAStatus A6lRadioConfig::getSimSlotsStatus(int32_t serial) {
    LOG(DEBUG) << "[" << serial << "] Config.getSimSlotsStatus";
    auto& core = ModemCore::get(1);
    if (!core.ready()) {
        respond()->getSimSlotsStatusResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    const int n = ModemCore::slotCount();
    auto cs = core.cardStatus();
    std::vector<aidlConfig::SimSlotStatus> out;
    for (int phys = 0; phys < n; phys++) {
        const uim::Card* card = cs && phys < static_cast<int>(cs->cards.size()) ? &cs->cards[phys] : nullptr;
        int logical = phys;  // default mapping
        for (int sub = 0; sub < n && cs; sub++)
            if (qmi::multisim::viewFor(*cs, sub).card == phys) logical = sub;
        std::string iccid;
        if (card && card->state == uim::kCardPresent) iccid = ModemCore::get(logical + 1).iccid();
        out.push_back(aidlConfig::SimSlotStatus{
                .cardState = card ? card->state : aidlSim::CardStatus::STATE_ABSENT,
                .atr = "",
                .eid = "",
                .portInfo = {{
                        .iccId = iccid,
                        .logicalSlotId = logical,
                        .portActive = true,
                }},
        });
        LOG(DEBUG) << "slot status phys=" << phys << " card_state=" << (card ? int(card->state) : -1)
                   << " logical=" << logical;
    }
    respond()->getSimSlotsStatusResponse(noError(serial), out);
    return ok();
}

ScopedAStatus A6lRadioConfig::getNumOfLiveModems(int32_t serial) {
    respond()->getNumOfLiveModemsResponse(noError(serial), static_cast<int8_t>(ModemCore::slotCount()));
    return ok();
}

ScopedAStatus A6lRadioConfig::getPhoneCapability(int32_t serial) {
    const int n = ModemCore::slotCount();
    aidlConfig::PhoneCapability cap{
            .maxActiveData = 1,  // DSDS: one PS subscription at a time
            .maxActiveInternetData = 1,
            .isInternetLingeringSupported = false,
            .logicalModemIds = {},
    };
    for (int i = 0; i < n; i++) cap.logicalModemIds.push_back(static_cast<int8_t>(i));
    respond()->getPhoneCapabilityResponse(noError(serial), cap);
    return ok();
}

ScopedAStatus A6lRadioConfig::setNumOfLiveModems(int32_t serial, int8_t numOfLiveModems) {
    LOG(INFO) << "[" << serial << "] Config.setNumOfLiveModems " << int(numOfLiveModems);
    // r5 round5 F42: no single<->multi SIM transition exists in this HAL; only the current count is acknowledged
    switch (policy::liveModems(numOfLiveModems, ModemCore::slotCount())) {
        case policy::Verdict::Accept: respond()->setNumOfLiveModemsResponse(noError(serial)); break;
        case policy::Verdict::InvalidArguments:
            respond()->setNumOfLiveModemsResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
            break;
        case policy::Verdict::NotSupported:
            LOG(WARNING) << "[" << serial << "] Config.setNumOfLiveModems " << int(numOfLiveModems)
                         << ": switching the live modem count is not implemented (slots=" << ModemCore::slotCount() << ")";
            respond()->setNumOfLiveModemsResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED));
            break;
    }
    return ok();
}

// Default data subscription (DDS). Single SIM: modem 0 only, nothing sent (v1 behaviour).
ScopedAStatus A6lRadioConfig::setPreferredDataModem(int32_t serial, int8_t modemId) {
    const int n = ModemCore::slotCount();
    if (modemId < 0 || modemId >= n) {
        respond()->setPreferredDataModemResponse(errorResponse(serial, RadioError::INVALID_ARGUMENTS));
        return ok();
    }
    if (n == 1) {
        respond()->setPreferredDataModemResponse(noError(serial));
        return ok();
    }
    auto& core = ModemCore::get(1);
    if (!core.ready()) {
        respond()->setPreferredDataModemResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
        return ok();
    }
    auto r = qmi::multisim::setDefaultDataSub(core.ctl(), static_cast<uint8_t>(modemId));
    qmi::multisim::DualStandbyPref p;
    auto rg = qmi::multisim::getDualStandbyPref(core.ctl(), &p);
    LOG(INFO) << "[" << serial << "] Config.setPreferredDataModem " << int(modemId) << ": " << r.describe()
              << " (readback " << rg.describe() << " dds=" << (p.defaultDataSubs ? int(*p.defaultDataSubs) : -1) << ")";
    // NO_EFFECT = already the DDS
    bool okR = r.ok() || (r.status == qmi::Result::QmiFailure && r.qmiError == qmi::kErrNoEffect);
    respond()->setPreferredDataModemResponse(okR ? noError(serial) : errorResponse(serial, qmiToRadioError(r)));
    return ok();
}

// ======================================================================== modem
A6lRadioModem::A6lRadioModem(std::shared_ptr<minimal::SlotContext> context)
    : minimal::RadioModem(context, {RadioTechnology::GSM, RadioTechnology::GPRS, RadioTechnology::EDGE,
                                    RadioTechnology::UMTS, RadioTechnology::HSDPA, RadioTechnology::HSUPA,
                                    RadioTechnology::HSPA, RadioTechnology::HSPAP, RadioTechnology::LTE}) {}

void A6lRadioModem::onUpdatedResponseFunctions() {
    auto& core = slotCore();
    indicate()->rilConnected(RadioIndicationType::UNSOLICITED);
    indicate()->radioStateChanged(
            RadioIndicationType::UNSOLICITED,
            !core.ready() ? aidlModem::RadioState::UNAVAILABLE
                          : core.radioOn() ? aidlModem::RadioState::ON : aidlModem::RadioState::OFF);
}

ScopedAStatus A6lRadioModem::getImei(int32_t serial) {
    auto ids = slotCore().ids();  // slot 2: DMS bound to the secondary subscription -> 2nd IMEI
    if (ids.imei.empty()) {
        respond()->getImeiResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    aidlModem::ImeiInfo info{
            .type = slotCore().sub() == 0 ? aidlModem::ImeiInfo::ImeiType::PRIMARY
                                          : aidlModem::ImeiInfo::ImeiType::SECONDARY,
            .imei = ids.imei,
            .svn = ids.imeisv,
    };
    respond()->getImeiResponse(noError(serial), info);
    return ok();
}

ScopedAStatus A6lRadioModem::getBasebandVersion(int32_t serial) {
    auto rev = slotCore().revision();
    respond()->getBasebandVersionResponse(noError(serial), rev.empty() ? "a6l-qmi (modem down)" : rev);
    return ok();
}

ScopedAStatus A6lRadioModem::setRadioPower(int32_t serial, bool powerOn, bool forEmergencyCall,
                                           bool preferredForEmergencyCall) {
    LOG(INFO) << "[" << serial << "] Modem.setRadioPower " << powerOn << " emergency=" << forEmergencyCall
              << "/" << preferredForEmergencyCall;
    auto& core = slotCore();
    if (!core.ready()) {
        respond()->setRadioPowerResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE));
        return ok();
    }
    bool okp = core.setRadioPower(powerOn);
    respond()->setRadioPowerResponse(okp ? noError(serial) : errorResponse(serial, RadioError::MODEM_ERR));
    return ok();
}

void A6lRadioModem::onModemReady() {
    auto& core = slotCore();
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

// ril3: per slot. Slot 1 = v1 sessions (card slot 1 / primary GW / non-prov slot 1); slot 2 uses
// the card of its subscription (card slot 2 = 7) and the secondary GW provisioning session (2).
uim::Session A6lRadioSim::sessionFor(const std::string& aidHex) {
    auto sim = slotCore().slotSim();
    if (aidHex.empty()) return uim::Session{sim.cardSession, {}};
    auto aid = qmi::unhex(aidHex).value_or(std::vector<uint8_t>{});
    if (sim.provisioned && sim.gwAid == aid) return uim::Session{sim.provSession, {}};
    return uim::Session{sim.nonProvSession, aid};
}

ScopedAStatus A6lRadioSim::getIccCardStatus(int32_t serial) {
    auto& core = slotCore();
    if (!core.ready()) {
        respond()->getIccCardStatusResponse(errorResponse(serial, RadioError::RADIO_NOT_AVAILABLE), {});
        return ok();
    }
    auto sim = core.slotSim();
    aidlSim::CardStatus out{};
    out.cardState = aidlSim::CardStatus::STATE_ABSENT;
    out.universalPinState = aidlSim::PinState::UNKNOWN;
    out.gsmUmtsSubscriptionAppIndex = -1;
    out.cdmaSubscriptionAppIndex = -1;
    out.imsSubscriptionAppIndex = -1;
    out.slotMap = {.physicalSlotId = sim.card >= 0 ? sim.card : core.sub(), .portId = 0};
    const uim::Card* card = sim.cardInfo ? &*sim.cardInfo : nullptr;
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
        out.gsmUmtsSubscriptionAppIndex = sim.gwAppIndex;  // provisioned index, else 1st USIM/SIM
        if (card->state == uim::kCardPresent) out.iccid = core.iccid();
    }
    LOG(INFO) << "[" << serial << "] Sim.getIccCardStatus slot" << core.sub() + 1 << " card=" << sim.card
              << " state=" << out.cardState
              << " apps=" << out.applications.size() << " gw=" << out.gsmUmtsSubscriptionAppIndex;
    respond()->getIccCardStatusResponse(noError(serial), out);
    return ok();
}

ScopedAStatus A6lRadioSim::getImsiForApp(int32_t serial, const std::string& aid) {
    auto& core = slotCore();
    std::string imsi;
    auto sess = sessionFor(aid);
    auto appAid = sess.type == uim::kSessionPrimaryGw || sess.type == qmi::multisim::kSessionSecondaryGw
                          ? std::vector<uint8_t>{}
                          : qmi::unhex(aid).value_or(std::vector<uint8_t>{});
    auto r = core.sub() == 0 ? uim::readImsi(core.ctl(), appAid, &imsi)  // v1 path
                             : uim::readImsi(core.ctl(), sess.type, appAid, &imsi, core.bound());
    LOG(INFO) << "[" << serial << "] Sim.getImsiForApp " << r.describe()
              << " mccmnc=" << imsi.substr(0, imsi.size() > 5 ? 5 : imsi.size());
    respond()->getImsiForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)), imsi);
    return ok();
}

ScopedAStatus A6lRadioSim::iccIoForApp(int32_t serial, const aidlSim::IccIo& io) {
    auto& core = slotCore();
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

// pinsafe (27 Sep 2026): the PIN reaches the card only when the card itself says PIN required with >= 1
// attempt left (after activating the provisioning session the modem sometimes leaves inactive). Every
// other case is answered here without spending an attempt: no card -> SIM_ABSENT, session/app not
// ready for a PIN -> INVALID_SIM_STATE, blocked / 0 attempts -> PASSWORD_INCORRECT with 0 left (Android
// then asks for the PUK), bad format -> INVALID_ARGUMENTS, the same PIN the card already rejected ->
// PASSWORD_INCORRECT (never re-sent automatically). Already unlocked -> NONE (nothing sent).
ScopedAStatus A6lRadioSim::supplyIccPinForApp(int32_t serial, const std::string& pin, const std::string& aid) {
    auto& core = slotCore();
    auto chk = core.pinPreflight(pin);
    using V = ModemCore::PinVerdict;
    if (chk.verdict != V::Send) {
        RadioError e = RadioError::INVALID_SIM_STATE;
        switch (chk.verdict) {
            case V::NoCard: e = RadioError::SIM_ABSENT; break;
            case V::Blocked:
            case V::RepeatedWrong: e = RadioError::PASSWORD_INCORRECT; break;
            case V::BadFormat: e = RadioError::INVALID_ARGUMENTS; break;
            case V::AlreadyReady: e = RadioError::NONE; break;
            default: break;
        }
        LOG(WARNING) << "[" << serial << "] Sim.supplyIccPin NOT sent: " << chk.why << " -> RadioError " << static_cast<int>(e)
                     << " left=" << chk.retries;
        respond()->supplyIccPinForAppResponse(e == RadioError::NONE ? noError(serial) : errorResponse(serial, e),
                                              chk.retries < 0 ? -1 : chk.retries);
        core.post([&core] { core.cardStatus(true); });
        return ok();
    }
    uim::PinResult pr;
    auto r = uim::verifyPin(core.ctl(), sessionFor(aid), chk.pinId, pin, &pr);  // r5 round5 F44: PIN1 or UPIN
    LOG(INFO) << "[" << serial << "] Sim.supplyIccPin pin_id=" << int(chk.pinId) << " " << r.describe()
              << " left=" << pr.verifyLeft
              << " (before " << chk.retries << ")";
    if (r.ok() || (r.status == qmi::Result::QmiFailure && r.qmiError == qmi::kErrIncorrectPin))
        core.notePinResult(pin, r.ok());
    respond()->supplyIccPinForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                          pr.verifyLeft);
    return ok();
}

// r5 round5 F44: PUK / change PIN / SC lock address the app's effective PIN (UPIN when it replaces PIN1), resolved
// from a fresh card status; no card / unknown app -> answered here, nothing sent. Nothing is ever sent automatically.
std::optional<uint8_t> A6lRadioSim::pinIdOrError(const std::string& aid, RadioError* err) {
    auto& core = slotCore();
    auto id = core.pinTarget(aid);
    if (!id) {
        auto sim = core.slotSim();
        *err = sim.cardInfo && sim.cardInfo->state == uim::kCardPresent ? RadioError::INVALID_ARGUMENTS
                                                                          : RadioError::SIM_ABSENT;
    }
    return id;
}

ScopedAStatus A6lRadioSim::supplyIccPukForApp(int32_t serial, const std::string& puk, const std::string& pin,
                                              const std::string& aid) {
    RadioError e = RadioError::NONE;
    auto pinId = pinIdOrError(aid, &e);
    if (!pinId) {
        LOG(WARNING) << "[" << serial << "] Sim.supplyIccPuk NOT sent: no card / app " << aid;
        respond()->supplyIccPukForAppResponse(errorResponse(serial, e), -1);
        return ok();
    }
    uim::PinResult pr;
    auto r = uim::unblockPin(slotCore().ctl(), sessionFor(aid), *pinId, puk, pin, &pr);
    LOG(INFO) << "[" << serial << "] Sim.supplyIccPuk " << r.describe() << " left=" << pr.unblockLeft;
    // r5 bug hunt round2 R2: the card now has `pin` as its PIN. A PIN the card rejected before the block (typically
    // the very value the user re-chooses) must not stay in the repeated-wrong-PIN guard, which would refuse it forever.
    if (r.ok()) slotCore().notePinResult(pin, true);
    respond()->supplyIccPukForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                          pr.unblockLeft);
    return ok();
}

ScopedAStatus A6lRadioSim::changeIccPinForApp(int32_t serial, const std::string& oldPin,
                                              const std::string& newPin, const std::string& aid) {
    RadioError e = RadioError::NONE;
    auto pinId = pinIdOrError(aid, &e);
    if (!pinId) {
        respond()->changeIccPinForAppResponse(errorResponse(serial, e), -1);
        return ok();
    }
    uim::PinResult pr;
    auto r = uim::changePin(slotCore().ctl(), sessionFor(aid), *pinId, oldPin, newPin, &pr);
    if (r.ok()) slotCore().notePinResult(newPin, true);  // r5 bug hunt round2 R2: newPin is the card's PIN now
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
    // r5 round5 F48: only SC is answered; FD (FDN) is not invented as "disabled" (its state is never queried)
    if (policy::facilityQuery(facility) == policy::FacilityQuery::SimPinLock) {
        auto sim = slotCore().slotSim(true);
        const uim::App* a = sim.cardInfo && sim.gwAppIndex >= 0 &&
                                            sim.gwAppIndex < static_cast<int>(sim.cardInfo->apps.size())
                                    ? &sim.cardInfo->apps[sim.gwAppIndex]
                                    : nullptr;
        if (!a) {
            respond()->getFacilityLockForAppResponse(errorResponse(serial, RadioError::SIM_ABSENT), 0);
            return ok();
        }
        uint8_t st = a->upinReplacesPin1 ? sim.cardInfo->upinState : a->pin1State;
        locked = (st == uim::kPinEnabledNotVerified || st == uim::kPinEnabledVerified ||
                  st == uim::kPinBlocked || st == uim::kPinPermBlocked) ? 1 : 0;
    } else {
        LOG(DEBUG) << "[" << serial << "] Sim.getFacilityLockForApp " << facility << ": not supported";
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
    RadioError e = RadioError::NONE;
    auto pinId = pinIdOrError(appId, &e);
    if (!pinId) {
        respond()->setFacilityLockForAppResponse(errorResponse(serial, e), -1);
        return ok();
    }
    uim::PinResult pr;
    auto r = uim::setPinProtection(slotCore().ctl(), sessionFor(appId), *pinId, lockState, passwd, &pr);
    if (r.ok()) slotCore().notePinResult(passwd, true);  // r5 bug hunt round2 R2: the card accepted this PIN
    respond()->setFacilityLockForAppResponse(r.ok() ? noError(serial) : errorResponse(serial, qmiToRadioError(r)),
                                             pr.verifyLeft);
    return ok();
}

// r5 round5 F42: UICC applications can not be disabled by this HAL (no deprovisioning / registration block that
// survives power cycles): disabling is REQUEST_NOT_SUPPORTED, and the getter reports the applied state (enabled).
// The answer does not depend on the modem being up (the state is static), so GsmCdmaPhone always learns it.
ScopedAStatus A6lRadioSim::enableUiccApplications(int32_t serial, bool enable) {
    if (policy::uiccApplications(enable) != policy::Verdict::Accept) {
        LOG(WARNING) << "[" << serial << "] Sim.enableUiccApplications(false): not supported, applications stay enabled";
        respond()->enableUiccApplicationsResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED));
        return ok();
    }
    respond()->enableUiccApplicationsResponse(noError(serial));
    return ok();
}

ScopedAStatus A6lRadioSim::areUiccApplicationsEnabled(int32_t serial) {
    respond()->areUiccApplicationsEnabledResponse(noError(serial), true);
    return ok();
}

// r5 round5 F47: logical channels / APDUs must reach the real card or fail honestly. The inherited libminradio
// versions answer from an in-memory SIM emulator (dummy EFs, no real AIDs) and hit fatal CHECKs on ordinary input
// (READ BINARY offset != 0) that would abort the whole radio service. Until UIM Open Logical Channel (0x0042) /
// Send APDU (0x003B) are wired and tested on the phone, all four answer REQUEST_NOT_SUPPORTED without any SIM access.
ScopedAStatus A6lRadioSim::iccOpenLogicalChannel(int32_t serial, const std::string& aid, int32_t p2) {
    LOG(DEBUG) << "[" << serial << "] Sim.iccOpenLogicalChannel " << aid << " p2=" << p2 << ": not supported";
    respond()->iccOpenLogicalChannelResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED), 0, {});
    return ok();
}

ScopedAStatus A6lRadioSim::iccCloseLogicalChannelWithSessionInfo(int32_t serial, const aidlSim::SessionInfo& info) {
    LOG(DEBUG) << "[" << serial << "] Sim.iccCloseLogicalChannel " << info.sessionId << ": not supported";
    respond()->iccCloseLogicalChannelWithSessionInfoResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED));
    return ok();
}

ScopedAStatus A6lRadioSim::iccTransmitApduBasicChannel(int32_t serial, const aidlSim::SimApdu&) {
    LOG(DEBUG) << "[" << serial << "] Sim.iccTransmitApduBasicChannel: not supported";
    respond()->iccTransmitApduBasicChannelResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED), {});
    return ok();
}

ScopedAStatus A6lRadioSim::iccTransmitApduLogicalChannel(int32_t serial, const aidlSim::SimApdu&) {
    LOG(DEBUG) << "[" << serial << "] Sim.iccTransmitApduLogicalChannel: not supported";
    respond()->iccTransmitApduLogicalChannelResponse(errorResponse(serial, RadioError::REQUEST_NOT_SUPPORTED), {});
    return ok();
}

void A6lRadioSim::onModemReady() { indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioSim::onModemLost() { indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED); }
void A6lRadioSim::onSimChanged() { indicate()->simStatusChanged(RadioIndicationType::UNSOLICITED); }

}  // namespace android::hardware::radio::a6l
