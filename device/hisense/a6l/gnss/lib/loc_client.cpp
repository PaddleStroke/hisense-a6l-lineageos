// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): LocClient + GnssEngine.
#include "loc_client.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <ctime>
#include <cstdio>
#include <thread>

#include "nmea.h"

namespace a6l {

namespace {
std::string hex16(uint32_t v) {
    char b[16];
    snprintf(b, sizeof(b), "0x%04x", v);
    return b;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------- LocClient
LocClient::LocClient(std::unique_ptr<Transport> t, LogFn log, uint32_t service)
    : t_(std::move(t)), log_(std::move(log)), service_(service) {}

LocClient::~LocClient() { close(); }

bool LocClient::open(std::string* err) {
    close();
    failed_ = false;
    up_ = false;
    lookupDone_ = false;
    if (!t_->open(err)) {
        failed_ = true;
        return false;
    }
    running_ = true;
    reader_ = std::thread([this] { readerLoop(); });
    if (!t_->sendCtrl(makeLookup(service_))) {
        if (err) *err = "NEW_LOOKUP send failed";
        close();
        failed_ = true;
        return false;
    }
    return true;
}

void LocClient::close() {
    running_ = false;
    if (reader_.joinable()) reader_.join();
    t_->close();
    up_ = false;
    cv_.notify_all();
}

bool LocClient::waitService(int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    return cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [this] { return up_.load() || failed_.load(); }) &&
           up_;
}

void LocClient::readerLoop() {
    std::vector<uint8_t> pkt;
    QrtrAddr from;
    while (running_) {
        int r = t_->recv(&from, &pkt, 200);
        if (r == 0) continue;
        if (r < 0) {
            if (log_) log_(kLogWarn, "qrtr receive failed; transport will be reopened");
            bool was = up_.exchange(false);
            failed_ = true;
            cv_.notify_all();
            if (was && onService_) onService_(false, server_, 0);
            break;
        }
        handlePacket(from, pkt);
    }
}

void LocClient::handlePacket(const QrtrAddr& from, const std::vector<uint8_t>& pkt) {
    if (from.port == kQrtrPortCtrl) {
        QrtrCtrl c;
        if (!parseCtrl(pkt, &c)) return;
        if (c.cmd == kQrtrNewServer && c.service == 0 && c.node == 0 && c.port == 0) {
            lookupDone_ = true;   // end of the initial lookup listing
            cv_.notify_all();
            return;
        }
        if ((c.cmd == kQrtrNewServer || c.cmd == kQrtrDelServer) && onAny_) onAny_(c.cmd == kQrtrNewServer, c);
        if (c.cmd == kQrtrNewServer && c.service == service_) {
            {
                std::lock_guard<std::mutex> lk(mu_);
                server_ = QrtrAddr{c.node, c.port};
            }
            if (log_)
                log_(kLogInfo, "LOC service up: node " + std::to_string(c.node) + " port " + std::to_string(c.port) +
                                       " instance " + hex16(c.instance));
            up_ = true;
            cv_.notify_all();
            if (onService_) onService_(true, server_, c.instance);
        } else if (c.cmd == kQrtrDelServer && c.service == service_ && c.node == server_.node &&
                   c.port == server_.port) {
            if (log_) log_(kLogWarn, "LOC service gone (modem restart?)");
            up_ = false;
            cv_.notify_all();
            if (onService_) onService_(false, server_, c.instance);
        } else if (c.cmd == kQrtrBye && c.node == server_.node && up_) {
            if (log_) log_(kLogWarn, "modem node said BYE");
            up_ = false;
            cv_.notify_all();
            if (onService_) onService_(false, server_, 0);
        }
        return;
    }
    if (tap_) tap_('<', from, pkt);
    qmi::Message m;
    std::string err;
    if (!qmi::decode(pkt.data(), pkt.size(), &m, &err)) {
        if (log_) log_(kLogWarn, "bad QMI packet: " + err);
        return;
    }
    if (m.type == qmi::kResponse) {
        std::lock_guard<std::mutex> lk(mu_);
        if (waiting_.count(m.txn)) {
            done_[m.txn] = std::make_shared<qmi::Message>(m);
            cv_.notify_all();
        } else if (log_) {
            log_(kLogDebug, "late/unknown response txn " + std::to_string(m.txn));
        }
        return;
    }
    if (m.type == qmi::kIndication && onInd_) onInd_(m);
}

int LocClient::request(qmi::Message req, qmi::Message* resp, int timeoutMs) {
    if (!up_) return -2;
    QrtrAddr to;
    uint16_t txn;
    {
        std::lock_guard<std::mutex> lk(mu_);
        txn = nextTxn_++;
        if (nextTxn_ == 0) nextTxn_ = 1;
        waiting_[txn] = true;
        to = server_;
    }
    req.type = qmi::kRequest;
    req.txn = txn;
    auto pkt = qmi::encode(req);
    if (tap_) tap_('>', to, pkt);
    if (!t_->send(to, pkt)) {
        std::lock_guard<std::mutex> lk(mu_);
        waiting_.erase(txn);
        return -1;
    }
    std::unique_lock<std::mutex> lk(mu_);
    bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                            [&] { return done_.count(txn) || !up_ || !running_; });
    waiting_.erase(txn);
    if (!got || !done_.count(txn)) {
        done_.erase(txn);
        return up_ ? -1 : -2;
    }
    auto r = done_[txn];
    done_.erase(txn);
    lk.unlock();
    if (resp) *resp = *r;
    uint16_t result = 0, error = 0;
    if (!r->getResult(&result, &error)) return 0;
    if (result == 0) return 0;
    return error ? error : 0xffff;
}

// --------------------------------------------------------------------------------------------------- GnssEngine
GnssEngine::GnssEngine(TransportFactory f, EngineListener* l, EngineConfig c, LogFn log)
    : factory_(std::move(f)), l_(l), cfg_(c), log_(std::move(log)) {
    interval_ = cfg_.intervalMs;
}

GnssEngine::~GnssEngine() { end(); }

int64_t GnssEngine::nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

void GnssEngine::begin() {
    if (running_) return;
    running_ = true;
    worker_ = std::thread([this] { workerLoop(); });
}

void GnssEngine::end() {
    if (!running_) return;
    running_ = false;
    cv_.notify_all();
    {
        std::lock_guard<std::mutex> lk(indMu_);
    }
    indCv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void GnssEngine::post(std::function<void()> fn) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        q_.push_back(std::move(fn));
    }
    cv_.notify_all();
}

