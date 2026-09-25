// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): QMI client over QRTR.
#include <a6lqmi/client.h>
#include <a6lqmi/log.h>

#include <chrono>

namespace a6l::qmi {

const char* serviceName(uint32_t svc) {
    switch (svc) {
        case kSvcWds: return "WDS";
        case kSvcDms: return "DMS";
        case kSvcNas: return "NAS";
        case kSvcWms: return "WMS";
        case kSvcVoice: return "VOICE";
        case kSvcUim: return "UIM";
        case kSvcPbm: return "PBM";
        case kSvcLoc: return "LOC";
        case kSvcWda: return "WDA";
        case kSvcImsa: return "IMSA";
        case kSvcDpm: return "DPM";
        default: return "SVC?";
    }
}

static void put32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back((v >> (8 * i)) & 0xff);
}
static uint32_t get32(const std::vector<uint8_t>& b, size_t off) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(b[off + i]) << (8 * i);
    return v;
}

std::vector<uint8_t> CtrlPacket::encodeLookup(uint32_t service, uint32_t instance) {
    CtrlPacket p;
    p.cmd = kCtrlNewLookup;
    p.service = service;
    p.instance = instance;
    return p.encode();
}

std::vector<uint8_t> CtrlPacket::encode() const {
    std::vector<uint8_t> o;
    put32(o, cmd);
    if (cmd == kCtrlBye || cmd == kCtrlDelClient) {
        put32(o, node);
        put32(o, port);
        put32(o, 0);
        put32(o, 0);
    } else {
        put32(o, service);
        put32(o, instance);
        put32(o, node);
        put32(o, port);
    }
    return o;
}

bool CtrlPacket::decode(const std::vector<uint8_t>& b, CtrlPacket* p) {
    if (b.size() < 4) return false;
    *p = CtrlPacket{};
    p->cmd = get32(b, 0);
    if (p->cmd == kCtrlBye || p->cmd == kCtrlDelClient) {
        if (b.size() >= 12) {
            p->node = get32(b, 4);
            p->port = get32(b, 8);
        }
        return true;
    }
    if (b.size() < 20) return p->cmd == kCtrlHello || p->cmd == kCtrlPing;
    p->service = get32(b, 4);
    p->instance = get32(b, 8);
    p->node = get32(b, 12);
    p->port = get32(b, 16);
    return true;
}

std::string Result::describe() const {
    switch (status) {
        case Ok: return "ok";
        case QmiFailure: return "qmi-error " + errorName(qmiError);
        case Timeout: return "timeout";
        case NoService: return "no-service";
        case TransportError: return "transport-error";
        case Stopped: return "stopped";
    }
    return "?";
}

Client::Client(std::unique_ptr<Transport> t, const char* tag) : mT(std::move(t)), mTag(tag) {}

Client::~Client() { stop(); }

bool Client::start(const std::vector<uint32_t>& services) {
    if (mRunning) return true;
    if (!mT->open()) {
        ALOGE_Q("%s: transport open failed", mTag.c_str());
        return false;
    }
    mRunning = true;
    mReader = std::thread([this] { readerLoop(); });
    mDispatcher = std::thread([this] { dispatchLoop(); });
    for (uint32_t s : services) {
        if (!mT->send(mT->nameService(), CtrlPacket::encodeLookup(s, 0))) {
            ALOGE_Q("%s: lookup send failed for %s", mTag.c_str(), serviceName(s));
        }
    }
    return true;
}

void Client::stop() {
    if (!mRunning.exchange(false)) return;
    {
        std::lock_guard<std::mutex> l(mLock);
        for (auto& [k, p] : mPending) {
            p->done = true;
            p->res.status = Result::Stopped;
        }
        mCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> l(mEvLock);
        mEvCv.notify_all();
    }
    if (mReader.joinable()) mReader.join();
    if (mDispatcher.joinable()) mDispatcher.join();
    mT->close();
}

void Client::readerLoop() {
    Addr from;
    std::vector<uint8_t> buf;
    while (mRunning) {
        int r = mT->recv(&from, &buf, 200);
        if (r < 0) {
            ALOGE_Q("%s: recv error, reader exits", mTag.c_str());
            break;
        }
        if (r == 0) continue;
        if (from.port == kPortCtrl) {
            handleCtrl(buf);
        } else {
            handleData(from, buf);
        }
    }
}

