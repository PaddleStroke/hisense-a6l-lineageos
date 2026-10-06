// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): AP side of IMSDCM (QMI 770), the job of the stock
// vendor imsdatadaemon. The modem's IMS stack asks for the IMS (or emergency) PDN with
// PDP_ACTIVATE {apn, apn type, rat, ip family, WDS profile}; we answer at once (PDP id), bring the
// PDN up with WDS Start Network on that profile (own WDS client per PDP, kept open while the PDN
// is up), then send the PDP_ACTIVATE indication with the address (or a failure result). SIP and
// voice RTP run on the modem's IP stack (modem-centric IMS), so no AP routing / netdev is set up:
// the WDS call only has to exist. Optional mux binding (default mux 9, outside the HAL's 1..8)
// keeps the AP data port layout consistent when DPM/WDA are active.
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/ims.h>
#include <a6lqmi/server.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace a6l::radio {

struct ImsDcmConfig {
    uint8_t muxId = 9;          // 0 = no WDS Bind Mux Data Port
    uint32_t epType = 4, epIface = 1;
    bool addrAsText = true;     // address TLV as text (stock prints it with %s); false = raw bytes
    bool ackUnknown = true;     // other DCM requests (app state, Wi-Fi/HO, ...) -> success
    int profileOverride = -1;   // >0: use this WDS profile instead of the modem's
    uint32_t instance = 0;      // QRTR service instance (word = 1 | instance << 8)
    int startTimeoutMs = 60000;
    bool noWds = false;         // test: never start a call, answer the IND with failure
};

class ImsDcmService {
  public:
    using ClientFactory = std::function<std::unique_ptr<qmi::Client>(const char* tag)>;
    using ReportFn = std::function<void(const std::string& line)>;

    ImsDcmService(ImsDcmConfig cfg, std::unique_ptr<qmi::Transport> serverTransport,
                  ClientFactory wdsFactory);
    ~ImsDcmService();

    void onReport(ReportFn fn) { mReport = std::move(fn); }  // "A6L_IMSDCM_..." lines
    // volte6: modem lifecycle requests (0x2e REGISTER_APP_STATE, 0x33 SUB_DESTROY_INSTANCE, 0x34
    // SERVICE_ENABLE_STATUS) are answered like stock (plain success) and then passed here, so the daemon
    // can re-assert the IMS settings for the subscription (what Android's ImsService/qcril do on stock).
    // Called on the server thread: keep it short (set flags).
    using EventFn = std::function<void(uint16_t msgId, std::optional<uint32_t> instance)>;
    void onModemEvent(EventFn fn) { mEvent = std::move(fn); }
    bool start();
    void stop();

    enum class State { Activating, Up, Failed, Down };
    struct Pdp {
        uint8_t id = 0;
        qmi::Addr client;
        qmi::imsdcm::PdpActivateReq req;
        State state = State::Activating;
        std::string address;
        uint32_t handle = 0;
        std::string detail;
    };
    std::vector<Pdp> pdps() const;
    static const char* stateName(State s);

  private:
    struct Entry {
        Pdp pdp;
        std::unique_ptr<qmi::Client> wds;
    };
    // r5 bug hunt round2 R4: one PDN's WDS session events, latched from before START (see activate())
    struct Watch {
        std::atomic<bool> ended{false};      // DISCONNECTED or WDS gone
        std::atomic<bool> published{false};  // activation finished: a loss now goes through release()
    };
    std::optional<qmi::Message> handle(const qmi::Addr& from, const qmi::Message& m);
    void post(std::function<void()> fn);
    void workerLoop();
    void activate(uint8_t id);
    void release(uint8_t id, bool notifyLost, const char* why);
    void report(const std::string& s);
    uint8_t allocId();
    std::vector<uint8_t> addrTlv(const Pdp& p) const;

    ImsDcmConfig mCfg;
    std::unique_ptr<qmi::Server> mServer;
    ClientFactory mFactory;
    ReportFn mReport;
    EventFn mEvent;

    mutable std::mutex mLock;
    std::map<uint8_t, Entry> mPdps;
    uint8_t mNextId = 1;

    std::mutex mWorkLock;
    std::condition_variable mWorkCv;
    std::deque<std::function<void()>> mWork;
    bool mRunning = false;
    std::thread mWorker;
};

}  // namespace a6l::radio
