// SPDX-License-Identifier: Apache-2.0
// a6l_gnss_test (agent gnss, 24 Sep 2026): standalone GNSS check for the Hisense A6L modem's QMI LOC service.
// Runs as root in the V74 recovery RAM session (static NDK binary) or in the installed ROM, with the modem up
// (radio2 order: qrtr, rmtfs, tqftpserv, diag-router, modem started). It does NOT touch RF settings beyond asking the
// modem's GNSS engine for standalone fixes (the GNSS receiver is receive-only).
//
//   a6l_gnss_test --list                      list every QRTR service the name service knows (like qrtr-lookup)
//   a6l_gnss_test [--seconds 300]             lookup LOC, configure, START, print fixes / NMEA / satellites, STOP
//   options: --interval MS  --mode standalone|default|msb  --cold (delete assistance data first)
//            --unlock (SET_ENGINE_LOCK none; writes modem NV, only if reports say ENGINE_LOCKED)
//            --no-nmea-config  --no-synth  --record FILE (packet log)  --replay FILE (offline, fake modem)
//            --wait-service SEC (default 30)  --inject-time (inject the system UTC time, 5 s uncertainty)  -v
//   assistance (misc agent, 24 Sep): --xtra FILE (inject a Qualcomm XTRA file, e.g. xtra2.bin / xtra3grc.bin pushed
//            from the laptop; parts of --xtra-part N bytes, default 1024) --xtra-query (ask the modem which XTRA
//            servers/sizes it wants and how long its current XTRA data is valid)
//            --inject-pos LAT,LON[,ACC_M] (coarse position, e.g. from the serving cell; default ACC 5000 m)
//            --pos-src cell|other (positionSrc TLV, default cell)  --xtra-required (exit 5 before START if the
//            modem rejects the XTRA file)
//            With any injection, START is sent only after time -> position -> XTRA have been injected.
//   --nmea-only-log [FILE]  quiet console (fixes, XTRA/injection results, SV summary every 30 s, result line);
//            raw NMEA sentences go to FILE (default /tmp/a6l-gnss.nmea)
// Output lines start with "A6L_GNSS"; the last line is
//   A6L_GNSS_RESULT result=FIX|NO_FIX|NO_SERVICE|NO_QRTR fixes=N nmea=N sv_reports=N max_cn0=X
// exit code: 0 FIX, 2 NO_FIX (service ok), 3 NO_SERVICE, 4 NO_QRTR (AF_QIPCRTR missing), 1 usage.
#include <signal.h>
#include <stdarg.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "fake_modem.h"
#include "loc_client.h"
#include "loc_v02.h"
#include "nmea.h"
#include "qmi.h"
#include "transport.h"

using namespace a6l;

static std::atomic<bool> gStop{false};
static std::mutex gOut;
static bool gVerbose = false;
static int64_t gT0 = 0;
static bool gQuiet = false;
static FILE* gNmeaFile = nullptr;

static int64_t monoMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
}

