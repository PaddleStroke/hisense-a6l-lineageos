// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio HAL (telephony-flows, 29 Sep 2026): pure decisions for USSD sessions, the modem-config emergency
// number list and the "emergency calls only" registration state. No AIDL/binder types (host-tested in
// tests/telephony_flows_tests.cc); the HAL converts the plain values below to the AIDL ones (same numeric values).
#pragma once

#include <a6lqmi/ussd.h>

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace android::hardware::radio::a6l::flows {

// ------------------------------------------------------------------ USSD
// == aidl::android::hardware::radio::voice::UssdModeType
enum UssdMode : int32_t { kNotify = 0, kRequest = 1, kNwRelease = 2, kLocalClient = 3, kNotSupported = 4, kNwTimeout = 5 };
struct UssdReport {
    UssdMode mode = kNotify;
    std::string msg;
};

// One USSD dialogue per slot (3GPP TS 24.090). Thread-safe: requests run on HAL executors, indications on the
// ModemCore worker thread.
//   sendUssd  : idle -> Originate (No Wait; sync Originate only if the modem does not know No Wait)
//               network asked for an answer (USSD ind, user action REQUIRED) -> Answer USSD
//   onUssd    : USSD ind REQUIRED -> REQUEST (dialogue stays open), NOT_REQUIRED/UNKNOWN with text -> NOTIFY
//               (network-initiated notify or an intermediate message), without text -> nothing
//               release ind -> NW_RELEASE (dialogue closed)
//               originate result: error -> NOT_SUPPORTED (Android shows "USSD failed"), network reply text -> NOTIFY
//               (final answer to the origination, dialogue closed), nothing -> NW_RELEASE
//               modem lost with an open dialogue -> NOT_SUPPORTED (the pending MMI must not stay "running")
class UssdSession {
  public:
    enum class Send { Originate, Answer };
    Send nextSend() const {
        std::lock_guard<std::mutex> g(mLock);
        return mAwaitingAnswer ? Send::Answer : Send::Originate;
    }
    bool active() const {
        std::lock_guard<std::mutex> g(mLock);
        return mActive;
    }
    bool awaitingAnswer() const {
        std::lock_guard<std::mutex> g(mLock);
        return mAwaitingAnswer;
    }
    // an Originate / Answer is about to be sent (set before the request so an early indication finds it open)
    void sent(Send s) {
        std::lock_guard<std::mutex> g(mLock);
        mActive = true;
        if (s == Send::Answer) mAwaitingAnswer = false;
    }
    // the modem refused the request (nothing is in progress on our side any more for an Originate)
    void sendFailed(Send s) {
        std::lock_guard<std::mutex> g(mLock);
        if (s == Send::Originate) mActive = mAwaitingAnswer = false;
    }
    void cancelled() {
        std::lock_guard<std::mutex> g(mLock);
        mActive = mAwaitingAnswer = false;
    }
    std::optional<UssdReport> modemLost() {
        std::lock_guard<std::mutex> g(mLock);
        bool was = mActive;
        mActive = mAwaitingAnswer = false;
        if (!was) return std::nullopt;
        return UssdReport{kNotSupported, ""};
    }
    std::optional<UssdReport> onEvent(const ::a6l::qmi::voice::ussd::Event& e) {
        namespace u = ::a6l::qmi::voice::ussd;
        std::lock_guard<std::mutex> g(mLock);
        switch (e.kind) {
            case u::Event::Notification:
                if (e.userAction == u::kActionRequired) {
                    mActive = mAwaitingAnswer = true;
                    return UssdReport{kRequest, e.text.utf8};
                }
                mAwaitingAnswer = false;
                if (!e.text.present || e.text.decodeError) return std::nullopt;
                return UssdReport{kNotify, e.text.utf8};
            case u::Event::Released:
                mActive = mAwaitingAnswer = false;
                return UssdReport{kNwRelease, ""};
            case u::Event::OriginateResult:
                mActive = mAwaitingAnswer = false;
                if (e.error && *e.error != 0) return UssdReport{kNotSupported, ""};
                if (e.text.present && !e.text.decodeError) return UssdReport{kNotify, e.text.utf8};
                return UssdReport{kNwRelease, ""};
        }
        return std::nullopt;
    }

  private:
    mutable std::mutex mLock;
    bool mActive = false, mAwaitingAnswer = false;
};

// ------------------------------------------------------------------ emergency numbers
// == aidl::android::hardware::radio::voice::EmergencyServiceCategory bits
enum : int32_t { kCatUnspecified = 0, kCatPolice = 1, kCatAmbulance = 2, kCatFire = 4, kCatMarine = 8, kCatMountain = 16 };
struct EccEntry {
    std::string number, mcc;
    int32_t categories = kCatUnspecified;
    bool operator==(const EccEntry& o) const {
        return number == o.number && mcc == o.mcc && categories == o.categories;
    }
};
// The list reported as SOURCE_MODEM_CONFIG (the framework merges it with its ECC database and the SIM EF_ECC):
//  - always 112 and 911 (3GPP TS 22.101 10.1.1: with or without a SIM);
//  - no SIM (card absent or not yet known): + 000, 08, 110, 118, 119, 999 (TS 22.101 10.1.1, "no SIM" list);
//  - camped on a French network (serving MCC 208, also in limited service): + 15 SAMU, 17 police, 18 pompiers,
//    196 sea rescue, with the matching categories and mcc "208" (never reported abroad: "17"/"18" are not emergency
//    numbers elsewhere). 114 (deaf/hard of hearing, SMS/fax) and 115/119 stay with the framework database.
inline std::vector<EccEntry> emergencyNumbers(bool simAbsent, const std::string& servingMcc) {
    std::vector<EccEntry> l{{"112", "", kCatUnspecified}, {"911", "", kCatUnspecified}};
    if (simAbsent)
        for (const char* n : {"000", "08", "110", "118", "119", "999"}) l.push_back({n, "", kCatUnspecified});
    if (servingMcc == "208") {
        l.push_back({"15", "208", kCatAmbulance});
        l.push_back({"17", "208", kCatPolice});
        l.push_back({"18", "208", kCatFire});
        l.push_back({"196", "208", kCatMarine});
    }
    return l;
}

// ------------------------------------------------------------------ "emergency calls only"
// == aidl::android::hardware::radio::network::RegState
enum RegState : int32_t {
    kNotRegNotSearching = 0, kRegHome = 1, kNotRegSearching = 2, kRegDenied = 3, kRegUnknown = 4, kRegRoaming = 5,
    kNotRegNotSearchingEm = 10, kNotRegSearchingEm = 12, kRegDeniedEm = 13, kUnknownEm = 14,
};
// Voice registration only, and only when the modem is NOT registered (NAS registration state != registered) but
// camps on a cell (the serving system lists a radio interface: limited service, e.g. no SIM, PIN-locked SIM, or a
// network that rejected the SIM): Android must be told "emergency calls only" (the *_EM states) instead of plain
// "no service". With no radio interface (no cell at all) there is no EM variant either.
inline int32_t emergencyOnlyVariant(int32_t voiceRegState, bool campedOnCell) {
    if (!campedOnCell) return voiceRegState;
    switch (voiceRegState) {
        case kNotRegNotSearching: return kNotRegNotSearchingEm;
        case kNotRegSearching: return kNotRegSearchingEm;
        case kRegDenied: return kRegDeniedEm;
        case kRegUnknown: return kUnknownEm;
        default: return voiceRegState;
    }
}

}  // namespace android::hardware::radio::a6l::flows