bool GnssEngine::serviceUp() const {
    std::lock_guard<std::mutex> lk(clientMu_);   // client_ is replaced by the worker; other threads only read it here
    return client_ && client_->serviceUp();
}

std::string GnssEngine::status() const {
    return std::string("service=") + (serviceUp() ? "up" : "down") + " session=" + (session_ ? "on" : "off") +
           " desired=" + (desired_ ? "on" : "off") + " fixes=" + std::to_string(fixes_) +
           " lastErr=" + std::to_string(lastErr_);
}

int GnssEngine::call(const qmi::Message& m, const char* what) {
    if (!client_) return -2;
    int rc = client_->request(m);
    if (rc != 0) lastErr_ = rc;
    if (log_) log_(rc == 0 ? kLogDebug : kLogWarn, std::string(what) + " (" + hex16(m.msgId) + ") -> " +
                                                           (rc == 0 ? "ok" : "error " + std::to_string(rc)));
    return rc;
}

bool GnssEngine::connect() {
    static int failures = 0;
    {
        std::lock_guard<std::mutex> lk(clientMu_);
        client_.reset();
    }
    auto c = std::make_unique<LocClient>(factory_(), log_);
    c->setIndicationHandler([this](const qmi::Message& m) { onIndication(m); });
    c->setServiceHandler([this](bool up, const QrtrAddr&, uint32_t) {
        if (up)
            pendingServiceUp_ = true;
        else
            pendingServiceDown_ = true;
        if (l_) l_->onServiceState(up);
        cv_.notify_all();
    });
    c->setTap([this](char d, const QrtrAddr& a, const std::vector<uint8_t>& p) {
        if (l_) l_->onPacket(d, a, p);
    });
    std::string err;
    if (!c->open(&err)) {
        if (log_ && (failures++ % 12) == 0) log_(kLogWarn, "modem transport unavailable: " + err + " (retrying)");
        return false;
    }
    failures = 0;
    {
        std::lock_guard<std::mutex> lk(clientMu_);
        client_ = std::move(c);
    }
    return true;
}

// r5 review F21 (28 Sep 2026): configuration is only marked applied when the essential commands were acknowledged.
// REG_EVENTS is essential (without it no position/SV indication is delivered). SET_OPERATION_MODE is essential unless
// the modem answers NOT_SUPPORTED (it then keeps its default mode). INFORM_CLIENT_REVISION and SET_NMEA_TYPES are
// optional (NMEA is synthesized from position reports when the modem sends none).
bool GnssEngine::configure() {
    configured_ = false;
    call(loc::makeInformClientRevision(cfg_.clientRevision), "INFORM_CLIENT_REVISION");
    uint64_t mask = loc::kEvPositionReport | loc::kEvGnssSvInfo | loc::kEvNmea | loc::kEvEngineState |
                    loc::kEvFixSessionState | loc::kEvInjectTimeReq | loc::kEvInjectPositionReq |
                    loc::kEvInjectOrbitsReq;
    if (call(loc::makeRegEvents(mask), "REG_EVENTS") != 0) return false;
    int rc = call(loc::makeSetOperationMode(cfg_.operationMode), "SET_OPERATION_MODE");
    if (rc != 0 && rc != 0x5E /* QMI_ERR_NOT_SUPPORTED */) return false;
    if (cfg_.configureNmea) call(loc::makeSetNmeaTypes(cfg_.nmeaMask), "SET_NMEA_TYPES");
    configured_ = true;
    return true;
}

