// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC client over a Transport, plus GnssEngine (session state machine shared
// by the AIDL HAL and a6l_gnss_test).
//
// Threads: LocClient owns a reader thread (packets -> responses / indications / name-service events). GnssEngine
// owns a worker thread that performs every blocking request, so indications are never blocked by a request.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "loc_v02.h"
#include "qmi.h"
#include "transport.h"

namespace a6l {

class LocClient {
  public:
    using IndicationFn = std::function<void(const qmi::Message&)>;
    using ServiceFn = std::function<void(bool up, const QrtrAddr& addr, uint32_t instance)>;
    using TapFn = std::function<void(char dir, const QrtrAddr& peer, const std::vector<uint8_t>& pkt)>;

    LocClient(std::unique_ptr<Transport> t, LogFn log, uint32_t service = loc::kServiceId);
    ~LocClient();

    void setIndicationHandler(IndicationFn f) { onInd_ = std::move(f); }
    void setServiceHandler(ServiceFn f) { onService_ = std::move(f); }
    void setTap(TapFn f) { tap_ = std::move(f); }
    // Every NEW_SERVER / DEL_SERVER the name service reports (service 0 lookup = all services, for --list).
    void setAnyServerHandler(std::function<void(bool up, const QrtrCtrl&)> f) { onAny_ = std::move(f); }

    bool open(std::string* err);   // open transport, start the reader, send NEW_LOOKUP(service)
    void close();
    bool isOpen() const { return running_; }
    bool transportFailed() const { return failed_; }
    bool serviceUp() const { return up_; }
    bool waitService(int timeoutMs);
    bool lookupDone() const { return lookupDone_; }

    // 0 = QMI success, >0 = QMI error code (response result TLV), -1 = transport error/timeout, -2 = no service.
    int request(qmi::Message req, qmi::Message* resp = nullptr, int timeoutMs = 5000);

  private:
    void readerLoop();
    void handlePacket(const QrtrAddr& from, const std::vector<uint8_t>& pkt);

    std::unique_ptr<Transport> t_;
    LogFn log_;
    uint32_t service_;
    IndicationFn onInd_;
    ServiceFn onService_;
    TapFn tap_;
    std::function<void(bool, const QrtrCtrl&)> onAny_;

    std::thread reader_;
    std::atomic<bool> running_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> up_{false};
    std::atomic<bool> lookupDone_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    QrtrAddr server_;
    uint16_t nextTxn_ = 1;
    std::map<uint16_t, std::shared_ptr<qmi::Message>> done_;   // txn -> response
    std::map<uint16_t, bool> waiting_;
};

struct EngineConfig {
    uint32_t intervalMs = 1000;
    uint32_t operationMode = loc::kModeStandalone;   // no SUPL/AGPS server path on this ROM
    uint32_t nmeaMask = loc::kDefaultNmeaMask;
    bool configureNmea = true;
    bool intermediate = true;         // IN_PROGRESS reports are logged, not reported as fixes
    bool synthesizeNmea = true;       // GGA+RMC from position reports when the modem sends no NMEA
    bool unlockEngine = false;        // QMI_LOC_SET_ENGINE_LOCK(NONE) before starting (writes modem NV)
    bool coldStart = false;           // delete all assistance data once before the first start
    int reconnectMs = 5000;           // retry period while AF_QIPCRTR / the LOC service is missing
    uint32_t clientRevision = 2;
    // r5 review F21 (28 Sep 2026): capped retries while a session is wanted but configuration/START failed with the
    // LOC service still present (backoff retryMinMs, doubling up to retryMaxMs, at most maxStartRetries in a row;
    // reset by setActive/service up).
    int retryMinMs = 1000;
    int retryMaxMs = 30000;
    int maxStartRetries = 10;
    int xtraPartTimeoutMs = 5000;   // per-part confirmation deadline (r5 F33)
};

class EngineListener {
  public:
    virtual ~EngineListener() = default;
    virtual void onFix(const loc::Fix&) {}
    virtual void onIntermediate(const loc::Fix&) {}
    virtual void onSvs(const std::vector<loc::Sv>&, const std::vector<uint16_t>& /*usedIds*/) {}
    virtual void onNmea(const std::string&, bool /*synthetic*/) {}
    virtual void onEngineState(bool /*on*/) {}
    virtual void onSessionState(bool /*started*/) {}
    virtual void onServiceState(bool /*up*/) {}
    virtual void onTimeRequest() {}
    virtual void onPositionRequest() {}
    virtual void onOrbitsRequest() {}                                      // modem asks for XTRA data
    virtual void onXtraResult(bool /*ok*/, const std::string& /*detail*/) {}   // end of injectXtra()
    virtual void onXtraInfo(const std::string& /*detail*/) {}               // queryXtra() results
    virtual void onPacket(char /*dir*/, const QrtrAddr&, const std::vector<uint8_t>&) {}
};

class GnssEngine {
  public:
    using TransportFactory = std::function<std::unique_ptr<Transport>()>;
    GnssEngine(TransportFactory f, EngineListener* l, EngineConfig c, LogFn log);
    ~GnssEngine();

