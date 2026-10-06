// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte5, 28 Sep 2026): IMSS/IMSA client setup, see a6lqmi/ims_setup.h.
#include <a6lqmi/ims_setup.h>
#include <a6lqmi/services.h>

#include <chrono>
#include <thread>

namespace a6l::radio {

using namespace a6l::qmi;

ImssMode imssModeFromString(const std::string& s) {
    if (s == "off" || s == "0") return ImssMode::Off;
    if (s == "enable" || s == "1") return ImssMode::Enable;
    if (s == "force") return ImssMode::Force;
    if (s == "toggle") return ImssMode::Toggle;
    return ImssMode::Read;
}
const char* imssModeName(ImssMode m) {
    switch (m) {
        case ImssMode::Off: return "off";
        case ImssMode::Read: return "read";
        case ImssMode::Enable: return "enable";
        case ImssMode::Force: return "force";
        case ImssMode::Toggle: return "toggle";
    }
    return "?";
}

bool ImssOutcome::enabled() const {
    const auto& c = wrote ? after : before;
    return (wrote ? readAfterOk : readOk) && c.imsService && *c.imsService == 1;
}

ImssOutcome imssSetup(Client& c, std::optional<uint32_t> sub, ImssMode mode, const ImsReportFn& report,
                      int toggleGapMs) {
    ImssOutcome o;
    auto rep = [&](const std::string& s) {
        if (report) report(s);
    };
    if (mode == ImssMode::Off) return o;
    if (sub) {
        auto r = imss::bindSubscription(c, *sub);
        o.bound = r.ok();
        rep("A6L_IMSDCM_IMSS bind sub=" + std::to_string(*sub) + ": " + r.describe());
        if (!o.bound) return o;  // Do not use an unbound/default subscription after a failed bind.
    }
    auto r = imss::getServiceEnableConfig(c, &o.before);
    o.readOk = r.ok();
    rep("A6L_IMSDCM_IMSS get " + (r.ok() ? o.before.summary() : r.describe()));
    if (mode == ImssMode::Read) return o;
    std::optional<bool> volte, ims;
    if (mode == ImssMode::Enable) {
        if (!o.readOk || !o.before.imsService || *o.before.imsService != 1) ims = true;
        if (!o.readOk || (o.before.volte && *o.before.volte != 1)) volte = true;
        if (!ims && !volte) {
            rep("A6L_IMSDCM_IMSS enable: already on (no write)");
            return o;
        }
    } else {  // Force / Toggle: stock ImsService re-sends after every SIM load, whatever the modem reports
        ims = true;
        volte = true;
        if (mode == ImssMode::Toggle) {
            auto r0 = imss::setServiceEnableConfig(c, std::nullopt, false);
            rep("A6L_IMSDCM_IMSS toggle set ims_service_enabled=0: " + r0.describe());
            if (toggleGapMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(toggleGapMs));
        }
    }
    o.wrote = true;
    r = imss::setServiceEnableConfig(c, volte, ims);
    o.writeOk = r.ok();
    rep(std::string("A6L_IMSDCM_IMSS set") + (volte ? " volte=1" : "") + (ims ? " ims_service_enabled=1" : "") +
        ": " + r.describe());
    r = imss::getServiceEnableConfig(c, &o.after);
    o.readAfterOk = r.ok();
    rep("A6L_IMSDCM_IMSS get-after " + (r.ok() ? o.after.summary() : r.describe()));
    return o;
}

ImsaOutcome imsaAttach(Client& c, std::optional<uint32_t> sub, const ImsReportFn& report) {
    ImsaOutcome o;
    auto rep = [&](const std::string& s) {
        if (report) report(s);
    };
    if (sub) {
        auto r = imsa::bindSubscription(c, *sub);
        o.bound = r.ok();
        rep("A6L_IMSDCM_IMSA bind sub=" + std::to_string(*sub) + ": " + r.describe());
        if (!o.bound) return o;  // Do not use an unbound/default subscription after a failed bind.
    }
    auto r = imsa::registerIndications(c);
    o.indOk = r.ok();
    rep("A6L_IMSDCM_IMSA register indications: " + r.describe());
    r = imsa::getRegStatus(c, &o.reg);
    o.regOk = r.ok();
    rep("A6L_IMSDCM_IMSA reg " + (r.ok() ? o.reg.summary() : r.describe()));
    r = imsa::getServicesStatus(c, &o.services);
    o.svcOk = r.ok();
    rep("A6L_IMSDCM_IMSA services " + (r.ok() ? o.services.summary() : r.describe()));
    return o;
}

// ---------------------------------------------------------------- volte6 NAS
const char* vdpName(uint32_t v) {
    switch (v) {
        case 0: return "cs-only";
        case 1: return "ps-only";
        case 2: return "cs-pref";
        case 3: return "ims-pref";
    }
    return "?";
}

static const char* usageName(uint32_t u) { return u == 1 ? "voice" : u == 2 ? "data" : "?"; }
static const char* lteDomName(uint32_t d) {
    switch (d) {
        case 0: return "none";
        case 1: return "ims";
        case 2: return "1x";
        case 3: return "cs";
    }
    return "?";
}

std::string NasImsState::summary() const {
    auto n = [](const auto& o) { return o ? std::to_string(*o) : std::string("-"); };
    std::string s = "vdp=" + n(vdp);
    if (vdp) s += std::string("(") + vdpName(*vdp) + ")";
    s += " usage=" + n(usage);
    if (usage) s += std::string("(") + usageName(*usage) + ")";
    s += " vops=" + n(vops) + " lte_voice_domain=" + n(lteVoiceDomain);
    if (lteVoiceDomain) s += std::string("(") + lteDomName(*lteVoiceDomain) + ")";
    s += " lte_srv=" + n(lteSrv);
    return s;
}

Result readNasImsState(Client& c, NasImsState* out) {
    NasImsState st;
    auto u32of = [](const Message& m, uint8_t t) -> std::optional<uint32_t> {
        if (auto* v = m.get(t); v && v->size() >= 4) return Reader(*v).u32();
        return std::nullopt;
    };
    auto r1 = c.request(kSvcNas, Message::request(nas::kGetSystemSelectionPreference));
    if (r1.ok()) {
        st.vdp = u32of(r1.msg, 0x20);
        st.usage = u32of(r1.msg, 0x1F);
        st.srvDomain = u32of(r1.msg, 0x18);
    }
    auto r2 = c.request(kSvcNas, Message::request(nas::kGetSystemInfo));
    if (r2.ok()) {
        if (auto* v = r2.msg.get(0x14); v && v->size() >= 2) {
            st.lteSrv = (*v)[0];
            st.lteTrueSrv = (*v)[1];
        }
        if (auto* v = r2.msg.get(0x29); v && !v->empty()) st.vops = (*v)[0];
        st.lteVoiceDomain = u32of(r2.msg, 0x2A);
    }
    if (out) *out = st;
    return r1.ok() ? r1 : r2;
}

Message buildSetVoiceDomainPref(uint32_t vdp) {
    return Message::request(nas::kSetSystemSelectionPreference).u32(0x23, vdp);
}

VdpMode vdpModeFromString(const std::string& s) { return s == "set" ? VdpMode::Set : VdpMode::Read; }

bool nasVoiceDomainSetup(Client& c, VdpMode mode, const ImsReportFn& report, NasImsState* state) {
    auto rep = [&](const std::string& s) {
        if (report) report(s);
    };
    NasImsState st;
    auto r = readNasImsState(c, &st);
    rep("A6L_IMSDCM_NAS get " + (r.ok() ? st.summary() : r.describe()));
    bool ok = st.vdp && *st.vdp == 3;
    if (mode == VdpMode::Set && r.ok() && !ok) {
        auto w = c.request(kSvcNas, buildSetVoiceDomainPref(3));
        rep("A6L_IMSDCM_NAS set voice_domain_pref=3(ims-pref): " + w.describe());
        auto r2 = readNasImsState(c, &st);
        rep("A6L_IMSDCM_NAS get-after " + (r2.ok() ? st.summary() : r2.describe()));
        ok = st.vdp && *st.vdp == 3;
        if (w.ok()) rep("A6L_IMSDCM_NAS NOTE the new preference reaches the network at the next attach/TAU");
    } else if (mode == VdpMode::Set && ok) {
        rep("A6L_IMSDCM_NAS voice_domain_pref already 3 (no write)");
    }
    if (state) *state = st;
    return ok;
}

// ---------------------------------------------------------------- volte6 kick policy
void ImsKickPolicy::schedule(int at, const std::string& why) {
    if (mKicks >= mMax) return;
    if (at < mLastKick + mDelay) at = mLastKick + mDelay;
    if (mDueAt < 0 || at < mDueAt) {
        mDueAt = at;
        mWhy = why;
    } else if (mWhy.find(why) == std::string::npos) {
        mWhy += "+" + why;
    }
}

void ImsKickPolicy::onDestroy(uint32_t instance, int nowS) {
    schedule(nowS + mDelay, "destroy-instance-" + std::to_string(instance));
}

void ImsKickPolicy::onNas(const NasImsState& st, int nowS) {
    bool full = st.lteFullService();
    if (full && !mHadFull) schedule(nowS + mDelay, "lte-full-service");
    if (st.lteSrv) mHadFull = full;  // unknown (query failed) keeps the last state
}

bool ImsKickPolicy::due(int nowS, std::string* why) {
    if (mDueAt < 0 || nowS < mDueAt || mKicks >= mMax) return false;
    if (why) *why = mWhy;
    mKicks++;
    mLastKick = nowS;
    mDueAt = -1;
    mWhy.clear();
    return true;
}

}  // namespace a6l::radio
