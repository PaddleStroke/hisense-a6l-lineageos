// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): mobile data call manager.
#include <a6lqmi/datacall.h>
#include <a6lqmi/multisim.h>
#include <a6lqmi/log.h>
#include <a6lqmi/rmnet.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace a6l::radio {

using namespace a6l::qmi;

namespace {
constexpr int kCauseUnspecified = 0xffff;       // DataCallFailCause::ERROR_UNSPECIFIED
constexpr int kCauseOemBase = 0x1001;           // OEM_DCFAILCAUSE_1
constexpr int kCauseNoNetdev = kCauseOemBase;   // OEM_DCFAILCAUSE_1: IPA/rmnet netdev missing
constexpr int kCauseQmi = kCauseOemBase + 1;    // OEM_DCFAILCAUSE_2: QMI transport problem
constexpr uint16_t kVerbose3gpp = 6;            // QMI_WDS_VERBOSE_CALL_END_REASON_TYPE_3GPP
}  // namespace

DataCallManager::DataCallManager(DataConfig cfg, ClientFactory factory, Client* control)
    : mCfg(std::move(cfg)), mFactory(std::move(factory)), mControl(control) {}

DataCallManager::~DataCallManager() { deactivateAll(); }

namespace {
uint32_t readSysUint(const std::string& path) {
    FILE* f = fopen(path.c_str(), "re");
    if (!f) return 0;
    unsigned v = 0;
    if (fscanf(f, "%u", &v) != 1) v = 0;
    fclose(f);
    return v;
}
}  // namespace

bool DataCallManager::openDpmPort() {
    if (!mDpm) {
        mDpm = mFactory("dpm");
        if (!mDpm || !mDpm->start({kSvcDpm}) || !mDpm->waitForServices({kSvcDpm}, 5000).empty()) {
            mReport.push_back("dpm: service 0x2f not found (continuing with WDA)");
            ALOGW_Q("data: %s", mReport.back().c_str());
            if (mDpm) mDpm->stop();
            mDpm.reset();
            return false;
        }
    }
    dpm::HwDataPort p;
    p.epType = mCfg.epType;
    p.iface = mCfg.epIface;
    const std::string sys = "/sys/class/net/" + mCfg.parentIface + "/device/modem/";
    p.rxEp = mCfg.dpmRxEp ? mCfg.dpmRxEp : readSysUint(sys + "tx_endpoint_id");
    p.txEp = mCfg.dpmTxEp ? mCfg.dpmTxEp : readSysUint(sys + "rx_endpoint_id");
    if (!p.rxEp) p.rxEp = 4;
    if (!p.txEp) p.txEp = 5;
    auto r = dpm::openPort(*mDpm, {p});
    char b[160];
    snprintf(b, sizeof(b), "dpm: open port hw ep=%u/%u rx_ep(consumer)=%u tx_ep(producer)=%u: %s",
             p.epType, p.iface, p.rxEp, p.txEp, r.describe().c_str());
    mReport.push_back(b);
    ALOGW_Q("data: %s", b);
    mDpmDone = r.ok() || (r.status == Result::QmiFailure && r.qmiError == kErrNoEffect);
    return mDpmDone;
}

