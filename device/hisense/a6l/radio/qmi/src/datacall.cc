// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): mobile data call manager.
#include <a6lqmi/datacall.h>
#include <a6lqmi/log.h>
#include <a6lqmi/rmnet.h>

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

bool DataCallManager::ensureFormat() {
    if (mFormatDone || !mCfg.setDataFormat) return true;
    if (!mControl) return false;
    wda::DataFormat f, got;
    f.epType = mCfg.epType;
    f.iface = mCfg.epIface;
    auto r = wda::setDataFormat(*mControl, f, &got);
    if (!r.ok()) {
        ALOGE_Q("data: WDA set data format failed: %s", r.describe().c_str());
        return false;
    }
    ALOGI_Q("data: WDA format llp=%u ul=%u dl=%u dl_max=%u/%u", got.llp, got.ulAgg, got.dlAgg,
            got.dlMaxDatagrams, got.dlMaxSize);
    mFormatDone = true;
    return true;
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

bool DataCallManager::startLeg(Entry& e, bool v6, const DataRequest& req, std::string* detail,
                               int* cause) {
    Leg leg;
    leg.v6 = v6;
    leg.client = mFactory(v6 ? "wds6" : "wds4");
    if (!leg.client || !leg.client->start({kSvcWds}) ||
        !leg.client->waitForServices({kSvcWds}, 5000).empty()) {
        *detail += v6 ? " v6:no-wds" : " v4:no-wds";
        *cause = kCauseQmi;
        return false;
    }
    auto r = wds::bindMuxDataPort(*leg.client, mCfg.epType, mCfg.epIface, e.call.muxId);
    if (!r.ok()) ALOGW_Q("data: bind mux %u: %s (continuing)", e.call.muxId, r.describe().c_str());
    r = wds::setIpFamily(*leg.client, v6 ? wds::kFamilyV6 : wds::kFamilyV4);
    if (!r.ok()) ALOGW_Q("data: set ip family: %s (continuing)", r.describe().c_str());

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
        *detail += std::string(v6 ? " v6:" : " v4:") + r.describe();
        if (sr.verboseReason)
            *detail += " verbose=" + std::to_string(sr.verboseReason->first) + "/" +
                       std::to_string(sr.verboseReason->second);
        ALOGW_Q("data: start network %s apn '%s' failed:%s", v6 ? "v6" : "v4", req.apn.c_str(),
                detail->c_str());
        leg.client->stop();
        return false;
    }
    leg.handle = sr.handle;
    wds::Settings s;
    r = wds::getCurrentSettings(*leg.client, &s);
    if (!r.ok()) {
        ALOGW_Q("data: get current settings: %s", r.describe().c_str());
    } else {
        ALOGI_Q("data: mux %u %s settings: %s", e.call.muxId, v6 ? "v6" : "v4",
                settingsSummary(s).c_str());
        applySettings(s, v6, &e.call);
    }
    int cid = e.call.cid;
    leg.client->onIndication(kSvcWds, wds::kPacketServiceStatus, [this, cid](const Message& m) {
        auto ps = wds::parsePacketStatus(m);
        if (ps.status == wds::kConnDisconnected) {
            ALOGW_Q("data: cid %d disconnected by network (end reason %d)", cid,
                    ps.callEndReason ? *ps.callEndReason : -1);
            if (mLost) mLost(cid);
        }
    });
    e.legs.push_back(std::move(leg));
    return true;
}

SetupOutcome DataCallManager::setup(const DataRequest& req) {
    SetupOutcome out;
    std::lock_guard<std::mutex> l(mLock);
    if (mCfg.requireNetdev && !rmnet::exists(mCfg.parentIface)) {
        out.failCause = kCauseNoNetdev;
        out.detail = "no " + mCfg.parentIface + " netdev (IPA driver missing)";
        return out;
    }
    if (!ensureFormat() && mCfg.requireNetdev) {
        out.failCause = kCauseQmi;
        out.detail = "WDA set data format failed";
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
    e.call.ifname = mCfg.ifPrefix + std::to_string(mux - mCfg.muxBase);
    e.call.apn = req.apn;

    int cause = kCauseUnspecified;
    bool any = false;
    if (req.protocol != Protocol::V6) any |= startLeg(e, false, req, &out.detail, &cause);
    if (req.protocol != Protocol::V4) any |= startLeg(e, true, req, &out.detail, &cause);
    if (!any) {
        out.failCause = cause;
        return out;
    }
    if (mCfg.requireNetdev) {
        int r = rmnet::createLink(mCfg.parentIface, e.call.ifname, e.call.muxId, mCfg.rmnetFlags);
        if (r == 0) {
            rmnet::setUp(mCfg.parentIface, true);
            int mtu = e.call.mtuV4 ? e.call.mtuV4 : e.call.mtuV6;
            if (mtu > 0) rmnet::setMtu(e.call.ifname, mtu);
            rmnet::setUp(e.call.ifname, true);
        } else {
            out.detail += " rmnet-link-failed";
            stopEntry(e);
            out.failCause = kCauseNoNetdev;
            return out;
        }
    }
    out.ok = true;
    out.call = e.call;
    mCalls[mux] = std::move(e);
    return out;
}

void DataCallManager::stopEntry(Entry& e) {
    for (auto& leg : e.legs) {
        if (leg.handle) {
            auto r = wds::stopNetwork(*leg.client, leg.handle);
            if (!r.ok()) ALOGW_Q("data: stop network: %s", r.describe().c_str());
        }
        leg.client->stop();
    }
    e.legs.clear();
    if (mCfg.requireNetdev) rmnet::deleteLink(e.call.ifname);
}

bool DataCallManager::deactivate(int cid) {
    std::lock_guard<std::mutex> l(mLock);
    auto it = mCalls.find(cid);
    if (it == mCalls.end()) return false;
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
        if (mCfg.requireNetdev) rmnet::deleteLink(e.call.ifname);
    }
    mCalls.clear();
    mFormatDone = false;
}

std::vector<DataCall> DataCallManager::list() const {
    std::lock_guard<std::mutex> l(mLock);
    std::vector<DataCall> v;
    for (const auto& [cid, e] : mCalls) v.push_back(e.call);
    return v;
}

}  // namespace a6l::radio
