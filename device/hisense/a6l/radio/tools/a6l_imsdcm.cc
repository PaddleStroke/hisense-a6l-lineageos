// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte2, 25 Sep 2026): a6l-imsdcm = AP-side IMS data connection manager, the
// open replacement for the stock vendor imsdatadaemon. It hosts QMI service IMSDCM (770) over QRTR
// so the modem IMS stack can ask for the IMS PDN, brings that PDN up with WDS (profile given by the
// modem, `ims` = profile 6 on the A6L) and answers with the address. Modem-centric VoLTE: SIP/RTP
// stay on the modem, nothing is routed on the AP.
//
// usage: a6l-imsdcm [--duration <s>] [--imsa]        (0 / no --duration = run until SIGTERM)
//   --imsa: also set up IMSS (18) and watch IMSA (33): bind both to the subscription (stock qcril does
//           this; unbound = INVALID_OPERATION), IMS service enable config (read / enable), register
//           indications, poll; prints A6L_IMSDCM_IMSS / A6L_IMSDCM_IMSA lines   (volte5, 28 Sep)
//   --status: one-shot IMSS read (or enable) + IMSA status, no 770 server; A6L_IMSDCM_STATUS line
// knobs (environment, or vendor properties persist.vendor.a6l.ril.imsdcm_<name> in the ROM):
//   A6L_IMSDCM_MUX=<0..255>  (default 9; 0 = no WDS Bind Mux Data Port)
//   A6L_IMSDCM_PROFILE=<n>   (override the modem's WDS profile)
//   A6L_IMSDCM_ADDR=bin      (address TLV as raw bytes instead of text)
//   A6L_IMSDCM_UNKNOWN=notsup (answer the non-PDP requests NOT_SUPPORTED instead of success)
//   A6L_IMSDCM_INSTANCE=<n>  (QRTR service instance, default 0)
//   A6L_IMSDCM_NOWDS=1       (never start a WDS call: every PDP_ACTIVATE fails; handshake test)
//   A6L_IMSDCM_SUB=<n>|none  (IMSA/IMSS bind subscription, default 0 = primary; none = no bind)
//   A6L_IMSDCM_IMSS=off|read|enable (default read; enable = IMSS Set IMS Service Enable Config
//                            ims_service_enabled=1 volte=1 when the modem reports them off)
//   A6L_IMSDCM_POLL=<s>      (IMSA status poll period, default 15, 0 = indications only)
// volte6 (29 Sep, see docs/volte6-20260929.md):
//   A6L_IMSDCM_KICK=off|force|toggle (default off): re-assert IMS for the subscription like stock
//                            ImsService/qcril do after every SIM load: IMSS 0x008F volte=1 ims_service_enabled=1
//                            written even when already on (force) or 0 then 1 (toggle), then IMSA re-bind.
//                            Triggers: 0x33 SUB_DESTROY_INSTANCE from the modem, LTE reaching full service.
//   A6L_IMSDCM_KICKS=<n>     (max re-asserts, default 3)
//   A6L_IMSDCM_VDP=read|set  (default read): NAS voice_domain_pref / usage / VoPS / LTE voice domain lines;
//                            set = NAS Set System Selection Preference {0x23 = 3} when it is not 3 (stock
//                            "VoLTE on"; reaches the network at the next attach)
//   Every IMSA registration change prints "A6L_IMSDCM_IMSA STATE registered=0|1 src=..." (the test
//   script uses the LAST such line, so a lost registration is not reported as registered).
//   A6L_QMI_LOG=<0..4>       (4 = every QMI message)
#include <a6lqmi/client.h>
#include <a6lqmi/ims.h>
#include <a6lqmi/ims_setup.h>
#include <a6lqmi/imsdcm_service.h>
#include <a6lqmi/log.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

using namespace a6l;
using namespace a6l::qmi;

static std::atomic<bool> gStop{false};
static std::mutex gOut;

static void out(const std::string& s) {
    std::lock_guard<std::mutex> l(gOut);
    printf("%s\n", s.c_str());
    fflush(stdout);
}

// env first (attended CLI runs), then persist.vendor.a6l.ril.imsdcm_<lowercase name> (ROM service)
static std::string knob(const char* env, const char* prop) {
    if (const char* v = getenv(env); v && *v) return v;
#ifdef __ANDROID__
    char buf[PROP_VALUE_MAX] = {};
    std::string p = std::string("persist.vendor.a6l.ril.imsdcm_") + prop;
    if (__system_property_get(p.c_str(), buf) > 0) return buf;
#else
    (void)prop;
#endif
    return {};
}

