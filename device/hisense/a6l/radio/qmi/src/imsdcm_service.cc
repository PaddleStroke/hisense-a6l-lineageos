// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): IMSDCM (770) server logic, see imsdcm_service.h.
#include <a6lqmi/imsdcm_service.h>
#include <a6lqmi/log.h>
#include <a6lqmi/services.h>

#include <cstdio>
#include <ctime>

namespace a6l::radio {

using namespace a6l::qmi;
namespace dcm = a6l::qmi::imsdcm;

const char* ImsDcmService::stateName(State s) {
    switch (s) {
        case State::Activating: return "activating";
        case State::Up: return "up";
        case State::Failed: return "failed";
        case State::Down: return "down";
    }
    return "?";
}

ImsDcmService::ImsDcmService(ImsDcmConfig cfg, std::unique_ptr<Transport> serverTransport,
                             ClientFactory wdsFactory)
    : mCfg(cfg), mFactory(std::move(wdsFactory)) {
    mServer = std::make_unique<Server>(std::move(serverTransport), dcm::kService, dcm::kIdlMajor,
                                       cfg.instance, "imsdcm");
    mServer->onRequest([this](const Addr& a, const Message& m) { return handle(a, m); });
    mServer->onClientGone([this](const Addr& a, bool node) {
        report(std::string("A6L_IMSDCM_CLIENT_GONE ") + std::to_string(a.node) + ":" +
               std::to_string(a.port) + (node ? " (modem node BYE)" : ""));
        std::vector<uint8_t> ids;
        {
            std::lock_guard<std::mutex> l(mLock);
            for (auto& [id, e] : mPdps)
                if (e.pdp.client == a || (node && e.pdp.client.node == a.node)) ids.push_back(id);
        }
        for (uint8_t id : ids) post([this, id] { release(id, false, "client gone"); });
    });
}

ImsDcmService::~ImsDcmService() { stop(); }

void ImsDcmService::report(const std::string& s) {
    ALOGI_Q("%s", s.c_str());
    if (mReport) mReport(s);
}

bool ImsDcmService::start() {
    {
        std::lock_guard<std::mutex> l(mWorkLock);
        if (mRunning) return true;
        mRunning = true;
    }
    mWorker = std::thread([this] { workerLoop(); });
    if (!mServer->start()) {
        report("A6L_IMSDCM_FAIL cannot open the QRTR server socket");
        stop();
        return false;
    }
    char b[160];
    snprintf(b, sizeof b, "A6L_IMSDCM_PUBLISHED service=770 idl=%u.15 instance_word=0x%x mux=%u profile=%s",
             dcm::kIdlMajor, mServer->instanceWord(), mCfg.muxId,
             mCfg.profileOverride > 0 ? std::to_string(mCfg.profileOverride).c_str() : "modem");
    report(b);
    return true;
}

void ImsDcmService::stop() {
    {
        std::lock_guard<std::mutex> l(mWorkLock);
        if (!mRunning && !mWorker.joinable()) return;
    }
    if (mServer) mServer->stop();
    std::vector<uint8_t> ids;
    {
        std::lock_guard<std::mutex> l(mLock);
        for (auto& [id, e] : mPdps) ids.push_back(id);
    }
    for (uint8_t id : ids) post([this, id] { release(id, false, "shutdown"); });
    {
        std::lock_guard<std::mutex> l(mWorkLock);
        mRunning = false;
        mWorkCv.notify_all();
    }
    if (mWorker.joinable()) mWorker.join();
}

void ImsDcmService::post(std::function<void()> fn) {
    std::lock_guard<std::mutex> l(mWorkLock);
    mWork.push_back(std::move(fn));
    mWorkCv.notify_all();
}

void ImsDcmService::workerLoop() {
    while (true) {
        std::function<void()> fn;
        {
            std::unique_lock<std::mutex> l(mWorkLock);
            mWorkCv.wait(l, [this] { return !mWork.empty() || !mRunning; });
            if (mWork.empty()) return;  // not running and drained
            fn = std::move(mWork.front());
            mWork.pop_front();
        }
        fn();
    }
}

std::vector<ImsDcmService::Pdp> ImsDcmService::pdps() const {
    std::lock_guard<std::mutex> l(mLock);
    std::vector<Pdp> out;
    for (auto& [id, e] : mPdps) out.push_back(e.pdp);
    return out;
}

uint8_t ImsDcmService::allocId() {  // mLock held; ids 1..255 ("PDP ID should not be zero")
    for (int i = 0; i < 255; i++) {
        uint8_t id = mNextId;
        mNextId = mNextId == 255 ? 1 : mNextId + 1;
        if (!mPdps.count(id)) return id;
    }
    return 0;
}

std::vector<uint8_t> ImsDcmService::addrTlv(const Pdp& p) const {
    return dcm::encodeAddress(p.req.family, p.address, mCfg.addrAsText);
}

std::optional<Message> ImsDcmService::handle(const Addr& from, const Message& m) {
    char b[400];
    switch (m.msgId) {
        case dcm::kPdpActivate: {
            auto q = dcm::parsePdpActivate(m);
            if (!q) {
                report("A6L_IMSDCM_REQ PDP_ACTIVATE malformed: " + m.dump());
                return dcm::buildPdpActivateResp(m.get(0x01) ? kErrMalformedMessage : kErrMissingArgument, 0,
                                                 std::nullopt, std::nullopt);
            }
            snprintf(b, sizeof b,
                     "A6L_IMSDCM_REQ from=%u:%u PDP_ACTIVATE apn='%s' type=%s rat=%s family=%s profile=%u seq=%d sub=%d slot=%d inst=%d",
                     from.node, from.port, q->apn.c_str(), dcm::apnTypeName(q->apnType), dcm::ratName(q->rat),
                     q->family == dcm::kFamilyV6 ? "v6" : "v4", q->profile, q->seq ? static_cast<int>(*q->seq) : -1,
                     q->subscription ? static_cast<int>(*q->subscription) : -1,
                     q->slot ? static_cast<int>(*q->slot) : -1, q->instance ? static_cast<int>(*q->instance) : -1);
            report(b);
            std::lock_guard<std::mutex> l(mLock);
            for (auto& [id, e] : mPdps) {
                auto& p = e.pdp;
                if (p.client == from && p.req.apnType == q->apnType && p.req.family == q->family &&
                    (p.state == State::Up || p.state == State::Activating)) {
                    uint8_t pid = id;
                    bool up = p.state == State::Up;
                    p.req.seq = q->seq;
                    report("A6L_IMSDCM_PDP id=" + std::to_string(pid) + " already " + stateName(p.state) +
                           " (same apn type/family): re-using");
                    if (up)
                        post([this, pid] {
                            std::lock_guard<std::mutex> l2(mLock);
                            auto it = mPdps.find(pid);
                            if (it == mPdps.end()) return;
                            auto& pp = it->second.pdp;
                            mServer->indicate(pp.client, dcm::buildPdpActivateInd(0, pid, pp.req.seq, addrTlv(pp),
                                                                                  pp.req.instance));
                        });
                    return dcm::buildPdpActivateResp(0, pid, q->seq, q->instance);
                }
            }
            uint8_t id = allocId();
            if (!id) return dcm::buildPdpActivateResp(kErrNoMemory, 0, q->seq, q->instance);
            Entry e;
            e.pdp.id = id;
            e.pdp.client = from;
            e.pdp.req = *q;
            mPdps[id] = std::move(e);
            post([this, id] { activate(id); });
            return dcm::buildPdpActivateResp(0, id, q->seq, q->instance);
        }
        case dcm::kPdpDeactivate:
        case dcm::kGetIpAddress: {
            auto* v = m.get(0x01);
            std::optional<uint32_t> inst;
            if (auto* x = m.get(0x10); x && x->size() >= 4) inst = Reader(*x).u32();
            if (!v || v->empty()) return dcm::buildSimpleResp(m.msgId, kErrMissingArgument);
            uint8_t id = (*v)[0];
            std::lock_guard<std::mutex> l(mLock);
            auto it = mPdps.find(id);
            snprintf(b, sizeof b, "A6L_IMSDCM_REQ from=%u:%u %s id=%u known=%d", from.node, from.port,
                     dcm::msgName(m.msgId), id, it != mPdps.end());
            report(b);
            if (it == mPdps.end()) return dcm::buildSimpleResp(m.msgId, kErrInvalidHandle, id, inst);
            if (m.msgId == dcm::kPdpDeactivate) {
                post([this, id] { release(id, false, "deactivated by modem"); });
            } else if (it->second.pdp.state == State::Up) {
                post([this, id] {
                    std::lock_guard<std::mutex> l2(mLock);
                    auto i2 = mPdps.find(id);
                    if (i2 == mPdps.end()) return;
                    auto& pp = i2->second.pdp;
                    mServer->indicate(pp.client, dcm::buildAddressInd(dcm::kGetIpAddress, id, addrTlv(pp),
                                                                      pp.req.instance));
                    report("A6L_IMSDCM_IND GET_IP_ADDRESS id=" + std::to_string(id) + " addr=" + pp.address);
                });
            }
            return dcm::buildSimpleResp(m.msgId, 0, id, inst);
        }
        case dcm::kLinkAddr: {
            std::string d = "?";
            if (auto* v = m.get(0x01)) {
                Reader r(*v);
                uint16_t port = r.u16();
                uint32_t fam = r.u32();
                auto a = r.bytes8();
                if (r.good()) {
                    bool text = true;
                    for (uint8_t c : a) text = text && ((c >= '0' && c <= '9') || c == '.' || c == ':' || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == 0);
                    d = "port=" + std::to_string(port) + " family=" + (fam ? "v6" : "v4") +
                        " addr=" + (text ? std::string(a.begin(), a.end()) : hex(a));
                }
            }
            report(std::string("A6L_IMSDCM_REQ LINK_ADDR ") + d + " -> ok");
            return dcm::buildSimpleResp(m.msgId, 0);
        }
        case dcm::kWlanTz: {  // volte5: stock answers success with the local time (Wi-Fi calling only)
            uint8_t pdp = 0;
            uint32_t seq = 0;
            if (auto* v = m.get(0x01); v && !v->empty()) pdp = (*v)[0];
            if (auto* v = m.get(0x02); v && v->size() >= 4) seq = Reader(*v).u32();
            report("A6L_IMSDCM_REQ from=" + std::to_string(from.node) + ":" + std::to_string(from.port) +
                   " 0x0032 WLAN_TZ pdp=" + std::to_string(pdp) + " -> ok (local time)");
            return dcm::buildWlanTzResp(pdp, seq, time(nullptr));
        }
        case dcm::kAppStateReq:
        case dcm::kSubDestroyInstance:
        case dcm::kServiceEnableStatus: {  // volte5: stock = plain success (see ims.h)
            std::string d;
            std::optional<uint32_t> inst;
            if (m.msgId == dcm::kAppStateReq) {
                auto* a = m.get(0x10);
                auto* i = m.get(0x11);
                if (i && i->size() >= 4) inst = Reader(*i).u32();
                d = std::string("app=") + (a && !a->empty() ? std::to_string((*a)[0]) : "-") + " instance=" +
                    (i && i->size() >= 4 ? std::to_string(Reader(*i).u32()) : "-");
            } else if (m.msgId == dcm::kSubDestroyInstance) {
                auto* i = m.get(0x01);
                if (i && i->size() >= 4) inst = Reader(*i).u32();
                d = "instance=" + (inst ? std::to_string(*inst) : std::string("-"));
            } else {
                auto* i = m.get(0x01);
                uint64_t mask = 0;
                if (i && i->size() >= 8)
                    for (int k = 7; k >= 0; k--) mask = (mask << 8) | (*i)[k];
                char mb[40];
                snprintf(mb, sizeof mb, "rcs_mask=0x%llx", static_cast<unsigned long long>(mask));
                d = mb;
            }
            snprintf(b, sizeof b, "A6L_IMSDCM_REQ from=%u:%u 0x%04x %s %s -> ok (stock behaviour)", from.node, from.port,
                     m.msgId, dcm::msgName(m.msgId), d.c_str());
            report(std::string(b) + " " + m.dump());
            if (m.msgId == dcm::kSubDestroyInstance)  // volte6: name it (QmiImsDcmInstanceId)
                report("A6L_IMSDCM_EVENT destroy instance=" + (inst ? std::to_string(*inst) : std::string("-")) +
                       " (" + (inst ? dcm::instanceName(*inst) : "?") + ")");
            if (mEvent) mEvent(m.msgId, inst);
            // Stock (imsdatadaemon 0x18a34 / 0x18e10 / 0x18f90): qmi_csi_send_resp with a zeroed result
            // struct -> the wire answer is the result TLV only: 02 04 00 00 00 00 00.
            return dcm::buildSimpleResp(m.msgId, 0);
        }
        default: {
            uint16_t err = mCfg.ackUnknown ? 0 : kErrNotSupported;
            snprintf(b, sizeof b, "A6L_IMSDCM_REQ from=%u:%u 0x%04x %s -> %s", from.node, from.port, m.msgId,
                     dcm::msgName(m.msgId), err ? "not-supported" : "ok");
            report(std::string(b) + " " + m.dump());
            return dcm::buildSimpleResp(m.msgId, err);
        }
    }
}

void ImsDcmService::activate(uint8_t id) {
    dcm::PdpActivateReq q;
    Addr client;
    {
        std::lock_guard<std::mutex> l(mLock);
        auto it = mPdps.find(id);
        if (it == mPdps.end()) return;
        q = it->second.pdp.req;
        client = it->second.pdp.client;
    }
    bool v6 = q.family == dcm::kFamilyV6;
    std::string detail;
    uint16_t err = 0;
    std::unique_ptr<Client> wds;
    wds::StartResult sr;
    std::string addr;
    if (mCfg.noWds) {
        err = kErrCallFailed;
        detail = "noWds (test mode)";
    } else {
        wds = mFactory ? mFactory(v6 ? "imsdcm-wds6" : "imsdcm-wds4") : nullptr;
        if (!wds || !wds->start({kSvcWds}) || !wds->waitForServices({kSvcWds}, 5000).empty()) {
            err = kErrInternal;
            detail = "no WDS service";
        }
    }
    // r5 bug hunt round2 R4: the session observers are installed before START (like the data calls, F60). Before,
    // the packet status callback was attached only after START + Get Current Settings, so a DISCONNECTED in that
    // window was consumed unseen and the modem was told the IMS PDN was up; a WDS service loss (WDS restart /
    // transport failure) was never observed at all, leaving the PDN "up" with a dead handle.
    auto watch = std::make_shared<Watch>();
    if (!err) {
        auto lost = [this, id, watch](const std::string& why) {
            if (watch->ended.exchange(true)) return;
            report("A6L_IMSDCM_PDP_LOST id=" + std::to_string(id) + " " + why);
            if (watch->published) post([this, id] { release(id, true, "network released the PDN"); });
        };
        wds->onIndication(kSvcWds, wds::kPacketServiceStatus, [lost](const Message& m) {
            auto ps = wds::parsePacketStatus(m);
            if (ps.status == wds::kConnDisconnected)
                lost("end=" + std::to_string(ps.callEndReason ? *ps.callEndReason : -1));
        });
        wds->onServiceChange([lost](uint32_t svc, bool up) {
            if (svc == kSvcWds && !up) lost("(WDS service gone)");
        });
    }
    if (!err && mCfg.muxId) {
        auto r = wds::bindMuxDataPort(*wds, mCfg.epType, mCfg.epIface, mCfg.muxId);
        detail += " bind-mux" + std::to_string(mCfg.muxId) + "=" + (r.ok() ? "ok" : r.describe());
    }
    if (!err) {
        auto r = wds::setIpFamily(*wds, v6 ? wds::kFamilyV6 : wds::kFamilyV4);
        if (!r.ok()) detail += " set-family=" + r.describe();
        wds::StartParams p;
        p.ipFamily = v6 ? wds::kFamilyV6 : wds::kFamilyV4;
        int prof = mCfg.profileOverride > 0 ? mCfg.profileOverride : static_cast<int>(q.profile);
        if (prof > 0 && prof < 256)
            p.profileIndex3gpp = static_cast<uint8_t>(prof);
        else
            p.apn = q.apn;
        r = wds::startNetwork(*wds, p, &sr, mCfg.startTimeoutMs);
        detail += std::string(" start(") + (p.profileIndex3gpp ? "profile " + std::to_string(prof) : "apn " + q.apn) +
                  ")=" + (r.ok() ? "ok" : r.describe());
        if (!r.ok() && !(r.status == Result::QmiFailure && r.qmiError == kErrNoEffect)) {
            err = r.status == Result::QmiFailure ? r.qmiError : static_cast<uint16_t>(kErrCallFailed);
            if (sr.callEndReason) detail += " end=" + std::to_string(*sr.callEndReason);
            if (sr.verboseReason)
                detail += " verbose=" + std::to_string(sr.verboseReason->first) + "/" +
                          std::to_string(sr.verboseReason->second);
        } else {
            wds::Settings s;
            auto g = wds::getCurrentSettings(*wds, &s);
            if (v6 && s.ipv6) addr = ipv6ToString(s.ipv6->data());
            else if (!v6 && s.ipv4) addr = ipv4ToString(*s.ipv4);
            if (!g.ok()) detail += " settings=" + g.describe();
            if (!s.pcscf4.empty()) detail += " pcscf4=" + ipv4ToString(s.pcscf4[0]);
            if (s.mtu) detail += " mtu=" + std::to_string(*s.mtu);
        }
    }
    if (err && wds) {
        wds->stop();
        wds.reset();
    }
    std::lock_guard<std::mutex> l(mLock);
    if (!err) {
        watch->published = true;  // from here a loss is reported by release(); before, it fails this activation
        if (watch->ended) {
            err = kErrCallFailed;
            detail += " ended-during-setup";
        }
    }
    auto it = mPdps.find(id);
    if (it == mPdps.end()) {  // deactivated meanwhile (the release job ran first)
        if (wds && sr.handle) wds::stopNetwork(*wds, sr.handle);
        return;
    }
    auto& p = it->second.pdp;
    p.detail = detail;
    p.handle = sr.handle;
    p.address = addr;
    p.state = err ? State::Failed : State::Up;
    it->second.wds = std::move(wds);
    char b[512];
    snprintf(b, sizeof b, "A6L_IMSDCM_PDP id=%u apn='%s' family=%s state=%s addr=%s handle=0x%x err=%s |%s", id,
             q.apn.c_str(), v6 ? "v6" : "v4", stateName(p.state), addr.empty() ? "-" : addr.c_str(), sr.handle,
             err ? errorName(err).c_str() : "none", detail.c_str());
    report(b);
    mServer->indicate(client, dcm::buildPdpActivateInd(err, id, q.seq,
                                                       err ? std::nullopt : std::optional(addrTlv(p)), q.instance));
    report(std::string("A6L_IMSDCM_IND PDP_ACTIVATE id=") + std::to_string(id) + (err ? " FAILURE" : " success") +
           (addr.empty() ? "" : " addr=" + addr));
    if (err) mPdps.erase(it);
}

void ImsDcmService::release(uint8_t id, bool notifyLost, const char* why) {
    Entry e;
    {
        std::lock_guard<std::mutex> l(mLock);
        auto it = mPdps.find(id);
        if (it == mPdps.end()) return;
        e = std::move(it->second);
        mPdps.erase(it);
    }
    std::string res = "no call";
    if (e.wds) {
        if (e.pdp.handle && !notifyLost) {
            auto r = wds::stopNetwork(*e.wds, e.pdp.handle);
            res = "stop=" + (r.ok() ? std::string("ok") : r.describe());
        } else {
            res = notifyLost ? "released by network" : "no handle";
        }
        e.wds->stop();
    }
    report("A6L_IMSDCM_PDP id=" + std::to_string(id) + " state=down (" + why + ") " + res);
    if (notifyLost) {
        // stock: PDP_ACTIVATE_IND with a failure result on eCS_ENETNONET (there is no deactivate ind)
        mServer->indicate(e.pdp.client, dcm::buildPdpActivateInd(kErrCallFailed, id, e.pdp.req.seq, std::nullopt,
                                                                 e.pdp.req.instance));
        report("A6L_IMSDCM_IND PDP_ACTIVATE id=" + std::to_string(id) + " FAILURE (lost)");
    }
}

}  // namespace a6l::radio