void GnssEngine::scheduleRetry(const char* what) {
    if (retryN_ >= cfg_.maxStartRetries) {
        retryAt_ = 0;
        if (log_) log_(kLogWarn, std::string(what) + " failed; retries exhausted until the next start/service change");
        return;
    }
    int64_t d = cfg_.retryMinMs;
    for (int i = 0; i < retryN_ && d < cfg_.retryMaxMs; i++) d *= 2;
    if (d > cfg_.retryMaxMs) d = cfg_.retryMaxMs;
    retryN_++;
    retryAt_ = nowMs() + d;
    if (log_) log_(kLogWarn, std::string(what) + " failed; retry " + std::to_string(retryN_) + " in " +
                                     std::to_string(d) + " ms");
}

void GnssEngine::reconcile() {
    if (desired_ && !session_ && !singleDone_)
        startSession();
    else if (!desired_ && session_)
        stopSession();
}

void GnssEngine::serviceControl() {
    if (!ctrlPending_.exchange(false)) return;
    if (!desired_ && session_) {
        if (log_) log_(kLogInfo, "stop requested during assistance transfer: stopping the session now");
        stopSession();
    }
}

void GnssEngine::pauseMs(int ms) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    std::unique_lock<std::mutex> lk(indMu_);
    while (running_) {
        bool woke = indCv_.wait_until(lk, end, [&] { return !running_ || ctrlPending_.load(); });
        if (!woke) return;
        if (!running_) return;
        lk.unlock();
        serviceControl();
        lk.lock();
    }
}

void GnssEngine::startSession() {
    if (!client_ || !client_->serviceUp()) return;
    if (!configured_ && !configure()) {
        session_ = false;
        scheduleRetry("configuration");
        return;
    }
    if (cfg_.coldStart && !coldDone_) {
        call(loc::makeDeleteAllAssistData(), "DELETE_ASSIST_DATA(all)");
        coldDone_ = true;
    }
    // r5 round6 F54: a requested deletion is applied before this START, never overtaken by it
    if (deletePending_ && !applyPendingDelete()) {
        scheduleRetry("DELETE_ASSIST_DATA before START");
        return;
    }
    if (cfg_.unlockEngine && !unlockTried_) {
        call(loc::makeSetEngineLock(loc::kLockNone), "SET_ENGINE_LOCK(none)");
        unlockTried_ = true;
    }
    const bool single = single_;
    int rc = call(loc::makeStart(1, interval_, cfg_.intermediate, 3,
                                 single ? loc::kRecurrenceSingle : loc::kRecurrencePeriodic),
                  single ? "START(single)" : "START");
    if (rc == 0) {
        session_ = true;
        sessionSingle_ = single;
        singleDone_ = false;
        retryN_ = 0;
        retryAt_ = 0;
    } else if (session_) {
        // a parameter update (new START on the running session) was refused: the old session keeps running
        scheduleRetry("START (update)");
    } else {
        scheduleRetry("START");
    }
}

// r5 review round6 F52 (29 Sep 2026): the session is only recorded as stopped when the modem accepted the STOP
// (or answered NO_EFFECT = nothing to stop), or when the LOC service is gone (the session went with it; service up
// reconfigures from scratch). A rejected / timed-out STOP keeps session_ (applied state = running or unknown) and is
// retried with the capped backoff; worker shutdown still sends its cleanup STOP. Layout errors (MALFORMED/MISSING/
// INVALID_ARG...) are permanent: not retried blindly (logged; end() still tries once).
void GnssEngine::stopSession() {
    if (!client_ || !client_->serviceUp()) {
        session_ = false;
        return;
    }
    int rc = call(loc::makeStop(1), "STOP");
    if (rc == 0 || rc == 0x1A /* QMI_ERR_NO_EFFECT: no session to stop */) {
        session_ = false;
        sessionSingle_ = false;
        if (!desired_) {
            retryN_ = 0;
            retryAt_ = 0;
        }
        return;
    }
    if (rc > 0 && loc::qmiErrorIsLayout(rc)) {
        retryAt_ = 0;
        if (log_) log_(kLogWarn, "STOP refused as malformed (error " + std::to_string(rc) + "): not retried");
        return;
    }
    scheduleRetry("STOP");
}