bool DataCallManager::ensureFormat() {
    if (mFormatDone || !mCfg.setDataFormat) return true;
    if (!mControl) return false;
    if (mCfg.dpmOpenPort && !mDpmDone) openDpmPort();
    char b[200];
    wda::DataFormat cur;
    auto g = wda::getDataFormat(*mControl, mCfg.epType, mCfg.epIface, &cur);
    if (g.ok())
        snprintf(b, sizeof(b), "wda: current llp=%u ul=%u dl=%u dl_max=%u/%u", cur.llp, cur.ulAgg,
                 cur.dlAgg, cur.dlMaxDatagrams, cur.dlMaxSize);
    else
        snprintf(b, sizeof(b), "wda: get data format: %s", g.describe().c_str());
    mReport.push_back(b);
    ALOGI_Q("data: %s", b);
    wda::DataFormat f, got;
    f.epType = mCfg.epType;
    f.iface = mCfg.epIface;
    f.ulAgg = mCfg.wdaUlAgg;
    f.dlAgg = mCfg.wdaDlAgg;
    f.dlMaxDatagrams = mCfg.wdaDlMaxDatagrams;
    f.dlMaxSize = mCfg.wdaDlMaxSize;
    auto r = wda::setDataFormat(*mControl, f, &got);
    if (!r.ok()) {
        snprintf(b, sizeof(b), "wda: set data format ep=%u/%u ul=%u dl=%u failed: %s%s", f.epType,
                 f.iface, f.ulAgg, f.dlAgg, r.describe().c_str(),
                 mCfg.dpmOpenPort && !mDpmDone ? " (DPM port not open)" : "");
        mReport.push_back(b);
        ALOGE_Q("data: %s", b);
        return false;
    }
    snprintf(b, sizeof(b), "wda: format set llp=%u ul=%u dl=%u dl_max=%u/%u", got.llp, got.ulAgg,
             got.dlAgg, got.dlMaxDatagrams, got.dlMaxSize);
    mReport.push_back(b);
    ALOGI_Q("data: %s", b);
    if (got.ulAgg != f.ulAgg || got.dlAgg != f.dlAgg)
        ALOGW_Q("data: modem granted ul=%u dl=%u (asked %u/%u)", got.ulAgg, got.dlAgg, f.ulAgg, f.dlAgg);
    mFormatDone = true;
    return true;
}

bool DataCallManager::prepareDataPath() {
    std::lock_guard<std::mutex> l(mLock);
    return ensureFormat();
}

std::string DataCallManager::formatReport() const {
    std::lock_guard<std::mutex> l(mLock);
    std::string o;
    for (auto& x : mReport) o += (o.empty() ? "" : "\n") + x;
    return o;
}

int DataCallManager::allocMux() const {
    for (int i = 0; i < mCfg.maxCalls; i++) {
        int mux = mCfg.muxBase + i;
        if (!mCalls.count(mux)) return mux;
    }
    return -1;
}

std::string DataCallManager::settingsSummary(const wds::Settings& s) {
    std::string o;
    auto add = [&](const char* k, const std::string& v) {
        if (!o.empty()) o += ' ';
        o += k;
        o += '=';
        o += v;
    };
    if (s.ipv4) add("ip", ipv4ToString(*s.ipv4) + (s.mask4 ? "/" + std::to_string(maskToPrefix(*s.mask4)) : ""));
    if (s.gw4) add("gw", ipv4ToString(*s.gw4));
    if (s.dns4a) add("dns1", ipv4ToString(*s.dns4a));
    if (s.dns4b) add("dns2", ipv4ToString(*s.dns4b));
    if (s.ipv6) add("ip6", ipv6ToString(s.ipv6->data()) + "/" + std::to_string(s.ipv6Prefix));
    if (s.gw6) add("gw6", ipv6ToString(s.gw6->data()));
    if (s.dns6a) add("dns6_1", ipv6ToString(s.dns6a->data()));
    if (s.dns6b) add("dns6_2", ipv6ToString(s.dns6b->data()));
    if (s.mtu) add("mtu", std::to_string(*s.mtu));
    if (!s.apn.empty()) add("apn", s.apn);
    return o;
}