// volte5: IMSS + IMSA on ONE client socket (the binds are per client). IMSS: bind, read the IMS
// service enable config and (A6L_IMSDCM_IMSS=enable) switch IMS/VoLTE on like Android's ImsService
// does through qcril on stock. IMSA: bind, register indications, read status, then poll every
// POLL s and print changes (a missed indication cannot hide the registration).
struct ImsOpts {
    std::optional<uint32_t> sub = 0;
    radio::ImssMode imss = radio::ImssMode::Read;
    int pollS = 15;
    radio::ImssMode kick = radio::ImssMode::Off;  // Off | Force | Toggle
    int maxKicks = 3;
    radio::VdpMode vdp = radio::VdpMode::Read;
};

// volte6: modem events from the 770 server thread (0x33 destroy) -> the IMS watch thread
static std::mutex gEvLock;
static std::vector<uint32_t> gDestroyed;
static std::atomic<int> gRegState{-1};

static void noteReg(int v, const char* src) {
    int old = gRegState.exchange(v);
    if (old != v) out(std::string("A6L_IMSDCM_IMSA STATE registered=") + std::to_string(v) + " src=" + src);
}

static void watchIms(int durationS, const ImsOpts& o) {
    Client c(makeQrtrTransport(), "ims");
    if (!c.start({kSvcImsa, kSvcImss, kSvcNas})) {
        out("A6L_IMSDCM_IMSA cannot open QRTR");
        return;
    }
    c.onIndication(kSvcImsa, imsa::kRegStatusInd, [](const Message& m) {
        auto rs = imsa::parseRegStatus(m);
        out("A6L_IMSDCM_IMSA_IND reg " + rs.summary());
        if (rs.registered()) out("A6L_IMSDCM_IMSA REGISTERED (indication)");
        noteReg(rs.registered() ? 1 : 0, "indication");
    });
    c.onIndication(kSvcImsa, imsa::kServicesStatusInd, [](const Message& m) {
        out("A6L_IMSDCM_IMSA_IND services " + imsa::parseServicesStatus(m).summary());
    });
    c.onIndication(kSvcImss, imss::kServiceEnableConfigInd, [](const Message& m) {
        out("A6L_IMSDCM_IMSS_IND " + imss::parseServiceEnableConfigInd(m).summary());
    });
    bool attached = false, imssDone = false, regSeen = false, nasDone = false;
    std::string lastReg, lastSvc, lastNas;
    radio::ImsKickPolicy kick(o.maxKicks, 3);
    auto t0 = std::chrono::steady_clock::now();
    auto lastPoll = t0, lastNasPoll = t0;
    int waited = 0;
    while (!gStop) {
        bool imssUp = c.hasService(kSvcImss);
        if (imssUp && !imssDone && o.imss != radio::ImssMode::Off) {
            imssDone = true;
            out("A6L_IMSDCM_IMSS PRESENT after " + std::to_string(waited) + " s (mode=" + radio::imssModeName(o.imss) + ")");
            auto io = radio::imssSetup(c, o.sub, o.imss, out);
            out(std::string("A6L_IMSDCM_IMSS RESULT ims_service_enabled=") + (io.enabled() ? "1" : "0/unknown") +
                " wrote=" + (io.wrote ? (io.writeOk ? "ok" : "failed") : "no"));
        } else if (!imssUp && imssDone) {
            imssDone = false;  // modem restart: do it again when IMSS comes back
            out("A6L_IMSDCM_IMSS GONE");
        }
        // volte6: NAS view of VoLTE (voice domain pref, VoPS, LTE voice domain) + the re-assert policy
        if (c.hasService(kSvcNas) && !nasDone) {
            nasDone = true;
            radio::NasImsState st;
            radio::nasVoiceDomainSetup(c, o.vdp, out, &st);
            lastNas = st.summary();
            kick.onNas(st, waited);
        }
        {
            std::vector<uint32_t> ev;
            {
                std::lock_guard<std::mutex> l(gEvLock);
                ev.swap(gDestroyed);
            }
            for (uint32_t i : ev) kick.onDestroy(i, waited);
        }
        if (nasDone && c.hasService(kSvcNas) &&
            std::chrono::steady_clock::now() - lastNasPoll >= std::chrono::seconds(o.pollS > 0 ? std::min(o.pollS, 10) : 10)) {
            lastNasPoll = std::chrono::steady_clock::now();
            radio::NasImsState st;
            if (radio::readNasImsState(c, &st).ok()) {
                if (st.summary() != lastNas) out("A6L_IMSDCM_NAS now t=" + std::to_string(waited) + "s " + st.summary());
                lastNas = st.summary();
                kick.onNas(st, waited);
            }
        }
        std::string why;
        if (o.kick != radio::ImssMode::Off && c.hasService(kSvcImss) && kick.due(waited, &why)) {
            out("A6L_IMSDCM_KICK n=" + std::to_string(kick.kicks()) + " why=" + why + " mode=" +
                radio::imssModeName(o.kick) + " t=" + std::to_string(waited) + "s");
            auto io = radio::imssSetup(c, o.sub, o.kick, out);
            out(std::string("A6L_IMSDCM_IMSS RESULT ims_service_enabled=") + (io.enabled() ? "1" : "0/unknown") +
                " wrote=" + (io.wrote ? (io.writeOk ? "ok" : "failed") : "no") + " (kick)");
            if (c.hasService(kSvcImsa)) {
                auto a = radio::imsaAttach(c, o.sub, out);
                if (a.regOk) noteReg(a.reg.registered() ? 1 : 0, "kick");
            }
        }
        bool up = c.hasService(kSvcImsa);
        auto now = std::chrono::steady_clock::now();
        if (up && !attached) {
            attached = true;
            out("A6L_IMSDCM_IMSA PRESENT (service 33 published) after " + std::to_string(waited) + " s");
            auto a = radio::imsaAttach(c, o.sub, out);
            lastReg = a.regOk ? a.reg.summary() : "";
            lastSvc = a.svcOk ? a.services.summary() : "";
            if (a.regOk && a.reg.registered() && !regSeen) {
                regSeen = true;
                out("A6L_IMSDCM_IMSA REGISTERED (query)");
            }
            if (a.regOk) noteReg(a.reg.registered() ? 1 : 0, "query");
            lastPoll = now;
        } else if (!up && attached) {
            attached = false;
            out("A6L_IMSDCM_IMSA GONE (service 33 withdrawn)");
            noteReg(0, "gone");
        } else if (up && attached && o.pollS > 0 && now - lastPoll >= std::chrono::seconds(o.pollS)) {
            lastPoll = now;
            imsa::RegStatus rs;
            imsa::ServicesStatus ss;
            auto r1 = imsa::getRegStatus(c, &rs);
            auto r2 = imsa::getServicesStatus(c, &ss);
            std::string reg = r1.ok() ? rs.summary() : r1.describe(), svc = r2.ok() ? ss.summary() : r2.describe();
            if (reg != lastReg || svc != lastSvc)
                out("A6L_IMSDCM_IMSA_POLL t=" + std::to_string(waited) + "s reg " + reg + " | services " + svc);
            lastReg = reg;
            lastSvc = svc;
            if (r1.ok() && rs.registered() && !regSeen) {
                regSeen = true;
                out("A6L_IMSDCM_IMSA REGISTERED (poll)");
            }
            if (r1.ok()) noteReg(rs.registered() ? 1 : 0, "poll");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        waited = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - t0).count());
        if (durationS > 0 && waited >= durationS) break;
    }
    if (!attached) out("A6L_IMSDCM_IMSA ABSENT (service 33 never published)");
    c.stop();
}

