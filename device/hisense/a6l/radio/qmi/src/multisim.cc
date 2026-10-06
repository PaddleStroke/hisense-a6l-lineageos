// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril3, 25 Sep 2026): dual SIM dual standby helpers (see multisim.h).
#include <a6lqmi/log.h>
#include <a6lqmi/multisim.h>

namespace a6l::qmi::multisim {

namespace {
Message req(uint16_t id) { return Message::request(id); }

int gwAppIn(const uim::Card& c) {
    // same rule as the v1 HAL's getIccCardStatus: the first USIM or SIM app
    for (size_t i = 0; i < c.apps.size(); i++)
        if (c.apps[i].type == uim::kAppUsim || c.apps[i].type == uim::kAppSim) return static_cast<int>(i);
    return -1;
}
bool validIndex(const uim::CardStatus& cs, uint16_t idx) {
    if (idx == 0xFFFF) return false;
    unsigned ci = idx >> 8, ai = idx & 0xff;
    return ci < cs.cards.size() && ai < cs.cards[ci].apps.size();
}
}  // namespace

SlotView viewFor(const uim::CardStatus& cs, int sub) {
    SlotView v;
    v.sub = sub;
    v.provSession = sub == 0 ? kSessionPrimaryGw : kSessionSecondaryGw;
    const uint16_t mine = sub == 0 ? cs.indexGwPrimary : cs.indexGwSecondary;
    const uint16_t other = sub == 0 ? cs.indexGwSecondary : cs.indexGwPrimary;
    if (validIndex(cs, mine)) {
        v.card = mine >> 8;
        v.gwAppIndex = mine & 0xff;
        v.provisioned = true;
    } else if (sub == 0) {
        // single-SIM behaviour of the v1 HAL: primaryCard() = card 0 when nothing is provisioned
        if (!cs.cards.empty()) v.card = 0;
        if (validIndex(cs, other) && (other >> 8) == 0 && cs.cards.size() > 1) v.card = 1;
    } else {
        int usedByOther = validIndex(cs, other) ? (other >> 8) : 0;
        int want = usedByOther == 1 ? 0 : 1;
        if (want < static_cast<int>(cs.cards.size())) v.card = want;
    }
    if (v.card >= 0) {
        v.cardPtr = &cs.cards[v.card];
        if (v.gwAppIndex < 0) v.gwAppIndex = gwAppIn(*v.cardPtr);
        if (v.gwAppIndex >= 0) v.gwApp = &v.cardPtr->apps[v.gwAppIndex];
        v.cardSession = v.card == 0 ? kSessionCardSlot1 : kSessionCardSlot2;
        v.nonProvSession = v.card == 0 ? kSessionNonProvSlot1 : kSessionNonProvSlot2;
    }
    return v;
}

std::optional<std::pair<uint8_t, std::vector<uint8_t>>> provisioningCandidate(
        const uim::CardStatus& cs, int sub) {
    SlotView v = viewFor(cs, sub);
    if (v.provisioned || !v.present() || !v.gwApp) return std::nullopt;
    // Only an app the card has fully detected (state >= DETECTED) can be activated.
    if (v.gwApp->state == uim::kAppStateUnknown) return std::nullopt;
    return std::make_pair(static_cast<uint8_t>(v.card + 1), v.gwApp->aid);
}

Message buildChangeProvisioning(uint8_t session, bool activate, uint8_t slot1Based,
                                const std::vector<uint8_t>& aid) {
    Message m = req(kUimChangeProvisioningSession);
    m.raw(0x01, {session, static_cast<uint8_t>(activate ? 1 : 0)});
    if (activate) {
        std::vector<uint8_t> app{slot1Based, static_cast<uint8_t>(aid.size())};
        app.insert(app.end(), aid.begin(), aid.end());
        m.raw(0x10, app);
    }
    return m;
}

Result changeProvisioning(Client& c, uint8_t session, bool activate, uint8_t slot1Based,
                          const std::vector<uint8_t>& aid) {
    return c.request(kSvcUim, buildChangeProvisioning(session, activate, slot1Based, aid), 15000);
}

Result nasBind(Client& c, uint8_t sub) { return c.request(kSvcNas, req(kNasBindSubscription).u8(0x01, sub)); }
Result wmsBind(Client& c, uint8_t sub) { return wms::bindSubscription(c, sub); }
Result voiceBind(Client& c, uint8_t sub) {
    return c.request(kSvcVoice, req(kVoiceBindSubscription).u8(0x01, sub));
}
Result dmsBind(Client& c, uint8_t sub) {
    return c.request(kSvcDms, req(kDmsBindSubscription).u32(0x01, sub + 1u));
}
Result wdsBind(Client& c, uint8_t sub) {
    return c.request(kSvcWds, req(kWdsBindSubscription).u32(0x01, sub + 1u));
}

Result bindAll(Client& c, int sub, bool withVoice, std::string* log) {
    Result first;
    first.status = Result::Ok;
    if (sub == 0) {
        if (log) *log = "primary (default binding, nothing sent)";
        return first;
    }
    auto s = static_cast<uint8_t>(sub);
    struct Step {
        const char* name;
        Result (*fn)(Client&, uint8_t);
    };
    const Step steps[] = {{"nas", nasBind}, {"wms", wmsBind}, {"dms", dmsBind}, {"voice", voiceBind}};
    for (const auto& st : steps) {
        if (!withVoice && st.fn == voiceBind) continue;
        Result r = st.fn(c, s);
        if (log) *log += std::string(log->empty() ? "" : " ") + st.name + "=" + r.describe();
        if (!r.ok() && first.ok()) first = r;
    }
    return first;
}

DualStandbyPref parseDualStandbyPref(const Message& m) {
    DualStandbyPref p;
    auto u8 = [&](uint8_t t, std::optional<uint8_t>* o) {
        if (auto* v = m.get(t); v && !v->empty()) *o = (*v)[0];
    };
    u8(0x10, &p.standbyPref);
    u8(0x11, &p.prioritySubs);
    u8(0x12, &p.activeSubs);
    u8(0x13, &p.defaultDataSubs);
    u8(0x14, &p.defaultVoiceSubs);
    if (auto* v = m.get(0x15); v && v->size() >= 8) p.activeSubsMask = Reader(*v).u64();
    return p;
}

Result getDualStandbyPref(Client& c, DualStandbyPref* out) {
    auto r = c.request(kSvcNas, req(kNasGetDualStandbyPref));
    if (r.ok()) *out = parseDualStandbyPref(r.msg);
    return r;
}

Message buildSetDefaultDataSub(uint8_t sub) { return req(kNasSetDualStandbyPref).u8(0x12, sub); }

Result setDefaultDataSub(Client& c, uint8_t sub) { return c.request(kSvcNas, buildSetDefaultDataSub(sub), 10000); }

bool PowerVote::vote(int sub, bool on) {
    std::lock_guard<std::mutex> l(mLock);
    if (sub >= 0 && sub < kMaxSlots) {
        mOn[sub] = on;
        mVoted[sub] = true;
    }
    for (int i = 0; i < kMaxSlots; i++)
        if (mVoted[i] && mOn[i]) return true;
    return false;
}
bool PowerVote::preview(int sub, bool on) const {
    std::lock_guard<std::mutex> l(mLock);
    for (int i = 0; i < kMaxSlots; i++) {
        bool voted = i == sub ? true : mVoted[i];
        bool v = i == sub ? on : mOn[i];
        if (voted && v) return true;
    }
    return false;
}
bool PowerVote::wants(int sub) const {
    std::lock_guard<std::mutex> l(mLock);
    return sub >= 0 && sub < kMaxSlots && mOn[sub];
}
bool PowerVote::any() const {
    std::lock_guard<std::mutex> l(mLock);
    for (int i = 0; i < kMaxSlots; i++)
        if (mVoted[i] && mOn[i]) return true;
    return false;
}
void PowerVote::reset() {
    std::lock_guard<std::mutex> l(mLock);
    for (int i = 0; i < kMaxSlots; i++) {
        mOn[i] = true;
        mVoted[i] = false;
    }
}

int slotCountFromConfig(const std::string& cfg, int override) {
    if (override == 1 || override == 2) return override;
    return (cfg == "dsds" || cfg == "dsda") ? 2 : 1;
}

}  // namespace a6l::qmi::multisim