static void out(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void out(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(gOut);
    printf("[%7.1f] ", (monoMs() - gT0) / 1000.0);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static const char* consName(int c) {
    static const char* n[] = {"?", "G", "S", "R", "J", "C", "E", "I"};
    return c >= 0 && c <= 7 ? n[c] : "?";
}

class Printer : public EngineListener {
  public:
    std::atomic<int> fixes{0}, nmeaCount{0}, svReports{0}, intermediates{0};
    std::atomic<bool> serviceSeen{false};
    float maxCn0 = 0;
    FILE* rec = nullptr;

    void onFix(const loc::Fix& f) override {
        fixes++;
        char utc[40] = "-";
        if (f.hasUtc) {
            time_t t = time_t(f.utcMs / 1000);
            struct tm tm;
            gmtime_r(&t, &tm);
            strftime(utc, sizeof(utc), "%Y-%m-%dT%H:%M:%SZ", &tm);
        }
        out("A6L_GNSS FIX lat=%.7f lon=%.7f acc=%.1fm alt=%.1fm speed=%.2fm/s bearing=%.1f utc=%s used=%zu hdop=%.1f",
            f.latitude, f.longitude, f.hasHorUnc ? f.horUncCircular : -1.f,
            f.hasAltEllipsoid ? f.altEllipsoid : NAN, f.hasSpeed ? f.speedHorizontal : NAN,
            f.hasHeading ? f.heading : NAN, utc, f.svUsed.size(), f.hasDop ? f.hdop : NAN);
    }
    void onIntermediate(const loc::Fix& f) override {
        intermediates++;
        if (f.hasLatLon)
            out("A6L_GNSS INTERMEDIATE lat=%.5f lon=%.5f acc=%.0fm", f.latitude, f.longitude,
                f.hasHorUnc ? f.horUncCircular : -1.f);
        else if (gVerbose || (!gQuiet && intermediates % 10 == 1))
            out("A6L_GNSS INTERMEDIATE (no position yet, %d reports)", intermediates.load());
    }
    void onSvs(const std::vector<loc::Sv>& svs, const std::vector<uint16_t>& used) override {
        svReports++;
        int tracked = 0;
        std::string best;
        std::vector<std::pair<float, std::string>> list;
        for (const auto& s : svs) {
            auto a = loc::toAndroid(s.system, s.svId);
            if ((s.valid & loc::kSvValidSnr) && s.snr > 0) {
                tracked++;
                if (s.snr > maxCn0) maxCn0 = s.snr;
                char b[32];
                snprintf(b, sizeof(b), "%s%02d:%.0f", consName(a.constellation), a.svid, s.snr);
                list.emplace_back(s.snr, b);
            }
        }
        std::sort(list.begin(), list.end(), [](auto& x, auto& y) { return x.first > y.first; });
        for (size_t i = 0; i < list.size() && i < 8; i++) best += " " + list[i].second;
        if (gVerbose || (gQuiet ? svReports % 30 == 1 : svReports % 5 == 1))
            out("A6L_GNSS SV total=%zu with_cn0=%d used=%zu best:%s", svs.size(), tracked, used.size(), best.c_str());
    }
    void onNmea(const std::string& s, bool synthetic) override {
        nmeaCount++;
        if (gNmeaFile) {
            std::lock_guard<std::mutex> lk(gOut);
            fprintf(gNmeaFile, "%s\n", s.c_str());
            fflush(gNmeaFile);
        }
        if (!gQuiet) out("A6L_GNSS NMEA%s %s", synthetic ? "(synth)" : "", s.c_str());
    }
    void onEngineState(bool on) override { out("A6L_GNSS ENGINE %s", on ? "on" : "off"); }
    void onSessionState(bool st) override { out("A6L_GNSS SESSION %s", st ? "started" : "finished"); }
    void onServiceState(bool up) override {
        if (up) serviceSeen = true;
        out("A6L_GNSS SERVICE %s", up ? "up" : "down");
    }
    void onTimeRequest() override { out("A6L_GNSS modem requests time injection"); }
    void onPositionRequest() override { out("A6L_GNSS modem requests position injection"); }
    void onOrbitsRequest() override { out("A6L_GNSS modem requests XTRA (predicted orbits) injection"); }
    std::atomic<int> xtraDone{0};   // 0 pending, 1 ok, 2 failed
    void onXtraResult(bool ok, const std::string& d) override {
        out("A6L_GNSS XTRA_%s %s", ok ? "OK" : "FAIL", d.c_str());
        xtraDone = ok ? 1 : 2;
    }
    void onXtraInfo(const std::string& d) override { out("A6L_GNSS XTRA_INFO %s", d.c_str()); }
    void onPacket(char dir, const QrtrAddr& a, const std::vector<uint8_t>& p) override {
        if (rec) {
            std::lock_guard<std::mutex> lk(gOut);
            fprintf(rec, "%lld %c %u:%u %s\n", (long long)(monoMs() - gT0), dir, a.node, a.port,
                    qmi::hex(p.data(), p.size()).c_str());
            fflush(rec);
        }
        if (gVerbose) {
            qmi::Message m;
            std::string e;
            if (qmi::decode(p.data(), p.size(), &m, &e))
                out("A6L_GNSS %s type=%u msg=0x%04x txn=%u tlvs=%zu", dir == '>' ? "TX" : "RX", m.type, m.msgId, m.txn,
                    m.tlvs.size());
        }
    }
};

// Replay: fake modem answering every request, then emitting the recorded indications ('<' lines, QMI type 4)
// with their original spacing (divided by --speed) once START has been received.
struct ReplayPkt {
    int64_t ms;
    std::vector<uint8_t> data;
};

static bool loadReplay(const char* path, std::vector<ReplayPkt>* pkts) {
    std::ifstream in(path);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        long long ms;
        char dir;
        std::string addr, hex;
        if (!(ss >> ms >> dir >> addr >> hex) || dir != '<') continue;
        ReplayPkt p{ms, {}};
        if (!qmi::unhex(hex, &p.data)) continue;
        if (p.data.empty() || p.data[0] != qmi::kIndication) continue;
        pkts->push_back(std::move(p));
    }
    return true;
}

static void usage() {
    fprintf(stderr,
            "usage: a6l_gnss_test [--list] [--seconds N] [--interval MS] [--mode standalone|default|msb] [--cold]\n"
            "                     [--unlock] [--no-nmea-config] [--no-synth] [--record FILE] [--replay FILE [--speed X]]\n"
            "                     [--wait-service SEC] [--inject-time] [-v]\n");
}

int main(int argc, char** argv) {
    gT0 = monoMs();
    int seconds = 300, waitService = 30;
    double speed = 1.0;
    bool list = false, injectTime = false, xtraQuery = false, havePos = false, xtraRequired = false;
    const char *recPath = nullptr, *replayPath = nullptr, *xtraPath = nullptr;
    const char* nmeaPath = "/tmp/a6l-gnss.nmea";
    size_t xtraPart = loc::kMaxOrbitsPart;
    double posLat = 0, posLon = 0;
    float posAcc = 5000;
    uint32_t posSrc = 1;   // CELLID
    EngineConfig cfg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](const char* what) -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", what);
                exit(1);
            }
            return argv[++i];
        };
        if (a == "--list") list = true;
        else if (a == "--seconds") seconds = atoi(next("--seconds"));
        else if (a == "--interval") cfg.intervalMs = uint32_t(atoi(next("--interval")));
        else if (a == "--mode") {
            std::string m = next("--mode");
            if (m == "standalone") cfg.operationMode = loc::kModeStandalone;
            else if (m == "default") cfg.operationMode = loc::kModeDefault;
            else if (m == "msb") cfg.operationMode = loc::kModeMsb;
            else { usage(); return 1; }
        } else if (a == "--cold") cfg.coldStart = true;
        else if (a == "--unlock") cfg.unlockEngine = true;
        else if (a == "--no-nmea-config") cfg.configureNmea = false;
        else if (a == "--no-synth") cfg.synthesizeNmea = false;
        else if (a == "--record") recPath = next("--record");
        else if (a == "--replay") replayPath = next("--replay");
        else if (a == "--speed") speed = atof(next("--speed"));
        else if (a == "--wait-service") waitService = atoi(next("--wait-service"));
        else if (a == "--inject-time") injectTime = true;
        else if (a == "--xtra") xtraPath = next("--xtra");
        else if (a == "--xtra-part") xtraPart = size_t(atoi(next("--xtra-part")));
        else if (a == "--xtra-query") xtraQuery = true;
        else if (a == "--xtra-required") xtraRequired = true;
        else if (a == "--inject-pos") {
            const char* v = next("--inject-pos");
            int n = sscanf(v, "%lf,%lf,%f", &posLat, &posLon, &posAcc);
            if (n < 2 || posLat < -90 || posLat > 90 || posLon < -180 || posLon > 180 || posAcc <= 0) {
                fprintf(stderr, "--inject-pos LAT,LON[,ACC_M] (decimal degrees)\n");
                return 1;
            }
            havePos = true;
        } else if (a == "--pos-src") {
            std::string v = next("--pos-src");
            posSrc = v == "cell" ? 1 : v == "other" ? 6 : 99;
            if (posSrc == 99) { usage(); return 1; }
        } else if (a == "--nmea-only-log") {
            gQuiet = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') nmeaPath = argv[++i];
        }
        else if (a == "-v") gVerbose = true;
        else { usage(); return 1; }
    }
    signal(SIGINT, [](int) { gStop = true; });
    signal(SIGTERM, [](int) { gStop = true; });
    LogFn log = [](int lvl, const std::string& m) {
        if (lvl >= kLogInfo || gVerbose) out("A6L_GNSS LOG %s %s", lvl >= kLogError ? "E" : lvl == kLogWarn ? "W" : "I", m.c_str());
    };

    if (list) {
        auto t = makeQrtrTransport(log);
        std::string err;
        LocClient c(std::move(t), log, 0);
        int n = 0;
        c.setAnyServerHandler([&](bool up, const QrtrCtrl& s) {
            n++;
            out("A6L_GNSS QRTR %s service=%u version=%u instance=%u node=%u port=%u%s", up ? "server" : "gone", s.service,
                s.instance & 0xff, s.instance >> 8, s.node, s.port, s.service == loc::kServiceId ? "  <- LOC" : "");
        });
        if (!c.open(&err)) {
            out("A6L_GNSS_RESULT result=NO_QRTR error=\"%s\"", err.c_str());
            return 4;
        }
        for (int i = 0; i < 30 && !c.lookupDone(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        out("A6L_GNSS_RESULT result=%s services=%d", n ? "LIST" : "EMPTY", n);
        return n ? 0 : 3;
    }

    Printer p;
    if (recPath) {
        p.rec = fopen(recPath, "w");
        if (!p.rec) {
            perror(recPath);
            return 1;
        }
        fprintf(p.rec, "# a6l_gnss_test packet log: <ms> <dir: > tx, < rx> <node:port> <hex QMI>\n");
    }

    std::shared_ptr<FakeModem> fake;
    std::vector<ReplayPkt> replay;
    GnssEngine::TransportFactory factory;
    if (replayPath) {
        if (!loadReplay(replayPath, &replay)) {
            perror(replayPath);
            return 1;
        }
        out("A6L_GNSS replay: %zu recorded indications from %s (fake modem, no QRTR)", replay.size(), replayPath);
        fake = std::make_shared<FakeModem>();
        factory = [fake] { return std::unique_ptr<Transport>(new FakeModemTransport(fake)); };
    } else {
        factory = [log] { return makeQrtrTransport(log); };
    }

    std::atomic<bool> noQrtr{false};
    LogFn engLog = [&](int lvl, const std::string& m) {
        if (m.find("modem transport unavailable") != std::string::npos) noQrtr = true;
        log(lvl, m);
    };
    if (gQuiet) {
        gNmeaFile = fopen(nmeaPath, "w");
        if (!gNmeaFile) {
            perror(nmeaPath);
            return 1;
        }
        out("A6L_GNSS quiet mode: NMEA -> %s", nmeaPath);
    }
    std::vector<uint8_t> xtraData;
    if (xtraPath) {
        std::ifstream xf(xtraPath, std::ios::binary);
        if (!xf) {
            perror(xtraPath);
            return 1;
        }
        xtraData.assign(std::istreambuf_iterator<char>(xf), std::istreambuf_iterator<char>());
        std::string why;
        if (!loc::looksLikeXtra(xtraData, &why)) {
            out("A6L_GNSS XTRA_FAIL %s: %s", xtraPath, why.c_str());
            return 1;
        }
        out("A6L_GNSS XTRA file %s: %zu bytes, %zu parts of <= %zu", xtraPath, xtraData.size(),
            (xtraData.size() + xtraPart - 1) / (xtraPart ? xtraPart : 1), xtraPart);
    }
    GnssEngine eng(factory, &p, cfg, engLog);
    eng.begin();
    bool timeInjected = false;
    bool assisted = injectTime || havePos || xtraPath || xtraQuery;
    if (assisted) {
        // time -> coarse position -> XTRA, all before START (the worker executes them in this order)
        int64_t until = monoMs() + int64_t(waitService) * 1000;
        while (!gStop && !eng.serviceUp() && monoMs() < until) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (eng.serviceUp()) {
            if (injectTime) {
                struct timespec ts;
                clock_gettime(CLOCK_REALTIME, &ts);
                uint64_t ms = uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec / 1000000);
                if (ts.tv_sec > 1735689600) {   // 2025-01-01: only a plausible clock
                    eng.injectTime(ms, 5000);
                    out("A6L_GNSS injected UTC time %llu", (unsigned long long)ms);
                } else {
                    out("A6L_GNSS system clock not set (date -u MMDDhhmmYYYY.ss first); time NOT injected");
                }
                timeInjected = true;
            }
            if (havePos) {
                eng.injectCoarseLocation(posLat, posLon, posAcc, posSrc);
                out("A6L_GNSS injecting coarse position %.5f,%.5f acc=%.0fm src=%s", posLat, posLon, posAcc,
                    posSrc == 1 ? "cell" : "other");
            }
            if (xtraQuery && !xtraPath) eng.queryXtra();
            if (xtraPath) {
                eng.injectXtra(xtraData, xtraPart);
                int64_t xdl = monoMs() + 120000;
                while (!gStop && p.xtraDone == 0 && monoMs() < xdl)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                if (p.xtraDone == 0) out("A6L_GNSS XTRA_FAIL no result after 120 s");
                if (xtraRequired && p.xtraDone != 1) {
                    eng.end();
                    if (gNmeaFile) fclose(gNmeaFile);
                    out("A6L_GNSS_RESULT result=XTRA_FAIL (--xtra-required: no START sent)");
                    return 5;
                }
            }
        }
    }
    eng.setActive(true);

    std::thread replayer;
    std::atomic<bool> replayDone{false};
    if (fake) {
        replayer = std::thread([&] {
            fake->waitFor([&] { return fake->count(loc::kStart) > 0 || gStop; }, 10000);
            int64_t base = replay.empty() ? 0 : replay[0].ms, start = monoMs();
            for (auto& r : replay) {
                int64_t due = start + int64_t((r.ms - base) / (speed > 0 ? speed : 1));
                while (!gStop && monoMs() < due) std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (gStop) break;
                qmi::Message m;
                std::string e;
                if (qmi::decode(r.data.data(), r.data.size(), &m, &e)) fake->inject(m);
            }
            replayDone = true;
        });
    }

    int64_t deadline = monoMs() + int64_t(seconds) * 1000;
    int64_t svcDeadline = monoMs() + int64_t(waitService) * 1000;
    while (!gStop && monoMs() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (!p.serviceSeen && monoMs() > svcDeadline) {
            out("A6L_GNSS no LOC service after %d s (modem not running? radio2 order: qrtr, rmtfs, tqftpserv, diag-router, echo start)",
                waitService);
            break;
        }
        if (injectTime && !timeInjected && eng.serviceUp()) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t ms = uint64_t(ts.tv_sec) * 1000 + uint64_t(ts.tv_nsec / 1000000);
            if (ts.tv_sec > 1735689600) {   // 2025-01-01: only a plausible clock
                eng.injectTime(ms, 5000);
                out("A6L_GNSS injected UTC time %llu", (unsigned long long)ms);
            } else {
                out("A6L_GNSS system clock not set; time NOT injected");
            }
            timeInjected = true;
        }
        if (fake && replayDone) {   // replay: stop shortly after the last recorded indication
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            break;
        }
    }
    gStop = true;
    eng.setActive(false);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    eng.end();
    if (replayer.joinable()) replayer.join();
    if (p.rec) fclose(p.rec);
    if (gNmeaFile) fclose(gNmeaFile);

    const char* result;
    int rc;
    if (p.fixes > 0) {
        result = "FIX";
        rc = 0;
    } else if (p.serviceSeen) {
        result = "NO_FIX";
        rc = 2;
    } else if (noQrtr) {
        result = "NO_QRTR";
        rc = 4;
    } else {
        result = "NO_SERVICE";
        rc = 3;
    }
    out("A6L_GNSS_RESULT result=%s fixes=%d nmea=%d sv_reports=%d intermediate=%d max_cn0=%.1f last_err=%d xtra=%s",
        result, p.fixes.load(), p.nmeaCount.load(), p.svReports.load(), p.intermediates.load(), p.maxCn0,
        eng.lastRequestError(), !xtraPath ? "none" : p.xtraDone == 1 ? "ok" : "fail");
    return rc;
}
