// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): LocClient + GnssEngine.
#include "loc_client.h"

#include <chrono>
#include <cstdio>

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

void GnssEngine::configure() {
    call(loc::makeInformClientRevision(cfg_.clientRevision), "INFORM_CLIENT_REVISION");
    uint64_t mask = loc::kEvPositionReport | loc::kEvGnssSvInfo | loc::kEvNmea | loc::kEvEngineState |
                    loc::kEvFixSessionState | loc::kEvInjectTimeReq | loc::kEvInjectPositionReq |
                    loc::kEvInjectOrbitsReq;
    call(loc::makeRegEvents(mask), "REG_EVENTS");
    call(loc::makeSetOperationMode(cfg_.operationMode), "SET_OPERATION_MODE");
    if (cfg_.configureNmea) call(loc::makeSetNmeaTypes(cfg_.nmeaMask), "SET_NMEA_TYPES");
    configured_ = true;
}

void GnssEngine::startSession() {
    if (!client_ || !client_->serviceUp()) return;
    if (!configured_) configure();
    if (cfg_.coldStart && !coldDone_) {
        call(loc::makeDeleteAllAssistData(), "DELETE_ASSIST_DATA(all)");
        coldDone_ = true;
    }
    if (cfg_.unlockEngine && !unlockTried_) {
        call(loc::makeSetEngineLock(loc::kLockNone), "SET_ENGINE_LOCK(none)");
        unlockTried_ = true;
    }
    int rc = call(loc::makeStart(1, interval_, cfg_.intermediate), "START");
    session_ = rc == 0;
}

void GnssEngine::stopSession() {
    if (client_ && client_->serviceUp()) call(loc::makeStop(1), "STOP");
    session_ = false;
}

void GnssEngine::setActive(bool on) {
    desired_ = on;
    post([this, on] {
        if (on)
            startSession();
        else if (session_)
            stopSession();
    });
}

void GnssEngine::setInterval(uint32_t ms) {
    interval_ = ms < 1000 ? 1000 : ms;
    post([this] {
        if (session_) startSession();   // a new START with the same session id updates the parameters
    });
}

void GnssEngine::injectTime(uint64_t utcMs, uint32_t uncMs) {
    post([this, utcMs, uncMs] {
        if (serviceUp()) call(loc::makeInjectUtcTime(utcMs, uncMs), "INJECT_UTC_TIME");
    });
}

void GnssEngine::injectLocation(double lat, double lon, float accM) {
    post([this, lat, lon, accM] {
        if (serviceUp()) call(loc::makeInjectPosition(lat, lon, accM), "INJECT_POSITION");
    });
}

void GnssEngine::deleteAll() {
    post([this] {
        if (serviceUp()) call(loc::makeDeleteAllAssistData(), "DELETE_ASSIST_DATA(all)");
    });
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
    std::unique_lock<std::mutex> lk(indMu_);
    if (!indCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] { return !inds_[id].empty() || !running_; }))
        return false;
    if (inds_[id].empty()) return false;
    *out = inds_[id].front();
    inds_[id].pop_front();
    return true;
}

bool GnssEngine::doInjectXtra(const std::vector<uint8_t>& f, size_t partSize, bool withFormat, std::string* detail,
                              bool* formatRejected) {
    *formatRejected = false;
    auto parts = loc::buildXtraParts(f, partSize, withFormat);
    clearInd(loc::kInjectPredictedOrbits);
    int missing = 0;
    int64_t t0 = nowMs();
    for (size_t i = 0; i < parts.size(); i++) {
        if (!client_ || !client_->serviceUp()) {
            *detail = "LOC service lost at part " + std::to_string(i + 1);
            return false;
        }
        int rc = client_->request(parts[i], nullptr, 5000);
        if (rc != 0) {
            if (i == 0 && withFormat && rc > 0) *formatRejected = true;
            *detail = "part " + std::to_string(i + 1) + "/" + std::to_string(parts.size()) + " request error " +
                      std::to_string(rc);
            return false;
        }
        qmi::Message ind;
        if (!waitInd(loc::kInjectPredictedOrbits, 5000, &ind)) {
            missing++;
            if (log_) log_(kLogWarn, "XTRA part " + std::to_string(i + 1) + ": no indication within 5 s");
            if (missing >= 3 && size_t(missing) == i + 1) {
                *detail = "no INJECT_PREDICTED_ORBITS indications for the first 3 parts";
                return false;
            }
            continue;
        }
        uint32_t st = 0;
        uint16_t pn = 0;
        loc::parseInjectOrbitsInd(ind, &st, &pn);
        if (st != 0) {
            *detail = "part " + std::to_string(i + 1) + " indication status " + std::to_string(st) +
                      " (" + loc::sessionStatusName(st) + ")";
            return false;
        }
    }
    *detail = "parts=" + std::to_string(parts.size()) + " bytes=" + std::to_string(f.size()) +
              " missing_ind=" + std::to_string(missing) + " format_tlv=" + (withFormat ? "yes" : "no") +
              " ms=" + std::to_string(nowMs() - t0);
    return true;
}

void GnssEngine::injectXtra(std::vector<uint8_t> file, size_t partSize) {
    post([this, f = std::move(file), partSize] {
        std::string why;
        if (!serviceUp()) {
            if (l_) l_->onXtraResult(false, "LOC service not up");
            return;
        }
        if (!loc::looksLikeXtra(f, &why)) {
            if (l_) l_->onXtraResult(false, why);
            return;
        }
        std::string detail;
        bool fmtRejected = false;
        bool ok = doInjectXtra(f, partSize, true, &detail, &fmtRejected);
        if (!ok && fmtRejected) {
            if (log_) log_(kLogWarn, "XTRA: first part rejected (" + detail + "); retrying without formatType TLV");
            ok = doInjectXtra(f, partSize, false, &detail, &fmtRejected);
        }
        if (log_) log_(ok ? kLogInfo : kLogWarn, std::string("XTRA injection ") + (ok ? "done: " : "FAILED: ") + detail);
        if (l_) l_->onXtraResult(ok, detail);
        if (ok) doQueryXtra();
    });
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
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait_for(lk, std::chrono::milliseconds(client_ ? 500 : 250), [this] {
                return !running_ || !q_.empty() || pendingServiceUp_ || pendingServiceDown_;
            });
            if (!q_.empty()) {
                fn = std::move(q_.front());
                q_.pop_front();
            }
        }
        if (!running_) break;
        if (pendingServiceDown_.exchange(false)) {
            session_ = false;
            configured_ = false;
        }
        if (pendingServiceUp_.exchange(false)) {
            session_ = false;
            configured_ = false;
            configure();
            if (desired_) startSession();
        }
        if (fn) fn();
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
            if (loc::parseU32Status(m, &v) && l_) l_->onSessionState(v == 1);
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