// --status: one-shot, no 770 server. IMSS per A6L_IMSDCM_IMSS (default read), IMSA status.
static int statusOnce(const ImsOpts& o) {
    Client c(makeQrtrTransport(), "ims-status");
    if (!c.start({kSvcImsa, kSvcImss, kSvcNas})) {
        out("A6L_IMSDCM_STATUS cannot open QRTR");
        return 1;
    }
    auto miss = c.waitForServices({kSvcImsa, kSvcImss}, 3000);
    for (auto m : miss) out(std::string("A6L_IMSDCM_STATUS absent ") + serviceName(m));
    if (c.hasService(kSvcImss)) radio::imssSetup(c, o.sub, o.imss == radio::ImssMode::Off ? radio::ImssMode::Read : o.imss, out);
    if (c.hasService(kSvcNas)) radio::nasVoiceDomainSetup(c, o.vdp, out);
    bool reg = false;
    if (c.hasService(kSvcImsa)) {
        auto a = radio::imsaAttach(c, o.sub, out);
        reg = a.regOk && a.reg.registered();
        out(std::string("A6L_IMSDCM_STATUS imsa=present registered=") + (reg ? "1" : "0") + " voice=" +
            (a.svcOk && a.services.voiceAvailable() ? "1" : "0"));
    } else {
        out("A6L_IMSDCM_STATUS imsa=absent registered=0 voice=0");
    }
    c.stop();
    return 0;
}