// r5 round6 F54: worker thread. true = nothing pending any more (acknowledged, or rejected by the modem = final).
bool GnssEngine::applyPendingDelete() {
    if (!deletePending_) return true;
    if (!client_ || !client_->serviceUp()) return false;
    int rc = call(loc::makeDeleteAllAssistData(), "DELETE_ASSIST_DATA(all)");
    if (rc < 0) return false;   // timeout / transport: still pending
    deletePending_ = false;
    if (rc != 0 && log_) log_(kLogWarn, "DELETE_ASSIST_DATA rejected by the modem (error " + std::to_string(rc) + ")");
    return true;
}

// r5 round6 F53: a single-fix session is complete after its first final fix: stop it on the modem (native SINGLE
// recurrence normally ends it by itself, so any STOP answer counts as done) and do not restart it until the next
// setActive(true) - service returns and retries must not turn it into an endless periodic request.
void GnssEngine::completeSingle() {
    if (!session_ || !sessionSingle_) return;
    if (client_ && client_->serviceUp()) call(loc::makeStop(1), "STOP(single fix complete)");
    session_ = false;
    sessionSingle_ = false;
    singleDone_ = true;
    retryAt_ = 0;
    if (log_) log_(kLogInfo, "single fix delivered: session complete");
}

// The modem reports that our session finished (e.g. engine reset): a single session is complete; a periodic one that
// is still wanted is restarted through the capped retry (no tight START loop); unwanted = nothing more to stop.
void GnssEngine::onSessionFinished() {
    if (!session_) return;
    session_ = false;
    if (sessionSingle_) {
        sessionSingle_ = false;
        singleDone_ = true;
        return;
    }
    if (desired_) scheduleRetry("session ended by the modem");
    else retryAt_ = 0;
}

// r5 review F34: a stop also raises ctrlPending_ so an assistance transfer occupying the worker stops the session from
// inside its waits (same thread: no concurrent client access). Queued tasks reconcile to the latest desired state, so a
// stale task never restarts a canceled session.
void GnssEngine::setActive(bool on) {
    desired_ = on;
    if (!on) {
        {
            std::lock_guard<std::mutex> lk(indMu_);
            ctrlPending_ = true;
        }
        indCv_.notify_all();
    }
    post([this, on] {
        if (on) singleDone_ = false;   // F53: every start request re-arms a single fix
        if (desired_ && !session_ && retryAt_ > 0) return;   // a failed start is already being retried
        retryN_ = 0;
        retryAt_ = 0;
        reconcile();
    });
}

void GnssEngine::setInterval(uint32_t ms) { setPositionMode(ms, single_); }

void GnssEngine::setPositionMode(uint32_t ms, bool single) {
    interval_ = ms < 1000 ? 1000 : ms;
    single_ = single;
    post([this] {
        if (session_) startSession();   // a new START with the same session id updates the parameters
    });
}

