// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): QMI client over QRTR (AF_QIPCRTR datagram sockets).
//
// One Client = one QRTR socket = one QMI "client id" towards every service it talks to.
// Service discovery uses the in-kernel QRTR name service (NEW_LOOKUP on the control port);
// NEW_SERVER / DEL_SERVER / BYE control packets keep the service table current across modem restarts.
#pragma once

#include <a6lqmi/message.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace a6l::qmi {

// QMI service ids (== QRTR service ids)
enum Service : uint32_t {
    kSvcWds = 0x01,
    kSvcDms = 0x02,
    kSvcNas = 0x03,
    kSvcWms = 0x05,
    kSvcVoice = 0x09,
    kSvcUim = 0x0B,
    kSvcPbm = 0x0C,
    kSvcLoc = 0x10,
    kSvcWda = 0x1A,
    kSvcImsa = 0x21,
    kSvcDpm = 0x2F,
};
const char* serviceName(uint32_t svc);

struct Addr {
    uint32_t node = 0;
    uint32_t port = 0;
    bool operator<(const Addr& o) const { return node != o.node ? node < o.node : port < o.port; }
    bool operator==(const Addr& o) const { return node == o.node && port == o.port; }
};

// Control packet types (linux/qrtr.h QRTR_TYPE_*)
enum CtrlType : uint32_t {
    kCtrlData = 1,
    kCtrlHello = 2,
    kCtrlBye = 3,
    kCtrlNewServer = 4,
    kCtrlDelServer = 5,
    kCtrlDelClient = 6,
    kCtrlResumeTx = 7,
    kCtrlExit = 8,
    kCtrlPing = 9,
    kCtrlNewLookup = 10,
    kCtrlDelLookup = 11,
};
constexpr uint32_t kPortCtrl = 0xfffffffeu;

struct CtrlPacket {
    uint32_t cmd = 0;
    uint32_t service = 0, instance = 0, node = 0, port = 0;  // server / client fields
    static std::vector<uint8_t> encodeLookup(uint32_t service, uint32_t instance);
    static bool decode(const std::vector<uint8_t>& b, CtrlPacket* out);
    std::vector<uint8_t> encode() const;
};

// Transport abstraction so the client can be unit-tested with a fake modem.
class Transport {
  public:
    virtual ~Transport() = default;
    virtual bool open() = 0;
    virtual void close() = 0;
    virtual uint32_t localNode() const = 0;
    // Control port of the name service (local node, kPortCtrl).
    virtual Addr nameService() const { return Addr{localNode(), kPortCtrl}; }
    virtual bool send(const Addr& to, const std::vector<uint8_t>& data) = 0;
    // 1 = packet received, 0 = timeout, -1 = error
    virtual int recv(Addr* from, std::vector<uint8_t>* data, int timeoutMs) = 0;
};

std::unique_ptr<Transport> makeQrtrTransport();

struct Result {
    enum Status { Ok, QmiFailure, Timeout, NoService, TransportError, Stopped };
    Status status = Stopped;
    uint16_t qmiError = 0;
    Message msg;
    bool ok() const { return status == Ok; }
    std::string describe() const;
};

class Client {
  public:
    using IndicationFn = std::function<void(const Message&)>;
    using ServiceFn = std::function<void(uint32_t service, bool up)>;

    explicit Client(std::unique_ptr<Transport> t, const char* tag = "qmi");
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Opens the transport, starts the reader thread and looks up `services`.
    bool start(const std::vector<uint32_t>& services);
    void stop();

    // Blocks until all `services` have a server (or timeout). Returns the missing ones.
    std::vector<uint32_t> waitForServices(const std::vector<uint32_t>& services, int timeoutMs);
    bool hasService(uint32_t svc) const;
    std::map<uint32_t, std::vector<std::pair<Addr, uint32_t>>> servers() const;  // svc -> (addr, instance)

    // Synchronous request. Never call from an indication/service callback (deadlock-free by
    // design: callbacks run on a separate dispatch thread, but they must not block on it).
    Result request(uint32_t svc, Message req, int timeoutMs = 5000);

    void onIndication(uint32_t svc, uint16_t msgId, IndicationFn fn);
    void onServiceChange(ServiceFn fn);

    // Prefer servers on a node other than ours (the modem). Default: true.
    void setPreferRemote(bool v) { mPreferRemote = v; }

  private:
    struct Pending {
        bool done = false;
        Result res;
    };
    struct Event {
        enum Kind { Ind, Svc } kind;
        uint32_t svc = 0;
        bool up = false;
        Message msg;
    };
    void readerLoop();
    void dispatchLoop();
    void handleCtrl(const std::vector<uint8_t>& data);
    void handleData(const Addr& from, const std::vector<uint8_t>& data);
    bool addrFor(uint32_t svc, Addr* out) const;
    void post(Event e);

    std::unique_ptr<Transport> mT;
    std::string mTag;
    std::atomic<bool> mRunning{false};
    std::thread mReader, mDispatcher;
    bool mPreferRemote = true;

    mutable std::mutex mLock;
    std::condition_variable mCv;
    std::map<uint32_t, std::map<Addr, uint32_t>> mServers;  // svc -> addr -> instance
    std::map<Addr, uint32_t> mAddrToSvc;
    std::map<uint32_t, uint16_t> mTxn;
    std::map<std::pair<uint32_t, uint16_t>, std::shared_ptr<Pending>> mPending;
    std::map<std::pair<uint32_t, uint16_t>, std::vector<IndicationFn>> mInd;
    std::vector<ServiceFn> mSvcFns;

    std::mutex mEvLock;
    std::condition_variable mEvCv;
    std::vector<Event> mEvents;
};

}  // namespace a6l::qmi