void DataCallManager::applySettings(const wds::Settings& s, bool v6, DataCall* c) {
    auto pushUnique = [](std::vector<std::string>& v, const std::string& x) {
        for (auto& y : v)
            if (y == x) return;
        v.push_back(x);
    };
    if (!v6) {
        if (s.ipv4) {
            int prefix = s.mask4 ? maskToPrefix(*s.mask4) : 32;
            if (prefix == 0) prefix = 32;
            pushUnique(c->addresses, ipv4ToString(*s.ipv4) + "/" + std::to_string(prefix));
            c->v4 = true;
        }
        if (s.gw4) pushUnique(c->gateways, ipv4ToString(*s.gw4));
        if (s.dns4a && *s.dns4a) pushUnique(c->dnses, ipv4ToString(*s.dns4a));
        if (s.dns4b && *s.dns4b) pushUnique(c->dnses, ipv4ToString(*s.dns4b));
        for (auto p : s.pcscf4) pushUnique(c->pcscf, ipv4ToString(p));
        if (s.mtu) c->mtuV4 = static_cast<int>(*s.mtu);
    } else {
        if (s.ipv6) {
            pushUnique(c->addresses, ipv6ToString(s.ipv6->data()) + "/" +
                                             std::to_string(s.ipv6Prefix ? s.ipv6Prefix : 64));
            c->v6 = true;
        }
        if (s.gw6) pushUnique(c->gateways, ipv6ToString(s.gw6->data()));
        if (s.dns6a) pushUnique(c->dnses, ipv6ToString(s.dns6a->data()));
        if (s.dns6b) pushUnique(c->dnses, ipv6ToString(s.dns6b->data()));
        if (s.mtu) c->mtuV6 = static_cast<int>(*s.mtu);
    }
    if (c->apn.empty()) c->apn = s.apn;
}

int DataCallManager::failCauseFrom(const wds::StartResult& r, const Result& res) {
    if (r.verboseReason && r.verboseReason->first == kVerbose3gpp && r.verboseReason->second > 0)
        return r.verboseReason->second;  // Android DataCallFailCause uses the 3GPP cause values
    if (res.status != Result::QmiFailure) return kCauseQmi;
    return kCauseUnspecified;
}

bool LinkOps::exists(const std::string& name) { return rmnet::exists(name); }
int LinkOps::createLink(const std::string& parent, const std::string& name, uint16_t muxId, uint32_t flags) {
    return rmnet::createLink(parent, name, muxId, flags);
}
int LinkOps::deleteLink(const std::string& name) { return rmnet::deleteLink(name); }
int LinkOps::setUp(const std::string& name, bool up) { return rmnet::setUp(name, up); }
int LinkOps::setMtu(const std::string& name, int mtu) { return rmnet::setMtu(name, mtu); }
int LinkOps::addAddress(const std::string& name, const std::string& cidr) { return rmnet::addAddress(name, cidr); }

void DataCallManager::stopLeg(Leg& leg) {
    leg.state->stopping = true;  // r5 round8: the DISCONNECTED our STOP causes is not a network loss
    if (leg.handle) {
        auto r = wds::stopNetwork(*leg.client, leg.handle);
        if (!r.ok()) ALOGW_Q("data: stop network: %s", r.describe().c_str());
        leg.handle = 0;
    }
    leg.client->stop();
}

