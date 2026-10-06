// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): AP-hosted QMI service over QRTR (see server.h).
#include <a6lqmi/log.h>
#include <a6lqmi/server.h>

namespace a6l::qmi {

Server::Server(std::unique_ptr<Transport> t, uint32_t service, uint32_t version, uint32_t instance,
               const char* tag)
    : mT(std::move(t)), mService(service), mVersion(version), mInstance(instance), mTag(tag) {}

Server::~Server() { stop(); }

void Server::publish(uint32_t cmd) {
    CtrlPacket p;
    p.cmd = cmd;
    p.service = mService;
    p.instance = instanceWord();
    p.node = mT->localNode();
    p.port = mT->localPort();  // the kernel ns also takes the sender's address for local servers
    std::lock_guard<std::mutex> l(mSendLock);
    mT->send(mT->nameService(), p.encode());
}

bool Server::start() {
    if (mRunning) return true;
    if (!mT->open()) return false;
    mRunning = true;
    mReader = std::thread([this] { readerLoop(); });
    publish(kCtrlNewServer);
    ALOGI_Q("%s: published service %u (0x%x) instance word 0x%x", mTag.c_str(), mService, mService,
            instanceWord());
    return true;
}

void Server::stop() {
    if (!mRunning.exchange(false)) return;
    publish(kCtrlDelServer);
    if (mReader.joinable()) mReader.join();
    mT->close();
    ALOGI_Q("%s: service %u withdrawn", mTag.c_str(), mService);
}

bool Server::respond(const Addr& to, uint16_t txn, Message resp) {
    resp.type = MsgType::Response;
    resp.txn = txn;
    ALOGV_Q("%s: -> %u:%u %s", mTag.c_str(), to.node, to.port, resp.dump().c_str());
    std::lock_guard<std::mutex> l(mSendLock);
    return mT->send(to, resp.encode());
}

bool Server::indicate(const Addr& to, Message ind) {
    ind.type = MsgType::Indication;
    ind.txn = 0;
    ALOGV_Q("%s: ind -> %u:%u %s", mTag.c_str(), to.node, to.port, ind.dump().c_str());
    std::lock_guard<std::mutex> l(mSendLock);
    return mT->send(to, ind.encode());
}

std::set<Addr> Server::clients() const {
    std::lock_guard<std::mutex> l(mLock);
    return mClients;
}

void Server::readerLoop() {
    std::vector<uint8_t> buf;
    Addr from;
    while (mRunning) {
        int r = mT->recv(&from, &buf, 200);
        if (r == 0) continue;
        if (r < 0) {
            if (mRunning) ALOGE_Q("%s: transport error, reader stops", mTag.c_str());
            break;
        }
        if (from.port == kPortCtrl) {
            CtrlPacket p;
            if (!CtrlPacket::decode(buf, &p)) continue;
            if (p.cmd == kCtrlDelClient || p.cmd == kCtrlBye) {
                bool node = p.cmd == kCtrlBye;
                std::vector<Addr> gone;
                {
                    std::lock_guard<std::mutex> l(mLock);
                    for (auto it = mClients.begin(); it != mClients.end();) {
                        bool match = node ? it->node == p.node : (it->node == p.node && it->port == p.port);
                        if (match) {
                            gone.push_back(*it);
                            it = mClients.erase(it);
                        } else {
                            ++it;
                        }
                    }
                }
                for (auto& a : gone) {
                    ALOGI_Q("%s: client %u:%u gone (%s)", mTag.c_str(), a.node, a.port,
                            node ? "BYE" : "DEL_CLIENT");
                    if (mGone) mGone(a, node);
                }
            }
            continue;
        }
        auto m = Message::decode(buf);
        if (!m) {
            ALOGW_Q("%s: undecodable packet from %u:%u (%zu bytes)", mTag.c_str(), from.node,
                    from.port, buf.size());
            continue;
        }
        if (m->type != MsgType::Request) {
            ALOGW_Q("%s: ignoring non-request type %u msg 0x%04x from %u:%u", mTag.c_str(),
                    static_cast<unsigned>(m->type), m->msgId, from.node, from.port);
            continue;
        }
        mRequests++;
        {
            std::lock_guard<std::mutex> l(mLock);
            mClients.insert(from);
        }
        ALOGV_Q("%s: <- %u:%u %s", mTag.c_str(), from.node, from.port, m->dump().c_str());
        std::optional<Message> resp;
        if (mReq) resp = mReq(from, *m);
        if (resp) {
            resp->msgId = m->msgId;
            respond(from, m->txn, std::move(*resp));
        }
    }
}

}  // namespace a6l::qmi
