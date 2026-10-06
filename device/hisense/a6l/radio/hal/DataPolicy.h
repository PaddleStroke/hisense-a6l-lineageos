// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL, r5 review fix F9 (28 Sep 2026): data roaming permission (IRadioData.setupDataCall
// roamingAllowed = the user's data-roaming setting). Pure decision on the NAS serving system, host-tested
// (tests/review5_tests.cc). The HAL re-reads the serving system on its data executor right before WDS Start
// Network, so a registration change between the binder call and the setup is honoured.
#pragma once

#include <a6lqmi/services.h>

#include <optional>

namespace android::hardware::radio::a6l {

enum class RoamingVerdict {
    Allow,          // home network, roaming permitted, or an emergency PDN
    RejectRoaming,  // serving network says roaming and the user did not allow data roaming
};

// Roaming is decided from NAS serving system TLV 0x10 (roaming indicator). Unknown (no serving system / no
// indicator) is treated as home: the modem only reports the indicator once registered, and an unregistered
// setup fails in the modem anyway. Emergency PDNs are never blocked by the roaming setting (3GPP TS 23.401).
inline bool servingIsRoaming(const std::optional<::a6l::qmi::nas::ServingSystem>& s) {
    return s && s->roaming && *s->roaming;
}

inline RoamingVerdict checkDataRoaming(const std::optional<::a6l::qmi::nas::ServingSystem>& s, bool roamingAllowed,
                                       bool emergency) {
    if (emergency || roamingAllowed) return RoamingVerdict::Allow;
    return servingIsRoaming(s) ? RoamingVerdict::RejectRoaming : RoamingVerdict::Allow;
}

// AIDL DataCallFailCause::DATA_ROAMING_SETTINGS_DISABLED ("PDN connection ... disallowed because data roaming is
// disabled from the device user interface settings and the UE is roaming").
constexpr int32_t kFailCauseDataRoamingSettingsDisabled = 0x810;

}  // namespace android::hardware::radio::a6l
