// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): in-process fake QRTR name service + fake modem for offline tests.
#pragma once

#include <a6lqmi/client.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>

namespace a6l::test {

using a6l::qmi::Addr;
using a6l::qmi::Message;

class FakeModem;

class FakeTransport : public qmi::Transport {
  public:
    explicit FakeTransport(FakeModem* m) : mModem(m) {}
    bool open() override;
    void close() override;
    uint32_t localNode() const override { return 1; }
    bool send(const Addr& to, const std::vector<uint8_t>& data) override;
    int recv(Addr* from, std::vector<uint8_t>* data, int timeoutMs) override;
    void deliver(const Addr& from, std::vector<uint8_t> data);
    uint32_t port() const { return mPort; }

  private:
    FakeModem* mModem;
    uint32_t mPort = 0;
    std::mutex mLock;
    std::condition_variable mCv;
    std::deque<std::pair<Addr, std::vector<uint8_t>>> mQ;
    bool mOpen = false;
    friend class FakeModem;
};

// Handler returns the response message (type/txn are filled in) or nullopt to stay silent.
using Handler = std::function<std::optional<Message>(uint32_t svc, const Message& req)>;

class FakeModem {
  public:
    static constexpr uint32_t kModemNode = 0;
    FakeModem();
    std::unique_ptr<qmi::Transport> transport();
    void addService(uint32_t svc, uint32_t instance = 0);
    void removeService(uint32_t svc);
    void setHandler(Handler h) { mHandler = std::move(h); }
    // Send an indication to every open client (all of them registered, as far as the fake cares).
    void indicate(uint32_t svc, Message ind);
    std::vector<std::pair<uint32_t, Message>> requests();  // log of received requests

    // called by FakeTransport
    void attach(FakeTransport* t);
    void detach(FakeTransport* t);
    void fromClient(FakeTransport* t, const Addr& to, const std::vector<uint8_t>& data);

  private:
    std::mutex mLock;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> mSvc;  // svc -> (port, instance)
    std::vector<FakeTransport*> mClients;
    std::vector<std::pair<FakeTransport*, uint32_t>> mLookups;
    std::vector<std::pair<uint32_t, Message>> mLog;
    uint32_t mNextPort = 100;
    uint32_t mNextClientPort = 5000;
    Handler mHandler;
    friend class FakeTransport;
};

// Build a response with result TLV (ok or error) and extra TLVs.
Message okResponse(uint16_t msgId);
Message errResponse(uint16_t msgId, uint16_t err);

}  // namespace a6l::test
