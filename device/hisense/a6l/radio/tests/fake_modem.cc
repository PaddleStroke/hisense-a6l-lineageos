// SPDX-License-Identifier: Apache-2.0
#include "fake_modem.h"

#include <chrono>

namespace a6l::test {

using namespace a6l::qmi;

bool FakeTransport::open() {
    mOpen = true;
    mModem->attach(this);
    return true;
}
void FakeTransport::close() {
    if (!mOpen) return;
    mOpen = false;
    mModem->detach(this);
    std::lock_guard<std::mutex> l(mLock);
    mCv.notify_all();
}
bool FakeTransport::send(const Addr& to, const std::vector<uint8_t>& data) {
    if (!mOpen) return false;
    mModem->fromClient(this, to, data);
    return true;
}
int FakeTransport::recv(Addr* from, std::vector<uint8_t>* data, int timeoutMs) {
    std::unique_lock<std::mutex> l(mLock);
    mCv.wait_for(l, std::chrono::milliseconds(timeoutMs), [&] { return !mQ.empty() || !mOpen; });
    if (mQ.empty()) return mOpen ? 0 : -1;
    *from = mQ.front().first;
    *data = std::move(mQ.front().second);
    mQ.pop_front();
    return 1;
}
void FakeTransport::deliver(const Addr& from, std::vector<uint8_t> data) {
    std::lock_guard<std::mutex> l(mLock);
    mQ.emplace_back(from, std::move(data));
    mCv.notify_all();
}

FakeModem::FakeModem() {}

std::unique_ptr<Transport> FakeModem::transport() { return std::make_unique<FakeTransport>(this); }

void FakeModem::attach(FakeTransport* t) {
    std::lock_guard<std::mutex> l(mLock);
    t->mPort = mNextClientPort++;
    mClients.push_back(t);
}
void FakeModem::detach(FakeTransport* t) {
    std::lock_guard<std::mutex> l(mLock);
    for (auto it = mClients.begin(); it != mClients.end();)
        it = (*it == t) ? mClients.erase(it) : it + 1;
    for (auto it = mLookups.begin(); it != mLookups.end();)
        it = (it->first == t) ? mLookups.erase(it) : it + 1;
}

static std::vector<uint8_t> ctrl(uint32_t cmd, uint32_t svc, uint32_t inst, uint32_t node,
                                 uint32_t port) {
    CtrlPacket p;
    p.cmd = cmd;
    p.service = svc;
    p.instance = inst;
    p.node = node;
    p.port = port;
    return p.encode();
}

void FakeModem::addService(uint32_t svc, uint32_t instance) {
    std::vector<std::pair<FakeTransport*, std::vector<uint8_t>>> out;
    {
        std::lock_guard<std::mutex> l(mLock);
        uint32_t port = mNextPort++;
        mSvc[svc] = {port, instance};
        for (auto& [t, s] : mLookups)
            if (s == 0 || s == svc) out.push_back({t, ctrl(kCtrlNewServer, svc, instance, kModemNode, port)});
    }
    for (auto& [t, d] : out) t->deliver(Addr{1, kPortCtrl}, d);
}

void FakeModem::removeService(uint32_t svc) {
    std::vector<std::pair<FakeTransport*, std::vector<uint8_t>>> out;
    {
        std::lock_guard<std::mutex> l(mLock);
        auto it = mSvc.find(svc);
        if (it == mSvc.end()) return;
        for (auto& [t, s] : mLookups)
            if (s == 0 || s == svc)
                out.push_back({t, ctrl(kCtrlDelServer, svc, it->second.second, kModemNode, it->second.first)});
        mSvc.erase(it);
    }
    for (auto& [t, d] : out) t->deliver(Addr{1, kPortCtrl}, d);
}

void FakeModem::indicate(uint32_t svc, Message ind) {
    ind.type = MsgType::Indication;
    ind.txn = 0;
    std::vector<FakeTransport*> cl;
    uint32_t port;
    {
        std::lock_guard<std::mutex> l(mLock);
        auto it = mSvc.find(svc);
        if (it == mSvc.end()) return;
        port = it->second.first;
        cl = mClients;
    }
    for (auto* t : cl) t->deliver(Addr{kModemNode, port}, ind.encode());
}

std::vector<std::pair<uint32_t, Message>> FakeModem::requests() {
    std::lock_guard<std::mutex> l(mLock);
    return mLog;
}

void FakeModem::fromClient(FakeTransport* t, const Addr& to, const std::vector<uint8_t>& data) {
    if (to.port == kPortCtrl) {
        CtrlPacket p;
        if (!CtrlPacket::decode(data, &p) || p.cmd != kCtrlNewLookup) return;
        std::vector<std::vector<uint8_t>> out;
        {
            std::lock_guard<std::mutex> l(mLock);
            mLookups.push_back({t, p.service});
            for (auto& [svc, pi] : mSvc)
                if (p.service == 0 || p.service == svc)
                    out.push_back(ctrl(kCtrlNewServer, svc, pi.second, kModemNode, pi.first));
            out.push_back(ctrl(kCtrlNewServer, 0, 0, 0, 0));
        }
        for (auto& d : out) t->deliver(Addr{1, kPortCtrl}, d);
        return;
    }
    uint32_t svc = 0;
    {
        std::lock_guard<std::mutex> l(mLock);
        for (auto& [s, pi] : mSvc)
            if (pi.first == to.port && to.node == kModemNode) svc = s;
    }
    if (!svc) return;
    auto m = Message::decode(data);
    if (!m) return;
    {
        std::lock_guard<std::mutex> l(mLock);
        mLog.push_back({svc, *m});
    }
    if (!mHandler) return;
    auto resp = mHandler(svc, *m);
    if (!resp) return;
    resp->type = MsgType::Response;
    resp->txn = m->txn;
    resp->msgId = m->msgId;
    t->deliver(to, resp->encode());
}

Message okResponse(uint16_t msgId) {
    Message m(MsgType::Response, msgId);
    m.raw(kTlvResult, {0, 0, 0, 0});
    return m;
}
Message errResponse(uint16_t msgId, uint16_t err) {
    Message m(MsgType::Response, msgId);
    m.raw(kTlvResult, {1, 0, static_cast<uint8_t>(err & 0xff), static_cast<uint8_t>(err >> 8)});
    return m;
}

}  // namespace a6l::test