// r5 review F15 (28 Sep 2026): a leg only counts when every prerequisite succeeded: WDS bind (subscription, mux
// data port), START with a packet data handle, and current settings carrying this family's address. Set IP Family
// stays best effort (older firmwares reject it; ModemManager continues too) because the settings check proves the
// family. A leg that fails after START stops its own WDS session.
bool DataCallManager::startLeg(Entry& e, bool v6, const DataRequest& req, std::string* detail,
                               int* cause) {
    const char* fam = v6 ? " v6:" : " v4:";
    Leg leg;
    leg.v6 = v6;
    leg.client = mFactory(v6 ? "wds6" : "wds4");
    if (!leg.client || !leg.client->start({kSvcWds}) ||
        !leg.client->waitForServices({kSvcWds}, 5000).empty()) {
        *detail += v6 ? " v6:no-wds" : " v4:no-wds";
        *cause = kCauseQmi;
        if (leg.client) leg.client->stop();
        return false;
    }
    if (mCfg.subscription > 0) {
        auto rb = multisim::wdsBind(*leg.client, mCfg.subscription);
        ALOGI_Q("data: WDS bind subscription %u: %s", mCfg.subscription, rb.describe().c_str());
        if (!rb.ok()) {
            *detail += " wds-bind-sub:" + rb.describe();
            *cause = kCauseQmi;
            leg.client->stop();
            return false;  // never start the secondary SIM's call on the primary subscription
        }
    }
    auto r = wds::bindMuxDataPort(*leg.client, mCfg.epType, mCfg.epIface, e.call.muxId);
    if (!r.ok()) {
        // an unbound client would start the session on the default (non-muxed) port: its traffic never
        // reaches rmnet_data<N>
        ALOGW_Q("data: bind mux %u: %s", e.call.muxId, r.describe().c_str());
        *detail += std::string(fam) + "bind-mux:" + r.describe();
        *cause = kCauseQmi;
        leg.client->stop();
        return false;
    }
    // r5 review round8 F60 (28 Sep 2026): lifecycle observers before START. Before, they were installed after START
    // and Get Current Settings: a DISCONNECTED consumed by the dispatcher in that window was lost and setup published
    // a dead call. The latch (LegState) is checked before publication; after it, the loss goes through mLost with
    // the call's (cid, generation) as before (a loss for a never-published generation is ignored as stale).
    // F59: CONNECTED + reconfiguration required on a published call -> mReconfig (refresh on a worker).
    const int cid = e.call.cid;
    const uint64_t gen = e.call.generation;
    auto st = leg.state;
    leg.client->onIndication(kSvcWds, wds::kPacketServiceStatus, [this, cid, gen, st](const Message& m) {
        if (st->stopping) return;
        auto ps = wds::parsePacketStatus(m);
        if (ps.status == wds::kConnDisconnected) {
            if (ps.verboseReason && ps.verboseReason->first == kVerbose3gpp && ps.verboseReason->second > 0)
                st->endReason = ps.verboseReason->second;
            if (st->ended.exchange(true)) return;
            ALOGW_Q("data: cid %d (gen %llu) disconnected by network (end reason %d)", cid,
                    static_cast<unsigned long long>(gen), ps.callEndReason ? *ps.callEndReason : -1);
            if (mLost) mLost(cid, gen);
        } else if (ps.status == wds::kConnConnected && ps.reconfig) {
            ALOGW_Q("data: cid %d (gen %llu) reconfiguration required", cid, static_cast<unsigned long long>(gen));
            st->reconfig = true;
            if (st->published && mReconfig) mReconfig(cid, gen);
        }
    });
    // r5 F17/F16: this leg's WDS service withdrawn (WDS restart) or its QRTR transport failed: the session is gone
    leg.client->onServiceChange([this, cid, gen, st](uint32_t svc, bool up) {
        if (svc != kSvcWds || up || st->stopping) return;
        if (st->ended.exchange(true)) return;
        ALOGW_Q("data: cid %d (gen %llu) lost its WDS service", cid, static_cast<unsigned long long>(gen));
        if (mLost) mLost(cid, gen);
    });

    r = wds::setIpFamily(*leg.client, v6 ? wds::kFamilyV6 : wds::kFamilyV4);
    if (!r.ok()) ALOGW_Q("data: set ip family: %s (continuing, the settings must carry the family)", r.describe().c_str());

    wds::StartParams p;
    p.apn = req.apn;
    p.user = req.user;
    p.password = req.password;
    p.auth = req.auth;
    p.ipFamily = v6 ? wds::kFamilyV6 : wds::kFamilyV4;
    wds::StartResult sr;
    r = wds::startNetwork(*leg.client, p, &sr);
    if (!r.ok()) {
        *cause = failCauseFrom(sr, r);
        *detail += std::string(fam) + r.describe();
        if (sr.verboseReason)
            *detail += " verbose=" + std::to_string(sr.verboseReason->first) + "/" +
                       std::to_string(sr.verboseReason->second);
        ALOGW_Q("data: start network %s apn '%s' failed:%s", v6 ? "v6" : "v4", req.apn.c_str(),
                detail->c_str());
        st->stopping = true;
        leg.client->stop();
        return false;
    }
    leg.handle = sr.handle;
    if (!leg.handle) {
        *detail += std::string(fam) + "no-handle";
        *cause = kCauseQmi;
        st->stopping = true;
        leg.client->stop();
        return false;
    }
    wds::Settings s;
    bool usable = false;
    std::string why;
    for (int i = 0; i < std::max(1, mCfg.settingsTries); i++) {
        if (i) std::this_thread::sleep_for(std::chrono::milliseconds(mCfg.settingsRetryMs));
        s = wds::Settings{};
        r = wds::getCurrentSettings(*leg.client, &s);
        if (!r.ok()) {
            why = "settings:" + r.describe();
            ALOGW_Q("data: get current settings (%d): %s", i + 1, r.describe().c_str());
            continue;
        }
        usable = v6 ? s.ipv6.has_value() : (s.ipv4.has_value() && *s.ipv4 != 0);
        if (usable) break;
        why = v6 ? "settings:no-ipv6-address" : "settings:no-ipv4-address";
        ALOGW_Q("data: mux %u %s settings without an address (%d): %s", e.call.muxId, v6 ? "v6" : "v4", i + 1,
                settingsSummary(s).c_str());
    }
    if (!usable) {
        *detail += std::string(fam) + why;
        *cause = kCauseQmi;
        stopLeg(leg);
        return false;
    }
    ALOGI_Q("data: mux %u %s settings: %s", e.call.muxId, v6 ? "v6" : "v4", settingsSummary(s).c_str());
    applySettings(s, v6, &e.call);
    e.legs.push_back(std::move(leg));
    return true;
}