void Client::post(Event e) {
    std::lock_guard<std::mutex> l(mEvLock);
    mEvents.push_back(std::move(e));
    mEvCv.notify_one();
}

void Client::dispatchLoop() {
    while (true) {
        std::vector<Event> evs;
        {
            std::unique_lock<std::mutex> l(mEvLock);
            mEvCv.wait_for(l, std::chrono::milliseconds(200),
                           [this] { return !mEvents.empty() || !mRunning; });
            if (!mRunning && mEvents.empty()) return;
            evs.swap(mEvents);
        }
        for (auto& e : evs) {
            if (e.kind == Event::Svc) {
                std::vector<ServiceFn> fns;
                {
                    std::lock_guard<std::mutex> l(mLock);
                    fns = mSvcFns;
                }
                for (auto& f : fns) f(e.svc, e.up);
            } else {
                std::vector<IndicationFn> fns;
                {
                    std::lock_guard<std::mutex> l(mLock);
                    auto it = mInd.find({e.svc, e.msg.msgId});
                    if (it != mInd.end()) fns = it->second;
                }
                if (fns.empty()) {
                    ALOGD_Q("%s: unhandled indication %s 0x%04x", mTag.c_str(),
                            serviceName(e.svc), e.msg.msgId);
                }
                for (auto& f : fns) f(e.msg);
            }
        }
    }
}

void Client::handleCtrl(const std::vector<uint8_t>& data) {
    CtrlPacket p;
    if (!CtrlPacket::decode(data, &p)) return;
    std::vector<Event> evs;
    {
        std::lock_guard<std::mutex> l(mLock);
        if (p.cmd == kCtrlNewServer) {
            if (p.service == 0 && p.node == 0 && p.port == 0) return;  // end of initial listing
            Addr a{p.node, p.port};
            bool wasUp = !mServers[p.service].empty();
            mServers[p.service][a] = p.instance;
            mAddrToSvc[a] = p.service;
            ALOGI_Q("%s: server %s(%u) inst %u at %u:%u", mTag.c_str(), serviceName(p.service),
                    p.service, p.instance, p.node, p.port);
            if (!wasUp) evs.push_back({Event::Svc, p.service, true, {}});
        } else if (p.cmd == kCtrlDelServer || p.cmd == kCtrlBye) {
            // DEL_SERVER names one server; BYE drops every server of a node.
            for (auto sit = mServers.begin(); sit != mServers.end(); ++sit) {
                auto& m = sit->second;
                bool had = !m.empty();
                for (auto it = m.begin(); it != m.end();) {
                    bool match = p.cmd == kCtrlBye
                                         ? it->first.node == p.node
                                         : (it->first.node == p.node && it->first.port == p.port);
                    if (match) {
                        mAddrToSvc.erase(it->first);
                        it = m.erase(it);
                    } else {
                        ++it;
                    }
                }
                if (had && m.empty()) {
                    ALOGW_Q("%s: service %s gone", mTag.c_str(), serviceName(sit->first));
                    evs.push_back({Event::Svc, sit->first, false, {}});
                    // fail pending requests of this service
                    for (auto& [k, pend] : mPending) {
                        if (k.first == sit->first && !pend->done) {
                            pend->done = true;
                            pend->res.status = Result::NoService;
                        }
                    }
                }
            }
        }
        mCv.notify_all();
    }
    for (auto& e : evs) post(std::move(e));
}

void Client::handleData(const Addr& from, const std::vector<uint8_t>& data) {
    auto m = Message::decode(data);
    if (!m) {
        ALOGW_Q("%s: undecodable packet from %u:%u (%zu bytes)", mTag.c_str(), from.node,
                from.port, data.size());
        return;
    }
    uint32_t svc;
    {
        std::lock_guard<std::mutex> l(mLock);
        auto it = mAddrToSvc.find(from);
        if (it == mAddrToSvc.end()) {
            ALOGW_Q("%s: packet from unknown server %u:%u msg 0x%04x", mTag.c_str(), from.node,
                    from.port, m->msgId);
            return;
        }
        svc = it->second;
        if (m->type == MsgType::Response) {
            auto pit = mPending.find({svc, m->txn});
            if (pit == mPending.end() || pit->second->done) {
                ALOGW_Q("%s: stray response %s txn %u msg 0x%04x", mTag.c_str(),
                        serviceName(svc), m->txn, m->msgId);
                return;
            }
            auto& res = pit->second->res;
            res.msg = std::move(*m);
            if (!res.msg.hasResult()) {
                res.status = Result::QmiFailure;
                res.qmiError = kErrMalformedMessage;
            } else if (res.msg.resultOk()) {
                res.status = Result::Ok;
            } else {
                res.status = Result::QmiFailure;
                res.qmiError = res.msg.error();
            }
            pit->second->done = true;
            mCv.notify_all();
            return;
        }
    }
    if (m->type == MsgType::Indication) {
        post({Event::Ind, svc, false, std::move(*m)});
    }
}

