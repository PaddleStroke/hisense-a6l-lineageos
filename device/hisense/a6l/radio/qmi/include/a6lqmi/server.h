// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): host a QMI *service* on the AP over QRTR.
//
// Some modem features need an AP-side QMI server (stock: QCSI servers in vendor daemons), e.g.
// IMSDCM (770) for the IMS PDN. Publishing = sending QRTR NEW_SERVER {service, major | inst<<8}
// to the local name service from the serving socket (the kernel ns takes node/port from the
// source address for local servers and broadcasts it to the modem; it re-announces it after a
// modem restart on HELLO). Requests arrive from the modem's client port; DEL_CLIENT / BYE control
// packets tell us when a client (or the whole modem node) went away.
#pragma once

#include <a6lqmi/client.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace a6l::qmi {

class Server {
  public:
    // Called on the reader thread: must not block. Return a response (type/txn/msgId are
    // filled in) or nullopt to answer later with respond().
    using RequestFn = std::function<std::optional<Message>(const Addr& from, const Message& req)>;
    using GoneFn = std::function<void(const Addr& client, bool wholeNode)>;

    Server(std::unique_ptr<Transport> t, uint32_t service, uint32_t version, uint32_t instance = 0,
           const char* tag = "qmi-srv");
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    void onRequest(RequestFn fn) { mReq = std::move(fn); }
    void onClientGone(GoneFn fn) { mGone = std::move(fn); }
    bool start();  // open + NEW_SERVER + reader thread
    void stop();   // DEL_SERVER + close

    bool respond(const Addr& to, uint16_t txn, Message resp);
    bool indicate(const Addr& to, Message ind);
    std::set<Addr> clients() const;
    uint32_t instanceWord() const { return (mVersion & 0xff) | (mInstance << 8); }
    uint64_t requestsSeen() const { return mRequests; }

  private:
    void readerLoop();
    void publish(uint32_t cmd);

    std::unique_ptr<Transport> mT;
    uint32_t mService, mVersion, mInstance;
    std::string mTag;
    RequestFn mReq;
    GoneFn mGone;
    std::atomic<bool> mRunning{false};
    std::thread mReader;
    mutable std::mutex mLock;
    std::set<Addr> mClients;
    std::mutex mSendLock;
    std::atomic<uint64_t> mRequests{0};
};

}  // namespace a6l::qmi