bool DataCallManager::prepareFormat() {
    std::lock_guard<std::mutex> l(mLock);
    return ensureFormat();
}

void DataCallManager::dropFamily(Entry& e, bool v6) {
    for (auto it = e.legs.begin(); it != e.legs.end();) {
        if (it->v6 == v6) {
            // stopLeg is not static: callers stop the leg before (see configureLink)
            it = e.legs.erase(it);
        } else {
            ++it;
        }
    }
    auto isV6 = [](const std::string& a) { return a.find(':') != std::string::npos; };
    for (auto* v : {&e.call.addresses, &e.call.gateways, &e.call.dnses, &e.call.pcscf}) {
        std::vector<std::string> keep;
        for (auto& a : *v)
            if (isV6(a) != v6) keep.push_back(a);
        v->swap(keep);
    }
    if (v6) {
        e.call.v6 = false;
        e.call.mtuV6 = 0;
    } else {
        e.call.v4 = false;
        e.call.mtuV4 = 0;
    }
}

// r5 F14/F15: rmnet link + parent up + addresses + link up, every result checked. The MTU is best effort: Android
// programs the interface MTU itself from mtuV4/mtuV6 (ConnectivityService updateMtu). An address family that cannot
// be installed (e.g. IPv6 disabled on the netdev) is dropped from a dual-stack call; nothing usable -> failure.
bool DataCallManager::configureLink(Entry& e, std::string* why) {
    const std::string& ifn = e.call.ifname;
    // our mux id / name: a netdev still there is stale (HAL restart) and may carry old addresses
    if (mLink->exists(ifn)) mLink->deleteLink(ifn);
    int r = mLink->createLink(mCfg.parentIface, ifn, e.call.muxId, mCfg.rmnetFlags);
    if (r) {
        *why = "rmnet-link-failed";
        return false;
    }
    if ((r = mLink->setUp(mCfg.parentIface, true))) {
        *why = "parent-up-failed:" + std::to_string(r);
        return false;
    }
    int mtu = e.call.mtuV4 ? e.call.mtuV4 : e.call.mtuV6;
    if (mtu > 0 && (r = mLink->setMtu(ifn, mtu)))
        ALOGW_Q("data: set mtu %d on %s: %d (continuing, Android sets the MTU from the call)", mtu, ifn.c_str(), r);
    if (mCfg.assignAddresses) {
        for (bool v6 : {false, true}) {
            if (!(v6 ? e.call.v6 : e.call.v4)) continue;
            std::vector<std::string> addrs;
            for (auto& a : e.call.addresses)
                if ((a.find(':') != std::string::npos) == v6) addrs.push_back(a);
            for (auto& a : addrs) {
                if (!(r = mLink->addAddress(ifn, a))) continue;
                ALOGW_Q("data: add %s to %s failed (%d): dropping %s", a.c_str(), ifn.c_str(), r, v6 ? "IPv6" : "IPv4");
                *why += std::string(v6 ? " v6" : " v4") + ":address-failed";
                for (auto& leg : e.legs)
                    if (leg.v6 == v6) stopLeg(leg);
                dropFamily(e, v6);
                break;
            }
        }
        if (e.legs.empty()) return false;
    }
    if ((r = mLink->setUp(ifn, true))) {
        *why += " link-up-failed:" + std::to_string(r);
        return false;
    }
    return true;
}