bool Client::addrFor(uint32_t svc, Addr* out) const {
    auto it = mServers.find(svc);
    if (it == mServers.end() || it->second.empty()) return false;
    const Addr* best = nullptr;
    uint32_t bestInst = 0;
    bool bestRemote = false;
    uint32_t local = mT->localNode();
    for (const auto& [a, inst] : it->second) {
        bool remote = a.node != local;
        bool better = best == nullptr || (mPreferRemote && remote && !bestRemote) ||
                      (remote == bestRemote && inst < bestInst);
        if (better) {
            best = &a;
            bestInst = inst;
            bestRemote = remote;
        }
    }
    *out = *best;
    return true;
}

bool Client::hasService(uint32_t svc) const {
    std::lock_guard<std::mutex> l(mLock);
    Addr a;
    return addrFor(svc, &a);
}

std::map<uint32_t, std::vector<std::pair<Addr, uint32_t>>> Client::servers() const {
    std::lock_guard<std::mutex> l(mLock);
    std::map<uint32_t, std::vector<std::pair<Addr, uint32_t>>> r;
    for (const auto& [s, m] : mServers)
        for (const auto& [a, i] : m) r[s].push_back({a, i});
    return r;
}

std::vector<uint32_t> Client::waitForServices(const std::vector<uint32_t>& services,
                                              int timeoutMs) {
    std::unique_lock<std::mutex> l(mLock);
    auto missing = [&] {
        std::vector<uint32_t> m;
        Addr a;
        for (auto s : services)
            if (!addrFor(s, &a)) m.push_back(s);
        return m;
    };
    mCv.wait_for(l, std::chrono::milliseconds(timeoutMs),
                 [&] { return missing().empty() || !mRunning; });
    return missing();
}

Result Client::request(uint32_t svc, Message req, int timeoutMs) {
    Result r;
    if (!mRunning) return r;
    Addr to;
    auto pend = std::make_shared<Pending>();
    uint16_t txn;
    {
        std::lock_guard<std::mutex> l(mLock);
        if (!addrFor(svc, &to)) {
            r.status = Result::NoService;
            return r;
        }
        txn = ++mTxn[svc];
        if (txn == 0) txn = ++mTxn[svc];
        mPending[{svc, txn}] = pend;
    }
    req.type = MsgType::Request;
    req.txn = txn;
    ALOGV_Q("%s: -> %s %s", mTag.c_str(), serviceName(svc), req.dump().c_str());
    if (!mT->send(to, req.encode())) {
        std::lock_guard<std::mutex> l(mLock);
        mPending.erase({svc, txn});
        r.status = Result::TransportError;
        return r;
    }
    std::unique_lock<std::mutex> l(mLock);
    bool got = mCv.wait_for(l, std::chrono::milliseconds(timeoutMs),
                            [&] { return pend->done || !mRunning; });
    mPending.erase({svc, txn});
    if (!got) {
        r.status = Result::Timeout;
        ALOGW_Q("%s: %s msg 0x%04x timed out after %d ms", mTag.c_str(), serviceName(svc),
                req.msgId, timeoutMs);
        return r;
    }
    if (!pend->done) {
        r.status = Result::Stopped;
        return r;
    }
    ALOGV_Q("%s: <- %s %s", mTag.c_str(), serviceName(svc), pend->res.msg.dump().c_str());
    return pend->res;
}

void Client::onIndication(uint32_t svc, uint16_t msgId, IndicationFn fn) {
    std::lock_guard<std::mutex> l(mLock);
    mInd[{svc, msgId}].push_back(std::move(fn));
}

void Client::onServiceChange(ServiceFn fn) {
    std::lock_guard<std::mutex> l(mLock);
    mSvcFns.push_back(std::move(fn));
}

}  // namespace a6l::qmi