int64_t GnssEngine::bootMs() {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool GnssEngine::advanceUtc(int64_t utcMs, int64_t refBootMs, int64_t bootNowMs, uint64_t* out) {
    if (utcMs < 0 || refBootMs < 0 || bootNowMs < refBootMs) return false;   // invalid / future reference
    int64_t el = bootNowMs - refBootMs;
    if (utcMs > INT64_MAX - el) return false;
    *out = uint64_t(utcMs + el);
    return true;
}

void GnssEngine::injectTime(uint64_t utcMs, uint32_t uncMs) {
    injectTime(utcMs > uint64_t(INT64_MAX) ? int64_t(-1) : int64_t(utcMs), uncMs, bootMs());
}

void GnssEngine::injectTime(int64_t utcMs, uint32_t uncMs, int64_t refBootMs) {
    post([this, utcMs, uncMs, refBootMs] {   // F64: advanced here, on the worker, right before the request
        if (!serviceUp()) return;
        int64_t now = bootMs();
        uint64_t utc = 0;
        if (!advanceUtc(utcMs, refBootMs, now, &utc)) {
            if (log_)
                log_(kLogWarn, "INJECT_UTC_TIME dropped: unusable sample utc=" + std::to_string(utcMs) +
                                       " ref=" + std::to_string(refBootMs) + " boot_now=" + std::to_string(now));
            return;
        }
        call(loc::makeInjectUtcTime(utc, uncMs), "INJECT_UTC_TIME");
    });
}

void GnssEngine::injectLocation(double lat, double lon, float accM) {
    post([this, lat, lon, accM] {
        if (serviceUp()) call(loc::makeInjectPosition(lat, lon, accM), "INJECT_POSITION");
    });
}

void GnssEngine::deleteAll() {
    deletePending_ = true;   // F54: kept across service absence; applied before the next START at the latest
    post([this] { applyPendingDelete(); });
}

void GnssEngine::injectCoarseLocation(double lat, double lon, float accM, uint32_t source) {
    post([this, lat, lon, accM, source] {
        if (serviceUp()) call(loc::makeInjectCoarsePosition(lat, lon, accM, source), "INJECT_POSITION(coarse)");
    });
}

void GnssEngine::clearInd(uint16_t id) {
    std::lock_guard<std::mutex> lk(indMu_);
    inds_[id].clear();
}

bool GnssEngine::waitInd(uint16_t id, int timeoutMs, qmi::Message* out) {
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(indMu_);
    for (;;) {
        bool ok = indCv_.wait_until(lk, end,
                                    [&] { return !inds_[id].empty() || !running_ || ctrlPending_.load(); });
        if (running_ && ctrlPending_) {   // r5 F34: a stop does not wait behind the transfer
            lk.unlock();
            serviceControl();
            lk.lock();
            continue;
        }
        if (!ok || inds_[id].empty()) return false;
        *out = inds_[id].front();
        inds_[id].pop_front();
        return true;
    }
}

bool GnssEngine::doInjectXtra(const std::vector<uint8_t>& f, size_t partSize, bool withFormat, std::string* detail,
                              int* firstPartErr) {
    // Same layout as stock LocApiV02::setXtraData (vendor/lib64/libloc_api_v02.so, disassembled 25 Sep 2026):
    // QMI_LOC_INJECT_PREDICTED_ORBITS_DATA (0x35), parts of 1024, formatType_valid=1/formatType=0 (XTRA), and
    // after each part a wait for indication 0x35 whose partNum must match.
    *firstPartErr = 0;
    auto parts = loc::buildXtraParts(f, partSize, withFormat);
    // r5 review F33 (28 Sep 2026): each part needs a well-formed indication (mandatory status TLV) whose partNum is
    // this part, within 5 s. Malformed indications and other part numbers (stale/duplicate/out of order) are ignored
    // until the deadline; an error status for this part (or without a part number) fails the transfer; no valid
    // confirmation fails it too (stock setXtraData: sync-request timeout / partNum mismatch = failure). Success means
    // "every part acknowledged by the modem"; usable data is checked separately by the validity query afterwards.
    clearInd(loc::kInjectPredictedOrbits);
    int64_t t0 = nowMs();
    int ignored = 0;
    for (size_t i = 0; i < parts.size(); i++) {
        serviceControl();
        if (!running_) {
            *detail = "engine stopping at part " + std::to_string(i + 1);
            return false;
        }
        if (!client_ || !client_->serviceUp()) {
            *detail = "LOC service lost at part " + std::to_string(i + 1);
            return false;
        }
        int rc = client_->request(parts[i], nullptr, 5000);
        if (rc != 0) {
            if (i == 0) *firstPartErr = rc;
            *detail = "part " + std::to_string(i + 1) + "/" + std::to_string(parts.size()) + " request error " +
                      std::to_string(rc) + " (QMI_ERR_" + loc::qmiErrorName(rc) + ")" +
                      " format_tlv=" + (withFormat ? "yes" : "no") + " part_size=" + std::to_string(partSize);
            return false;
        }
        const uint16_t want = uint16_t(i + 1);
        int64_t deadline = nowMs() + cfg_.xtraPartTimeoutMs;
        bool confirmed = false;
        while (!confirmed) {
            int64_t left = deadline - nowMs();
            qmi::Message ind;
            if (left <= 0 || !waitInd(loc::kInjectPredictedOrbits, int(left), &ind)) break;
            uint32_t st = 0;
            uint16_t pn = 0;
            if (!loc::parseInjectOrbitsInd(ind, &st, &pn)) {
                ignored++;
                if (log_) log_(kLogWarn, "XTRA part " + std::to_string(want) + ": malformed indication ignored");
                continue;
            }
            if (st != 0 && (pn == want || pn == 0)) {
                *detail = "part " + std::to_string(want) + " indication status " + std::to_string(st) + " (" +
                          loc::sessionStatusName(st) + ")";
                return false;
            }
            if (pn != want) {
                ignored++;
                if (log_)
                    log_(kLogWarn, "XTRA part " + std::to_string(want) + ": ignoring indication for part " +
                                           std::to_string(pn));
                continue;
            }
            confirmed = true;
        }
        if (!confirmed) {
            *detail = "part " + std::to_string(want) + "/" + std::to_string(parts.size()) +
                      ": no valid confirmation within " + std::to_string(cfg_.xtraPartTimeoutMs) + " ms (ignored=" + std::to_string(ignored) + ")";
            return false;
        }
    }
    *detail = "all parts acknowledged: parts=" + std::to_string(parts.size()) + " bytes=" + std::to_string(f.size()) +
              " ignored_ind=" + std::to_string(ignored) + " format_tlv=" + (withFormat ? "yes" : "no") +
              " part_size=" + std::to_string(partSize) + " ms=" + std::to_string(nowMs() - t0);
    return true;
}

bool GnssEngine::querySource(loc::OrbitsSource* o) {
    clearInd(loc::kGetPredictedOrbitsSource);
    int rc = call(loc::makeGetPredictedOrbitsSource(), "GET_PREDICTED_ORBITS_DATA_SOURCE");
    qmi::Message ind;
    if (rc != 0 || !waitInd(loc::kGetPredictedOrbitsSource, 3000, &ind)) return false;
    loc::parseOrbitsSourceInd(ind, o);
    return true;
}

void GnssEngine::injectXtra(std::vector<uint8_t> file, size_t partSize) {
    post([this, f = std::move(file), partSize]() mutable {
        std::string why;
        if (!loc::looksLikeXtra(f, &why)) {
            if (l_) l_->onXtraResult(false, why);
            return;
        }
        // r5 bug hunt round2 G2 (29 Sep 2026): the HAL has already told the framework the PSDS data was accepted and
        // the framework does not download again by itself; the modem stack starts late (a6l-radio.sh after boot) and
        // restarts. Data arriving while LOC is absent is kept (latest file only) and injected when the service is up.
        if (!serviceUp()) {
            deferXtra(std::move(f), partSize, "LOC service not up");
            return;
        }
        runXtra(f, partSize);
    });
}

void GnssEngine::deferXtra(std::vector<uint8_t> f, size_t partSize, const char* why) {
    if (log_)
        log_(kLogInfo, std::string("XTRA (") + std::to_string(f.size()) + " bytes) deferred: " + why +
                               "; injected when the LOC service is up");
    pendingXtra_ = std::move(f);
    pendingXtraPart_ = partSize;
    if (l_) l_->onXtraInfo(std::string("deferred: ") + why);
}

void GnssEngine::runPendingXtra() {
    if (pendingXtra_.empty() || !serviceUp()) return;
    std::vector<uint8_t> f = std::move(pendingXtra_);
    pendingXtra_.clear();
    runXtra(f, pendingXtraPart_);
}

void GnssEngine::runXtra(const std::vector<uint8_t>& f, size_t partSize) {
    {
        // misc2 (25 Sep 2026): the 25 Sep attended run failed with "request error 3" = QMI_ERR_INTERNAL (not
        // MALFORMED: the layout is identical to stock), then 94 = QMI_ERR_NOT_SUPPORTED on the no-formatType retry.
        // Now: ask the modem for its part size first (like stock's xtra client), keep formatType (stock always sends
        // it), retry INTERNAL/ABORTED/NOT_READY/IN_USE twice after 3 s, and drop formatType only on a LAYOUT error.
        size_t ps = partSize ? partSize : loc::kMaxOrbitsPart;
        loc::OrbitsSource src;
        if (querySource(&src)) {
            std::string d = "XTRA source status=" + std::to_string(src.status);
            if (src.hasSizes) {
                d += " max_file=" + std::to_string(src.maxFileSize) + " max_part=" + std::to_string(src.maxPartSize);
                if (src.maxPartSize > 0 && src.maxPartSize < ps) ps = src.maxPartSize;
                if (src.maxFileSize > 0 && f.size() > src.maxFileSize) d += " WARNING file larger than max_file";
            }
            for (auto& u : src.servers) d += " server=" + u;
            if (log_) log_(kLogInfo, d);
        }
        std::string detail;
        int err = 0;
        bool ok = false;
        bool withFormat = true;
        for (int attempt = 0; attempt < 3 && !ok; attempt++) {
            ok = doInjectXtra(f, ps, withFormat, &detail, &err);
            if (ok) break;
            if (err == 0x03 || err == 0x04 || err == 0x3A || err == 0x17) {
                if (attempt == 2) break;
                if (log_) log_(kLogWarn, "XTRA: " + detail + "; transient? retry in 3 s");
                pauseMs(3000);   // r5 F34: services a stop request while backing off
                if (!running_) break;
                continue;
            }
            if (withFormat && loc::qmiErrorIsLayout(err)) {
                if (log_) log_(kLogWarn, "XTRA: layout rejected (" + detail + "); retrying without formatType TLV");
                withFormat = false;
                continue;
            }
            break;
        }
        if (!ok && running_ && !serviceUp()) {   // G2: LOC went away mid-transfer: again when it is back
            deferXtra(std::vector<uint8_t>(f), partSize, "LOC service lost during the transfer");
            return;
        }
        if (log_) log_(ok ? kLogInfo : kLogWarn, std::string("XTRA injection ") + (ok ? "done: " : "FAILED: ") + detail);
        if (l_) l_->onXtraResult(ok, detail);
        doQueryXtra();   // validity after success, and as a diagnostic after a failure
    }
}

void GnssEngine::queryXtra() {
    post([this] { doQueryXtra(); });
}

void GnssEngine::doQueryXtra() {
    if (!serviceUp()) return;
    clearInd(loc::kGetPredictedOrbitsSource);
    int rc = call(loc::makeGetPredictedOrbitsSource(), "GET_PREDICTED_ORBITS_DATA_SOURCE");
    qmi::Message ind;
    if (rc == 0 && waitInd(loc::kGetPredictedOrbitsSource, 3000, &ind)) {
        loc::OrbitsSource o;
        loc::parseOrbitsSourceInd(ind, &o);
        std::string d = "source status=" + std::to_string(o.status);
        if (o.hasSizes)
            d += " max_file=" + std::to_string(o.maxFileSize) + " max_part=" + std::to_string(o.maxPartSize);
        for (auto& u : o.servers) d += " server=" + u;
        if (l_) l_->onXtraInfo(d);
    } else if (l_) {
        l_->onXtraInfo("source: no answer (rc " + std::to_string(rc) + ")");
    }
    clearInd(loc::kGetPredictedOrbitsValidity);
    rc = call(loc::makeGetPredictedOrbitsValidity(), "GET_PREDICTED_ORBITS_DATA_VALIDITY");
    if (rc == 0 && waitInd(loc::kGetPredictedOrbitsValidity, 3000, &ind)) {
        loc::OrbitsValidity v;
        loc::parseOrbitsValidityInd(ind, &v);
        std::string d = "validity status=" + std::to_string(v.status);
        if (v.valid)
            d += " start_gps_s=" + std::to_string(v.startGpsSec) + " duration_h=" + std::to_string(v.durationHours);
        else
            d += " (no valid XTRA data in the modem)";
        if (l_) l_->onXtraInfo(d);
    } else if (l_) {
        l_->onXtraInfo("validity: no answer (rc " + std::to_string(rc) + ")");
    }
}

void GnssEngine::workerLoop() {
    int64_t nextConnect = 0;
    while (running_) {
        if (!client_ || client_->transportFailed()) {
            session_ = false;
            configured_ = false;
            if (nowMs() >= nextConnect && !connect()) nextConnect = nowMs() + cfg_.reconnectMs;
        }
        std::function<void()> fn;
        {
            int64_t waitMs = client_ ? 500 : 250;
            if (retryAt_ > 0) waitMs = std::max<int64_t>(1, std::min<int64_t>(waitMs, retryAt_ - nowMs()));
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(waitMs), [this] {
                return !running_ || !q_.empty() || pendingServiceUp_ || pendingServiceDown_;
            });
            if (!q_.empty()) {
                fn = std::move(q_.front());
                q_.pop_front();
            }
        }
        if (!running_) break;
        // r5 bug hunt round2 G1 (29 Sep 2026): ctrlPending_ is NOT cleared here any more. A stop requested while a
        // long task (XTRA) was still queued sits behind it: clearing the flag when that task is dequeued made the
        // transfer ignore the stop (STOP only after all part deadlines / retries / validity queries). A stale flag is
        // harmless: serviceControl() only acts on !desired_ && session_ and clears it.
        if (pendingServiceDown_.exchange(false)) {
            session_ = false;
            configured_ = false;
            retryAt_ = 0;
        }
        if (pendingServiceUp_.exchange(false)) {
            session_ = false;
            configured_ = false;
            retryN_ = 0;
            retryAt_ = 0;
            if (desired_ && !singleDone_) {
                startSession();
            } else {
                configure();
                applyPendingDelete();
            }
            runPendingXtra();   // G2
        }
        if (fn) fn();
        // r5 review F21: a wanted session whose configuration/START failed while LOC stayed present is retried
        if (retryAt_ > 0 && nowMs() >= retryAt_) {
            retryAt_ = 0;
            if (desired_ && !session_ && !singleDone_ && client_ && client_->serviceUp()) startSession();
            else if (desired_ && session_ && client_ && client_->serviceUp()) startSession();   // refused update
            else if (!desired_ && session_ && client_ && client_->serviceUp()) stopSession();   // F52: rejected STOP
        }
    }
    if (session_) stopSession();
    if (client_) client_->close();
    {
        std::lock_guard<std::mutex> lk(clientMu_);
        client_.reset();
    }
}