SetupOutcome DataCallManager::setup(const DataRequest& req) {
    SetupOutcome out;
    std::lock_guard<std::mutex> l(mLock);
    if (mCfg.requireNetdev && !mLink->exists(mCfg.parentIface)) {
        out.failCause = kCauseNoNetdev;
        out.detail = "no " + mCfg.parentIface + " netdev (IPA driver missing)";
        return out;
    }
    if (!ensureFormat() && mCfg.requireNetdev) {
        out.failCause = kCauseQmi;
        out.detail = "WDA set data format failed";
        if (!mReport.empty()) out.detail += " [" + mReport.back() + "]";
        return out;
    }
    int mux = allocMux();
    if (mux < 0) {
        out.failCause = 0x41;  // INSUFFICIENT_RESOURCES
        out.detail = "no free mux id";
        return out;
    }
    Entry e;
    e.call.cid = mux;
    e.call.muxId = static_cast<uint8_t>(mux);
    e.call.ifname = mCfg.ifPrefix + std::to_string(mCfg.ifIndexBase + mux - mCfg.muxBase);
    e.call.apn = req.apn;
    e.call.generation = ++mNextGeneration;

    int cause = kCauseUnspecified;
    bool any = false;
    // dual stack: one family may fail (e.g. v6 not offered) and the call still comes up with the other
    if (req.protocol != Protocol::V6) any |= startLeg(e, false, req, &out.detail, &cause);
    if (req.protocol != Protocol::V4) any |= startLeg(e, true, req, &out.detail, &cause);
    if (!any) {
        out.failCause = cause;
        return out;
    }
    if (mCfg.requireNetdev) {
        std::string why;
        if (!configureLink(e, &why)) {
            out.detail += " " + why;
            stopEntry(e);
            out.failCause = kCauseNoNetdev;
            return out;
        }
        if (!why.empty()) out.detail += why;
    }
    // r5 round8 F60: a leg the network already ended during setup (its DISCONNECTED/WDS loss was latched) must not be
    // published as a working call: the whole setup fails (a loss is per call, like after publication).
    for (auto& leg : e.legs) {
        if (!leg.state->ended) continue;
        int reason = leg.state->endReason;
        out.detail += std::string(leg.v6 ? " v6" : " v4") + ":ended-during-setup";
        ALOGW_Q("data: mux %u %s ended during setup: failing it", e.call.muxId, leg.v6 ? "v6" : "v4");
        stopEntry(e);
        out.failCause = reason > 0 ? reason : kCauseUnspecified;
        return out;
    }
    out.ok = true;
    out.call = e.call;
    const int cid = e.call.cid;
    const uint64_t gen = e.call.generation;
    bool reconfig = false;
    for (auto& leg : e.legs) leg.state->published = true;
    for (auto& leg : e.legs) reconfig |= leg.state->reconfig.load();
    mCalls[mux] = std::move(e);
    // F59: a reconfiguration flagged before publication is handled like one after it (the callback saw published=false)
    if (reconfig && mReconfig) mReconfig(cid, gen);
    return out;
}