    void begin();
    void end();
    void setActive(bool on);
    void setInterval(uint32_t ms);
    // r5 round6 F53: Android IGnss.setPositionMode: interval + recurrence. A single-fix session is started with QMI
    // recurrence SINGLE and completed (STOP, no restart) after its first final fix; setActive(true) re-arms it.
    void setPositionMode(uint32_t intervalMs, bool single);
    // r5 review F64 (28 Sep 2026): the UTC sample keeps its CLOCK_BOOTTIME reference (Android elapsedRealtime at
    // sampling) through the worker queue and is advanced by the elapsed boot time immediately before the QMI request is
    // built, so a request waiting behind blocking assistance work is not stale. A reference in the future, a negative
    // UTC or an overflowing sum is discarded (logged). The two-argument form means "sampled now".
    void injectTime(uint64_t utcMs, uint32_t uncMs);
    void injectTime(int64_t utcMs, uint32_t uncMs, int64_t refBootMs);
    static int64_t bootMs();   // CLOCK_BOOTTIME (suspend-inclusive, = Android elapsedRealtime) in ms
    // pure: UTC at bootNowMs for a sample (utcMs at refBootMs); false = unusable sample
    static bool advanceUtc(int64_t utcMs, int64_t refBootMs, int64_t bootNowMs, uint64_t* out);
    void injectLocation(double lat, double lon, float accM);
    // r5 round6 F54: the deletion stays pending until the modem acknowledged it; a START never overtakes it (service
    // absence / timeout: retried before the next START; a QMI rejection is final and logged).
    void deleteAll();
    bool deletePending() const { return deletePending_; }
    // XTRA predicted orbits: splits the file and injects it part by part, waiting for each part's indication.
    // Result through EngineListener::onXtraResult; then queries validity (onXtraInfo).
    void injectXtra(std::vector<uint8_t> file, size_t partSize = loc::kMaxOrbitsPart);
    // GET_PREDICTED_ORBITS_DATA_SOURCE + _VALIDITY -> onXtraInfo (server URLs the modem wants, max sizes, validity)
    void queryXtra();
    // Coarse position (cell/network based or user supplied): INJECT_POSITION with reliability LOW + source.
    void injectCoarseLocation(double lat, double lon, float accM, uint32_t source);

    bool serviceUp() const;
    bool sessionRunning() const { return session_; }
    std::string status() const;
    int lastRequestError() const { return lastErr_; }
    uint64_t fixCount() const { return fixes_; }

  private:
    void workerLoop();
    void post(std::function<void()> fn);
    void onIndication(const qmi::Message& m);
    bool connect();
    bool configure();        // true when the essential configuration was acknowledged
    void startSession();
    void stopSession();      // r5 round6 F52: session_ stays set when the STOP was not applied (bounded retry)
    bool applyPendingDelete();   // F54: worker only; false = still pending (no service / timeout)
    void completeSingle();       // F53: first final fix of a single-fix session
    void onSessionFinished();    // F52 related: modem-reported end of our session
    void reconcile();        // bring the modem session to desired_
    void scheduleRetry(const char* what);
    void serviceControl();   // r5 F34: run a pending stop from inside long assistance waits (worker thread only)
    void pauseMs(int ms);    // interruptible sleep (services stop requests)
    int call(const qmi::Message& m, const char* what);
    bool waitInd(uint16_t id, int timeoutMs, qmi::Message* out);
    void clearInd(uint16_t id);
    bool doInjectXtra(const std::vector<uint8_t>& f, size_t partSize, bool withFormat, std::string* detail,
                      int* firstPartErr);
    bool querySource(loc::OrbitsSource* o);
    // r5 bug hunt round2 G2: XTRA received while the LOC service is absent (or lost mid-transfer) is kept (latest
    // file only, worker only) and injected once the service is up.
    void runXtra(const std::vector<uint8_t>& f, size_t partSize);
    void deferXtra(std::vector<uint8_t> f, size_t partSize, const char* why);
    void runPendingXtra();
    void doQueryXtra();
    static int64_t nowMs();

    TransportFactory factory_;
    EngineListener* l_;
    EngineConfig cfg_;
    LogFn log_;
    std::unique_ptr<LocClient> client_;
    mutable std::mutex clientMu_;   // guards client_ replacement vs serviceUp() from other threads

    std::thread worker_;
    std::atomic<bool> running_{false};
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;

    std::atomic<bool> desired_{false};
    std::atomic<bool> session_{false};
    std::atomic<bool> configured_{false};
    std::atomic<bool> pendingServiceUp_{false};
    std::atomic<bool> pendingServiceDown_{false};
    std::atomic<bool> ctrlPending_{false};   // setActive(false) waiting for the worker (guarded wakeup: indMu_)
    int64_t retryAt_ = 0;                    // worker only: next START/configure retry (0 = none)
    int retryN_ = 0;
    std::atomic<int> lastErr_{0};
    std::atomic<uint64_t> fixes_{0};
    std::atomic<int64_t> lastModemNmeaMs_{0};
    std::atomic<uint32_t> interval_{1000};
    std::atomic<bool> single_{false};        // F53: requested recurrence
    std::atomic<bool> deletePending_{false}; // F54
    bool sessionSingle_ = false;             // worker only: recurrence of the running session
    bool singleDone_ = false;                // worker only: the single fix was delivered; no restart until setActive(true)
    std::vector<uint8_t> pendingXtra_;      // G2: worker only
    size_t pendingXtraPart_ = 0;
    bool coldDone_ = false;
    bool unlockTried_ = false;
    std::mutex indMu_;
    std::condition_variable indCv_;
    std::map<uint16_t, std::deque<qmi::Message>> inds_;   // request/indication pairs the worker waits for
    std::mutex usedMu_;
    std::vector<uint16_t> lastUsed_;
    int64_t lastUsedMs_ = 0;
};

}  // namespace a6l
