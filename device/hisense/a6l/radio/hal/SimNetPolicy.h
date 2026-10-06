// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL, r5 review round5 fixes (28 Sep 2026): pure decisions behind IRadioConfig / IRadioSim /
// IRadioNetwork answers that must describe what the modem actually applied. No AIDL / Android dependency so they
// are host-tested (tests/review5c_tests.cc); the HAL static_asserts the RadioAccessFamily bits against AIDL.
//  F42 live modems / UICC applications: only states the HAL really establishes are acknowledged
//  F45 allowed network types: never broadened; an unrepresentable request is rejected, the applied subset cached
//  F46 manual selection: exactly 5/6 decimal digits, the MNC width travels to NAS (TLV 0x12)
//  F48 facility lock query: FD (FDN) is not invented as "disabled"
#pragma once

#include <a6lqmi/services.h>

#include <cstdint>
#include <optional>
#include <string>

namespace android::hardware::radio::a6l::policy {

// ---------------------------------------------------------------- F42
enum class Verdict { Accept, InvalidArguments, NotSupported };

// IRadioConfig.setNumOfLiveModems. The HAL has no single<->multi SIM transition (the slot count is fixed at boot
// from persist.radio.multisim.config / ro.vendor.a6l.ril.slots): the current count is a no-op success, any other
// valid count is REQUEST_NOT_SUPPORTED (the framework then keeps its phone count: "Not switching multi-sim config").
inline Verdict liveModems(int requested, int slots) {
    if (requested < 1 || requested > slots) return Verdict::InvalidArguments;
    return requested == slots ? Verdict::Accept : Verdict::NotSupported;
}

// IRadioSim.enableUiccApplications. Disabling (modem must stop registering on that SIM, remembered across power
// cycles) is not implemented: REQUEST_NOT_SUPPORTED, nothing changes. Enabling = the only applied state: success.
// areUiccApplicationsEnabled therefore always reports true (the framework does not retry on REQUEST_NOT_SUPPORTED).
inline Verdict uiccApplications(bool enable) { return enable ? Verdict::Accept : Verdict::NotSupported; }

// ---------------------------------------------------------------- F45
// RadioAccessFamily bits (1 << RadioTechnology), checked against AIDL in RadioNetworkData.cpp
namespace raf {
constexpr int32_t kGprs = 1 << 1, kEdge = 1 << 2, kUmts = 1 << 3, kHsdpa = 1 << 9, kHsupa = 1 << 10,
                  kHspa = 1 << 11, kLte = 1 << 14, kHspap = 1 << 15, kGsm = 1 << 16, kLteCa = 1 << 19;
constexpr int32_t kGsmFamily = kGsm | kGprs | kEdge;
constexpr int32_t kUmtsFamily = kUmts | kHsdpa | kHsupa | kHspa | kHspap;
constexpr int32_t kLteFamily = kLte | kLteCa;
constexpr int32_t kSupported = kGsmFamily | kUmtsFamily | kLteFamily;
}  // namespace raf

struct AllowedModes {
    uint16_t modeMask = 0;  // NAS RAT mode preference (QmiNasRatModePreference) to send
    int32_t applied = 0;    // the requested bits the modem can honour (what the getter reports)
};
// nullopt: no requested family is supported by this modem (empty bitmap, NR-only, CDMA-only, ...). The request must
// be rejected without touching the modem (IRadioNetwork: *only* the given types; never broadened to all).
inline std::optional<AllowedModes> allowedModes(int32_t bm) {
    AllowedModes m;
    if (bm & raf::kGsmFamily) m.modeMask |= ::a6l::qmi::nas::kModeGsm;
    if (bm & raf::kUmtsFamily) m.modeMask |= ::a6l::qmi::nas::kModeUmts;
    if (bm & raf::kLteFamily) m.modeMask |= ::a6l::qmi::nas::kModeLte;
    if (m.modeMask == 0) return std::nullopt;
    m.applied = bm & raf::kSupported;
    return m;
}
// Getter fallback: modem mode preference -> bitmap (used when nothing was applied by this HAL instance)
inline int32_t bitmapForModes(uint16_t mm) {
    int32_t bm = 0;
    if (mm & ::a6l::qmi::nas::kModeGsm) bm |= raf::kGsmFamily;
    if (mm & ::a6l::qmi::nas::kModeUmts) bm |= raf::kUmtsFamily;
    if (mm & ::a6l::qmi::nas::kModeLte) bm |= raf::kLteFamily;
    return bm;
}

// ---------------------------------------------------------------- F46
struct Plmn {
    uint16_t mcc = 0, mnc = 0;
    bool mncThreeDigits = false;
};
// "MCCMNC": exactly 5 or 6 decimal digits (3GPP TS 23.003: MCC 3 digits, MNC 2 or 3 digits)
inline std::optional<Plmn> parsePlmn(const std::string& s) {
    if ((s.size() != 5 && s.size() != 6) || s.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
    Plmn p;
    p.mcc = static_cast<uint16_t>(std::stoi(s.substr(0, 3)));
    p.mnc = static_cast<uint16_t>(std::stoi(s.substr(3)));
    p.mncThreeDigits = s.size() == 6;
    return p;
}

// ---------------------------------------------------------------- F48
enum class FacilityQuery { SimPinLock, NotSupported };
// Only "SC" (SIM PIN lock) is answered from the card status. "FD" (FDN) needs the SIM's FDN service state, which the
// HAL does not query: REQUEST_NOT_SUPPORTED instead of a made-up "disabled". Call barring etc: not supported either.
inline FacilityQuery facilityQuery(const std::string& facility) {
    return facility == "SC" ? FacilityQuery::SimPinLock : FacilityQuery::NotSupported;
}

}  // namespace android::hardware::radio::a6l::policy