// r5 review round8 F59 (28 Sep 2026)
DataCallManager::Refresh DataCallManager::refresh(int cid, uint64_t generation, DataCall* out) {
    std::lock_guard<std::mutex> l(mLock);
    auto it = mCalls.find(cid);
    if (it == mCalls.end() || it->second.call.generation != generation) return Refresh::Gone;
    Entry& e = it->second;
    DataCall fresh = e.call;
    fresh.addresses.clear();
    fresh.gateways.clear();
    fresh.dnses.clear();
    fresh.pcscf.clear();
    fresh.mtuV4 = fresh.mtuV6 = 0;
    fresh.v4 = fresh.v6 = false;
    for (auto& leg : e.legs) {
        leg.state->reconfig = false;
        wds::Settings s;
        auto r = wds::getCurrentSettings(*leg.client, &s);
        bool usable = r.ok() && (leg.v6 ? s.ipv6.has_value() : (s.ipv4.has_value() && *s.ipv4 != 0));
        if (!usable) {
            ALOGW_Q("data: cid %d reconfiguration: %s settings unusable (%s): invalidating", cid, leg.v6 ? "v6" : "v4",
                    r.ok() ? "no address" : r.describe().c_str());
            return Refresh::Invalidate;
        }
        ALOGI_Q("data: cid %d reconfigured %s settings: %s", cid, leg.v6 ? "v6" : "v4", settingsSummary(s).c_str());
        applySettings(s, leg.v6, &fresh);
    }
    auto sorted = [](std::vector<std::string> v) {
        std::sort(v.begin(), v.end());
        return v;
    };
    if (sorted(fresh.addresses) != sorted(e.call.addresses)) {
        ALOGW_Q("data: cid %d reconfiguration changed its addresses: invalidating (reconnect)", cid);
        return Refresh::Invalidate;
    }
    if (fresh.gateways == e.call.gateways && fresh.dnses == e.call.dnses && fresh.pcscf == e.call.pcscf &&
        fresh.mtuV4 == e.call.mtuV4 && fresh.mtuV6 == e.call.mtuV6)
        return Refresh::Unchanged;
    int oldMtu = e.call.mtuV4 ? e.call.mtuV4 : e.call.mtuV6;
    int mtu = fresh.mtuV4 ? fresh.mtuV4 : fresh.mtuV6;
    if (mCfg.requireNetdev && mtu > 0 && mtu != oldMtu) {
        int r = mLink->setMtu(e.call.ifname, mtu);
        if (r) ALOGW_Q("data: set mtu %d on %s: %d (Android sets it from the call)", mtu, e.call.ifname.c_str(), r);
    }
    e.call = fresh;
    *out = e.call;
    return Refresh::Updated;
}

void DataCallManager::stopEntry(Entry& e) {
    for (auto& leg : e.legs) stopLeg(leg);
    e.legs.clear();
    if (mCfg.requireNetdev) mLink->deleteLink(e.call.ifname);
}

bool DataCallManager::deactivate(int cid) {
    std::lock_guard<std::mutex> l(mLock);
    auto it = mCalls.find(cid);
    if (it == mCalls.end()) return false;
    stopEntry(it->second);
    mCalls.erase(it);
    return true;
}

bool DataCallManager::deactivateIfCurrent(int cid, uint64_t generation) {
    std::lock_guard<std::mutex> l(mLock);
    auto it = mCalls.find(cid);
    if (it == mCalls.end() || it->second.call.generation != generation) {
        ALOGI_Q("data: stale loss for cid %d gen %llu ignored", cid, static_cast<unsigned long long>(generation));
        return false;
    }
    stopEntry(it->second);
    mCalls.erase(it);
    return true;
}

void DataCallManager::deactivateAll() {
    std::lock_guard<std::mutex> l(mLock);
    for (auto& [cid, e] : mCalls) stopEntry(e);
    mCalls.clear();
}

void DataCallManager::modemReset() {
    std::lock_guard<std::mutex> l(mLock);
    for (auto& [cid, e] : mCalls) {
        for (auto& leg : e.legs) {
            leg.handle = 0;  // the modem forgot them
            leg.client->stop();
        }
        if (mCfg.requireNetdev) mLink->deleteLink(e.call.ifname);
    }
    mCalls.clear();
    mFormatDone = false;
    mDpmDone = false;
    if (mDpm) mDpm->stop();
    mDpm.reset();
    mReport.clear();
}

std::vector<DataCall> DataCallManager::list() const {
    std::lock_guard<std::mutex> l(mLock);
    std::vector<DataCall> v;
    for (const auto& [cid, e] : mCalls) v.push_back(e.call);
    return v;
}

}  // namespace a6l::radio