void GnssEngine::onIndication(const qmi::Message& m) {
    switch (m.msgId) {
        case loc::kIndPosition: {
            loc::Fix f;
            if (!loc::parsePosition(m, &f)) {
                if (log_) log_(kLogWarn, "malformed position report");
                return;
            }
            if (f.status == loc::kStatusSuccess && f.hasLatLon) {
                fixes_++;
                if (single_) post([this] { completeSingle(); });   // F53 (the task checks the session's recurrence)
                {
                    std::lock_guard<std::mutex> lk(usedMu_);
                    lastUsed_ = f.svUsed;
                    lastUsedMs_ = nowMs();
                }
                if (l_) l_->onFix(f);
                if (cfg_.synthesizeNmea && nowMs() - lastModemNmeaMs_ > 2500 && l_) {
                    auto g = nmea::gga(f), r = nmea::rmc(f);
                    if (!g.empty()) l_->onNmea(g, true);
                    if (!r.empty()) l_->onNmea(r, true);
                }
            } else if (f.status == loc::kStatusInProgress) {
                if (l_) l_->onIntermediate(f);
            } else {
                if (log_)
                    log_(kLogWarn, std::string("position report status ") + loc::sessionStatusName(f.status));
                if (f.status == loc::kStatusEngineLocked && cfg_.unlockEngine && !unlockTried_) {
                    post([this] { startSession(); });
                }
            }
            return;
        }
        case loc::kIndSvInfo: {
            std::vector<loc::Sv> svs;
            bool assumed = false;
            if (!loc::parseSvInfo(m, &svs, &assumed)) {
                if (log_) log_(kLogWarn, "malformed SV info");
                return;
            }
            std::vector<uint16_t> used;
            {
                std::lock_guard<std::mutex> lk(usedMu_);
                if (nowMs() - lastUsedMs_ < 3000) used = lastUsed_;
            }
            if (l_) l_->onSvs(svs, used);
            return;
        }
        case loc::kIndNmea: {
            std::vector<std::string> s;
            if (!loc::parseNmea(m, &s)) return;
            lastModemNmeaMs_ = nowMs();
            if (l_)
                for (const auto& x : s) l_->onNmea(x, false);
            return;
        }
        case loc::kIndEngineState: {
            uint32_t v;
            if (loc::parseU32Status(m, &v) && l_) l_->onEngineState(v == 1);
            return;
        }
        case loc::kIndFixSessionState: {
            uint32_t v;
            uint8_t sid = 1;
            if (!loc::parseU32Status(m, &v)) return;
            if (l_) l_->onSessionState(v == loc::kSessionStarted);
            // F52 related: our session (id 1) ended on the modem side: handled on the worker (state owner)
            if (v == loc::kSessionFinished && (!m.getU8(0x10, &sid) || sid == 1)) post([this] { onSessionFinished(); });
            return;
        }
        case loc::kIndInjectTimeReq:
            if (l_) l_->onTimeRequest();
            return;
        case loc::kIndInjectPositionReq:
            if (l_) l_->onPositionRequest();
            return;
        case loc::kIndInjectOrbitsReq:
            if (l_) l_->onOrbitsRequest();
            return;
        case loc::kInjectPredictedOrbits:
        case loc::kGetPredictedOrbitsSource:
        case loc::kGetPredictedOrbitsValidity: {
            {
                std::lock_guard<std::mutex> lk(indMu_);
                auto& q = inds_[m.msgId];
                if (q.size() > 64) q.pop_front();
                q.push_back(m);
            }
            indCv_.notify_all();
            return;
        }
        case loc::kInjectUtcTime:
        case loc::kInjectPosition: {
            uint32_t st = 0;
            if (loc::parseU32Status(m, &st) && log_)
                log_(st == 0 ? kLogInfo : kLogWarn, std::string(m.msgId == loc::kInjectUtcTime ? "INJECT_UTC_TIME"
                                                                                               : "INJECT_POSITION") +
                                                        " indication status " + std::to_string(st) + " (" +
                                                        loc::sessionStatusName(st) + ")");
            return;
        }
        default: {
            uint32_t st = 0;
            bool has = loc::parseU32Status(m, &st);
            if (log_)
                log_(has && st != 0 ? kLogWarn : kLogDebug,
                     "indication " + hex16(m.msgId) + (has ? " status " + std::to_string(st) : std::string()));
            return;
        }
    }
}

}  // namespace a6l