int main(int argc, char** argv) {
    int duration = 0;
    bool imsaWatch = false, status = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--duration") && i + 1 < argc) duration = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--imsa")) imsaWatch = true;
        else if (!strcmp(argv[i], "--status")) status = true;
        else {
            fprintf(stderr, "usage: a6l-imsdcm [--duration <s>] [--imsa] | --status\n");
            return 2;
        }
    }
    std::string lvl = knob("A6L_QMI_LOG", "log");
    gLogLevel = lvl.empty() ? 1 : atoi(lvl.c_str());
    ImsOpts io;
    {
        std::string sv = knob("A6L_IMSDCM_SUB", "sub");
        if (sv == "none") io.sub.reset();
        else if (!sv.empty()) io.sub = static_cast<uint32_t>(atoi(sv.c_str()));
        io.imss = radio::imssModeFromString(knob("A6L_IMSDCM_IMSS", "imss"));
        sv = knob("A6L_IMSDCM_POLL", "poll");
        if (!sv.empty()) io.pollS = atoi(sv.c_str());
        sv = knob("A6L_IMSDCM_KICK", "kick");
        io.kick = (sv == "force" || sv == "toggle") ? radio::imssModeFromString(sv) : radio::ImssMode::Off;
        sv = knob("A6L_IMSDCM_KICKS", "kicks");
        if (!sv.empty()) io.maxKicks = atoi(sv.c_str());
        io.vdp = radio::vdpModeFromString(knob("A6L_IMSDCM_VDP", "vdp"));
    }
    signal(SIGPIPE, SIG_IGN);
    if (status) return statusOnce(io);

    radio::ImsDcmConfig cfg;
    std::string v;
    if (!(v = knob("A6L_IMSDCM_MUX", "mux")).empty()) cfg.muxId = static_cast<uint8_t>(atoi(v.c_str()));
    if (!(v = knob("A6L_IMSDCM_PROFILE", "profile")).empty()) cfg.profileOverride = atoi(v.c_str());
    if (knob("A6L_IMSDCM_ADDR", "addr") == "bin") cfg.addrAsText = false;
    if (knob("A6L_IMSDCM_UNKNOWN", "unknown") == "notsup") cfg.ackUnknown = false;
    if (!(v = knob("A6L_IMSDCM_INSTANCE", "instance")).empty()) cfg.instance = static_cast<uint32_t>(atoi(v.c_str()));
    if (knob("A6L_IMSDCM_NOWDS", "nowds") == "1") cfg.noWds = true;

    struct sigaction sa {};
    sa.sa_handler = [](int) { gStop = true; };
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGINT, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);

    radio::ImsDcmService svc(cfg, makeQrtrTransport(),
                             [](const char* tag) { return std::make_unique<Client>(makeQrtrTransport(), tag); });
    svc.onReport([](const std::string& s) { out(s); });
    svc.onModemEvent([](uint16_t msgId, std::optional<uint32_t> inst) {
        if (msgId != imsdcm::kSubDestroyInstance) return;
        std::lock_guard<std::mutex> l(gEvLock);
        gDestroyed.push_back(inst.value_or(0));
    });
    for (int tries = 0; !svc.start(); tries++) {  // qrtr module may not be loaded yet (boot)
        if (gStop || (duration > 0 && tries * 5 >= duration)) return 1;
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
    std::thread imsaT;
    if (imsaWatch) imsaT = std::thread([duration, io] { watchIms(duration, io); });
    auto t0 = std::chrono::steady_clock::now();
    while (!gStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (duration > 0 && std::chrono::steady_clock::now() - t0 >= std::chrono::seconds(duration)) break;
    }
    gStop = true;
    if (imsaT.joinable()) imsaT.join();
    auto pdps = svc.pdps();
    std::string s = "A6L_IMSDCM_SUMMARY pdps=" + std::to_string(pdps.size());
    for (auto& p : pdps)
        s += " [id=" + std::to_string(p.id) + " " + p.req.apn + " " + radio::ImsDcmService::stateName(p.state) +
             " " + (p.address.empty() ? "-" : p.address) + "]";
    out(s);
    svc.stop();  // stops the IMS PDN(s) and withdraws service 770
    out("A6L_IMSDCM_DONE");
    return 0;
}
