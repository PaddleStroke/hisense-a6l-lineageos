// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril3, 25 Sep 2026): dual SIM dual standby (DSDS) helpers.
//
// Stock runs the SDM660 modem in DSDS (persist.radio.multisim.config=dsds). On a Qualcomm modem a
// QMI client talks to the PRIMARY subscription unless it binds itself to another one, so the HAL
// uses one QRTR socket (= one QMI client per service) per logical slot and binds NAS, WMS, VOICE,
// DMS (and the WDS data clients) of slot 2 to the secondary subscription. UIM has no binding: it
// addresses subscriptions through session types (primary / secondary GW provisioning) and slots.
//
// Message ids and TLV ids/sizes below were read from the stock IDL tables in
// vendor/lib64/libqmiservices.so (A6L stock firmware; decoder: docs/ril3-20260925.md §2) and match
// the Gobi API 2013 message list; subscription enum values follow Qualcomm/libqmi conventions:
//   NAS  0x0045 Bind Subscription         TLV 0x01 enum8   0 primary, 1 secondary
//   WMS  0x004C Bind Subscription         TLV 0x01 enum8   0 primary, 1 secondary
//   VOICE 0x0044 Bind Subscription        TLV 0x01 enum8   0 primary, 1 secondary
//   DMS  0x0054 Bind Subscription         TLV 0x01 u32     1 primary, 2 secondary
//   WDS  0x00AF Bind Subscription         TLV 0x01 u32     1 primary, 2 secondary (libqmi)
//   UIM  0x0038 Change Provisioning Session  0x01 {session u8, activate u8}, 0x10 {slot u8, aid u8-len}
//   NAS  0x004B Set Dual Standby Pref     0x10..0x13 enum8, 0x14 u64 mask, 0x15 enum8
//   NAS  0x005C Get Dual Standby Pref     resp 0x10..0x14 enum8, 0x15 u64
#pragma once

#include <a6lqmi/services.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace a6l::qmi::multisim {

constexpr int kMaxSlots = 2;

enum : uint16_t {
    kVoiceBindSubscription = 0x0044,
    kNasBindSubscription = 0x0045,
    kNasSetDualStandbyPref = 0x004B,
    kNasGetDualStandbyPref = 0x005C,
    kDmsBindSubscription = 0x0054,
    kWdsBindSubscription = 0x00AF,
    kUimChangeProvisioningSession = 0x0038,
};
enum : uint8_t {
    kSessionPrimaryGw = 0,
    kSessionSecondaryGw = 2,
    kSessionNonProvSlot1 = 4,
    kSessionNonProvSlot2 = 5,
    kSessionCardSlot1 = 6,
    kSessionCardSlot2 = 7,
};

// Per logical slot view of UIM Get Card Status. Logical slot `sub` 0 = primary subscription
// (Android slot1), 1 = secondary (slot2). The physical card is the one the subscription is
// provisioned from (index >> 8), else card `sub` when that card exists, else the card the other
// subscription does not use.
struct SlotView {
    int sub = 0;
    int card = -1;                        // index into CardStatus::cards (0-based UIM slot), -1 none
    const uim::Card* cardPtr = nullptr;
    int gwAppIndex = -1;                  // app index inside the card, -1 none
    const uim::App* gwApp = nullptr;
    bool provisioned = false;             // the modem has a GW provisioning session for `sub`
    uint8_t provSession = kSessionPrimaryGw;
    uint8_t cardSession = kSessionCardSlot1;
    uint8_t nonProvSession = kSessionNonProvSlot1;
    bool present() const { return cardPtr && cardPtr->state == uim::kCardPresent; }
};
SlotView viewFor(const uim::CardStatus& cs, int sub);

// A USIM/SIM app that should be provisioned for `sub` but is not (secondary slot after a SIM swap,
// or a modem that does not auto-provision). Returns {uim slot (1-based), aid}.
std::optional<std::pair<uint8_t, std::vector<uint8_t>>> provisioningCandidate(
        const uim::CardStatus& cs, int sub);

Message buildChangeProvisioning(uint8_t session, bool activate, uint8_t slot1Based,
                                const std::vector<uint8_t>& aid);
Result changeProvisioning(Client& c, uint8_t session, bool activate, uint8_t slot1Based,
                          const std::vector<uint8_t>& aid);

// Binds every service this client uses to `sub` (no-op for sub 0: the default binding is primary,
// so single-SIM traffic is byte-for-byte unchanged). Returns the first failure; `log` gets one
// "svc=result" item per service.
Result bindAll(Client& c, int sub, bool withVoice, std::string* log = nullptr);
Result nasBind(Client& c, uint8_t sub);
Result wmsBind(Client& c, uint8_t sub);
Result voiceBind(Client& c, uint8_t sub);
Result dmsBind(Client& c, uint8_t sub);  // encodes sub + 1
Result wdsBind(Client& c, uint8_t sub);  // encodes sub + 1

struct DualStandbyPref {
    std::optional<uint8_t> standbyPref, prioritySubs, activeSubs, defaultDataSubs, defaultVoiceSubs;
    std::optional<uint64_t> activeSubsMask;
};
// Response layout of 0x005C (read-only; lets the attended test confirm the field order).
Result getDualStandbyPref(Client& c, DualStandbyPref* out);
DualStandbyPref parseDualStandbyPref(const Message& m);
// Default data subscription (IRadioConfig.setPreferredDataModem): Set Dual Standby Pref TLV 0x12.
// TLV id taken from the stock IDL (0x12 is the 3rd enum8 after standby_pref and priority_subs);
// the attended test reads 0x005C back before/after to confirm.
Message buildSetDefaultDataSub(uint8_t sub);
Result setDefaultDataSub(Client& c, uint8_t sub);

// Radio power across subscriptions: the modem has ONE operating mode (DMS). Airplane mode turns
// every slot off -> LOW_POWER; any slot on -> ONLINE. Per-slot OFF while the other is ON is kept
// as a HAL state (Android sees the slot OFF), the modem stays online.
class PowerVote {
  public:
    // Returns the operating mode the modem should be in after this vote.
    bool vote(int sub, bool on);
    // r5 review F18: the mode vote(sub, on) would give, without recording it (commit with vote() once applied).
    bool preview(int sub, bool on) const;
    bool wants(int sub) const;
    bool any() const;
    void reset();  // modem restart: unknown until Android votes again (defaults: on)

  private:
    mutable std::mutex mLock;
    bool mOn[kMaxSlots] = {true, true};
    bool mVoted[kMaxSlots] = {false, false};
};

// Active slot count from the product config ("dsds"/"dsda" -> 2, anything else -> 1).
int slotCountFromConfig(const std::string& multisimConfig, int override = 0);

}  // namespace a6l::qmi::multisim
