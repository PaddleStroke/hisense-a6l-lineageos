// SPDX-License-Identifier: Apache-2.0
// Host tests of the A6L USB gadget core (android-usb, 29 Sep 2026) against a fake configfs/functionfs/sysfs tree.
// Kernel behaviour modelled: UDC attribute (name when bound, "none"/empty when not), FunctionFS endpoint files that
// appear once the daemon wrote its descriptors and vanish (with an implicit UDC unbind) when it closes ep0.
#include "a6l_gadget_core.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>
#include <string>
#include <thread>

using namespace a6l::usb;

static int fails = 0, checks = 0;
#define CHECK(c, msg)                                                   \
    do {                                                                \
        checks++;                                                       \
        if (!(c)) {                                                     \
            fails++;                                                    \
            fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, msg); \
        }                                                               \
    } while (0)

static std::string R;
static std::string rd(const std::string& p) {
    std::ifstream f(p);
    std::stringstream s;
    s << f.rdbuf();
    std::string v = s.str();
    while (!v.empty() && v.back() == '\n') v.pop_back();
    return v;
}
static void wr(const std::string& p, const std::string& v) {
    std::ofstream f(p, std::ios::trunc);
    f << v;
}
static void mk(const std::string& p) {
    std::string cmd = "mkdir -p '" + p + "'";
    if (system(cmd.c_str()) != 0) abort();
}
static std::string link(const std::string& p) {
    char b[512];
    ssize_t n = readlink(p.c_str(), b, sizeof(b) - 1);
    if (n < 0) return "";
    b[n] = 0;
    return b;
}
static bool isLink(const std::string& p) {
    struct stat st;
    return lstat(p.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}
static void eps(const std::string& inst, int n, bool on) {
    for (int i = 1; i <= n; i++) {
        std::string p = R + "/ffs/" + inst + "/ep" + std::to_string(i);
        if (on)
            wr(p, "");
        else
            unlink(p.c_str());
    }
}
static std::string G() { return R + "/config/usb_gadget/g1"; }

static void setupTree(bool midi) {
    char tmpl[] = "/tmp/a6l-usb-test-XXXXXX";
    R = mkdtemp(tmpl);
    for (const char* a : {"idVendor", "idProduct", "bDeviceClass", "bDeviceSubClass", "bDeviceProtocol", "UDC"}) {
        mk(G());
        wr(G() + "/" + a, "");
    }
    mk(G() + "/configs/b.1/strings/0x409");
    wr(G() + "/configs/b.1/strings/0x409/configuration", "");
    wr(G() + "/configs/b.1/MaxPower", "500");
    for (const char* f : {"ffs.adb", "ffs.mtp", "ffs.ptp", "rndis.gs4", "ncm.gs9"}) mk(G() + "/functions/" + f);
    if (midi) mk(G() + "/functions/midi.gs5");
    for (const char* f : {"adb", "mtp", "ptp"}) {
        mk(R + "/ffs/" + f);
        wr(R + "/ffs/" + f + "/ep0", "");
    }
    mk(R + "/udc/a800000.usb");
    wr(R + "/udc/a800000.usb/soft_connect", "");
    wr(R + "/udc/a800000.usb/current_speed", "high-speed\n");
}

static bool waitFor(std::function<bool()> f, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return f();
}

static void testCompose() {
    struct {
        uint64_t f;
        bool ok;
        unsigned pid;
        const char* cfg;
        size_t nfn;
        size_t nffs;
    } t[] = {
            {kNone, true, 0, "", 0, 0},
            {kAdb, true, 0x4ee7, "adb", 1, 1},
            {kMtp, true, 0x4ee1, "mtp", 1, 1},
            {kMtp | kAdb, true, 0x4ee2, "mtp_adb", 2, 2},
            {kPtp, true, 0x4ee5, "ptp", 1, 1},
            {kPtp | kAdb, true, 0x4ee6, "ptp_adb", 2, 2},
            {kRndis, true, 0x4ee3, "rndis", 1, 0},
            {kRndis | kAdb, true, 0x4ee4, "rndis_adb", 2, 1},
            {kMidi, true, 0x4ee8, "midi", 1, 0},
            {kMidi | kAdb, true, 0x4ee9, "midi_adb", 2, 1},
            {kNcm, true, 0x4eeb, "ncm", 1, 0},
            {kNcm | kAdb, true, 0x4eec, "ncm_adb", 2, 1},
            {kAccessory, false, 0, "", 0, 0},
            {kAccessory | kAdb, false, 0, "", 0, 0},
            {kAudioSource, false, 0, "", 0, 0},
            {kUvc, false, 0, "", 0, 0},
            {kCtrl | kMtp, false, 0, "", 0, 0},
            {kMtp | kPtp, false, 0, "", 0, 0},
            {kRndis | kNcm | kAdb, false, 0, "", 0, 0},
            {1ull << 40, false, 0, "", 0, 0},
    };
    for (auto& x : t) {
        Composition c = compose(x.f);
        char m[96];
        snprintf(m, sizeof(m), "compose 0x%llx", (unsigned long long)x.f);
        CHECK(c.supported == x.ok, m);
        if (!x.ok) continue;
        CHECK(c.pid == x.pid, m);
        CHECK(c.configName == x.cfg, m);
        CHECK(c.functions.size() == x.nfn, m);
        CHECK(c.ffs.size() == x.nffs, m);
        if (x.f & kAdb) CHECK(c.functions.back() == "ffs.adb", "adb linked last");
    }
    CHECK(compose(kRndis).iad && compose(kNcm).iad && !compose(kMtp).iad, "IAD class for rndis/ncm only");
    CHECK(compose(kMtp).ffs[0].endpoints.size() == 3 && compose(kAdb).ffs[0].endpoints.size() == 2, "ep sets");
}

static unsigned sLogBind = 0;
static void quiet(int, const std::string& m) {
    if (m.rfind("bind:", 0) == 0) sLogBind++;
    if (getenv("A6L_USB_TEST_VERBOSE")) fprintf(stderr, "  log: %s\n", m.c_str());
}

static void testFlows() {
    setupTree(false);
    Paths p{G(), R + "/ffs", R + "/udc"};
    std::string ctl = "a800000.usb";
    Gadget g(p, [&] { return ctl; }, quiet);
    Controller c(&g, quiet, 20);
    c.start();

    // 1. adb requested before adbd wrote descriptors: not bound within the timeout, bound by the monitor later
    auto r = c.setFunctions(kAdb, 200);
    CHECK(r == Controller::Result::kError, "adb without descriptors -> ERROR after timeout");
    CHECK(!g.bound() && !c.applied(), "not bound without descriptors");
    CHECK(link(G() + "/configs/b.1/f1") == G() + "/functions/ffs.adb", "f1 -> ffs.adb");
    CHECK(rd(G() + "/idProduct") == "0x4ee7" && rd(G() + "/idVendor") == "0x18d1", "adb ids");
    eps("adb", 2, true);
    CHECK(waitFor([&] { return g.bound(); }, 2000), "monitor binds once ep1/ep2 exist");
    CHECK(rd(G() + "/UDC") == "a800000.usb", "UDC = controller");
    CHECK(rd(R + "/udc/a800000.usb/soft_connect") == "connect", "soft_connect connect after bind (a6l_manual_usb)");
    CHECK(c.applied() && c.bindCount() == 1, "applied, 1 bind");

    // 2. mtp,adb with system_server's MTP descriptors present: immediate success
    eps("mtp", 3, true);
    wr(R + "/udc/a800000.usb/soft_connect", "");
    r = c.setFunctions(kMtp | kAdb, 2500);
    CHECK(r == Controller::Result::kSuccess, "mtp,adb SUCCESS");
    CHECK(link(G() + "/configs/b.1/f1") == G() + "/functions/ffs.mtp", "f1 -> ffs.mtp");
    CHECK(link(G() + "/configs/b.1/f2") == G() + "/functions/ffs.adb", "f2 -> ffs.adb");
    CHECK(!isLink(G() + "/configs/b.1/f3"), "no stale f3");
    CHECK(rd(G() + "/idProduct") == "0x4ee2", "mtp_adb pid");
    CHECK(rd(G() + "/configs/b.1/strings/0x409/configuration") == "mtp_adb", "config string");
    CHECK(rd(G() + "/bDeviceClass") == "0x0", "no IAD class for mtp");
    CHECK(rd(R + "/udc/a800000.usb/soft_connect") == "connect", "re-connect after re-bind");

    // 3. USB tethering (rndis,adb): old links replaced, IAD class
    r = c.setFunctions(kRndis | kAdb, 2500);
    CHECK(r == Controller::Result::kSuccess, "rndis,adb SUCCESS");
    CHECK(link(G() + "/configs/b.1/f1") == G() + "/functions/rndis.gs4", "f1 -> rndis.gs4");
    CHECK(link(G() + "/configs/b.1/f2") == G() + "/functions/ffs.adb", "f2 -> ffs.adb (rndis)");
    CHECK(rd(G() + "/bDeviceClass") == "0xef" && rd(G() + "/bDeviceSubClass") == "0x2" &&
                  rd(G() + "/bDeviceProtocol") == "0x1",
          "IAD device class for rndis");
    CHECK(rd(G() + "/idProduct") == "0x4ee4", "rndis_adb pid");

    // 4. adbd restarts: kernel drops the endpoints and unbinds; the monitor re-binds when they come back
    unsigned before = c.bindCount();
    eps("adb", 2, false);
    wr(G() + "/UDC", "");
    CHECK(waitFor([&] { return !c.applied(); }, 7000), "unbind noticed");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(!g.bound(), "no bind while adb endpoints are missing");
    eps("adb", 2, true);
    CHECK(waitFor([&] { return g.bound() && c.applied(); }, 3000), "re-bound after adbd restart");
    CHECK(c.bindCount() == before + 1, "exactly one re-bind");

    // 5. reset(): pull down and up again
    before = c.bindCount();
    CHECK(c.reset(), "reset ok");
    CHECK(waitFor([&] { return c.bindCount() == before + 1 && g.bound(); }, 2000), "reset re-binds");

    // 6. charging only
    r = c.setFunctions(kNone, 2500);
    CHECK(r == Controller::Result::kSuccess, "NONE SUCCESS");
    CHECK(!g.bound() && rd(G() + "/UDC") == "none", "NONE -> UDC none");
    CHECK(!isLink(G() + "/configs/b.1/f1") && !isLink(G() + "/configs/b.1/f2"), "NONE -> no links");
    CHECK(c.applied() && c.currentFunctions() == kNone, "NONE applied");
    CHECK(!c.reset(), "reset without a bound composition fails");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK(!g.bound(), "monitor does not bind in charging-only mode");

    // 7. MIDI without usb_f_midi.ko (no functions/midi.gs5) and AOA accessory: not supported, gadget left down
    r = c.setFunctions(kMidi | kAdb, 500);
    CHECK(r == Controller::Result::kNotSupported, "midi without module -> CONFIGURATION_NOT_SUPPORTED");
    CHECK(!g.bound() && !isLink(G() + "/configs/b.1/f1"), "midi: nothing linked/bound");
    r = c.setFunctions(kAccessory, 500);
    CHECK(r == Controller::Result::kNotSupported, "accessory -> CONFIGURATION_NOT_SUPPORTED");
    mk(G() + "/functions/midi.gs5");
    r = c.setFunctions(kMidi, 500);
    CHECK(r == Controller::Result::kSuccess && g.bound(), "midi with the function dir -> bound");

    // 8. a newer request supersedes one still waiting for descriptors
    eps("ptp", 3, false);
    auto slow = std::async(std::launch::async, [&] { return c.setFunctions(kPtp | kAdb, 3000); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto t0 = std::chrono::steady_clock::now();
    r = c.setFunctions(kMtp | kAdb, 2500);
    auto rs = slow.get();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(r == Controller::Result::kSuccess, "newer mtp,adb wins");
    CHECK(rs == Controller::Result::kError, "superseded ptp request reports ERROR");
    CHECK(ms < 1500, "superseded request released promptly");
    CHECK(link(G() + "/configs/b.1/f1") == G() + "/functions/ffs.mtp", "gadget carries the newer composition");

    // 9. ptp once its descriptors exist
    eps("ptp", 3, true);
    r = c.setFunctions(kPtp, 2500);
    CHECK(r == Controller::Result::kSuccess && rd(G() + "/idProduct") == "0x4ee5", "ptp");

    // 10. controller name from /sys/class/udc when sys.usb.controller is unset
    ctl = "";
    CHECK(g.udc() == "a800000.usb", "udc fallback from class dir");
    r = c.setFunctions(kNcm | kAdb, 2500);
    CHECK(r == Controller::Result::kSuccess && rd(G() + "/UDC") == "a800000.usb", "ncm,adb via fallback udc");
    CHECK(g.currentSpeed() == "high-speed", "current_speed");

    // 11. no controller at all -> ERROR, nothing bound; the retries do not flood the log (r6c: QEMU logged 562 lines)
    rename((R + "/udc/a800000.usb").c_str(), (R + "/udc-off").c_str());
    sLogBind = 0;
    ctl = "a800000.usb";  // sys.usb.controller names a UDC that does not exist (QEMU): not used
    CHECK(g.udc() == "", "sys.usb.controller without /sys/class/udc/<name> -> no UDC");
    r = c.setFunctions(kMtp, 300);
    CHECK(r == Controller::Result::kError && !g.bound(), "no UDC -> ERROR");
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    CHECK(g.bindFailures() >= 3, "monitor keeps retrying");
    CHECK(sLogBind <= 2, "bind failures logged once (rate-limited)");
    rename((R + "/udc-off").c_str(), (R + "/udc/a800000.usb").c_str());
    CHECK(waitFor([&] { return g.bound(); }, 4000), "UDC appears -> monitor binds (within the 2 s back-off)");
    c.stop();
    std::string cmd = "rm -rf '" + R + "'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "cleanup failed\n");
}

// r6c (30 Sep 2026): the r6b boot - init's early-adb rule may already have bound adb when usbd asks the HAL for adb
// (adopt, no unbind), and a gadget the HAL cannot write (EACCES: configfs attributes are root 0644 unless init chowns
// them) must be reported as kApplyFailed so UsbGadget hands it back to init (vendor.a6l.usb.hal=0).
static void testAdoptAndHandBack() {
    setupTree(false);
    Paths p{G(), R + "/ffs", R + "/udc"};
    Gadget g(p, [] { return std::string("a800000.usb"); }, quiet);
    Controller c(&g, quiet, 20);
    c.start();
    // what init.a6l.usb.rc does (early-boot ids + sys.usb.ffs.ready=1 rule), absolute link target like init's symlink
    wr(G() + "/idVendor", "0x18d1");
    wr(G() + "/idProduct", "0x4ee7");
    CHECK(symlink((G() + "/functions/ffs.adb").c_str(), (G() + "/configs/b.1/f1").c_str()) == 0, "init link f1");
    wr(G() + "/UDC", "a800000.usb");
    wr(R + "/udc/a800000.usb/soft_connect", "init-connect");
    eps("adb", 2, true);
    CHECK(g.matches(compose(kAdb)), "init adb binding matches compose(adb)");
    CHECK(!g.matches(compose(kMtp | kAdb)), "init adb binding does not match mtp,adb");
    // usbd at boot: setCurrentUsbFunctions(ADB, timeout 0)
    auto r = c.setFunctions(kAdb, 0);
    CHECK(r == Controller::Result::kSuccess, "adb over an identical init binding -> SUCCESS even with timeout 0");
    CHECK(c.adoptCount() == 1 && c.bindCount() == 0, "adopted, no bind");
    CHECK(rd(G() + "/UDC") == "a800000.usb" && rd(R + "/udc/a800000.usb/soft_connect") == "init-connect",
          "adopt: UDC and pull-up untouched (no unbind, adbd keeps its session)");
    CHECK(c.applied() && c.currentFunctions() == kAdb, "adopt: applied");
    // the monitor owns it now: an adbd restart (endpoints gone + implicit unbind) is re-bound by the HAL
    eps("adb", 2, false);
    wr(G() + "/UDC", "");
    CHECK(waitFor([&] { return !c.applied(); }, 7000), "adopted gadget: unbind noticed");
    eps("adb", 2, true);
    CHECK(waitFor([&] { return g.bound() && c.applied() && c.bindCount() == 1; }, 3000), "adopted gadget re-bound by the HAL");
    CHECK(rd(R + "/udc/a800000.usb/soft_connect") == "connect", "HAL re-bind releases the pull-up hold");
    // a different composition is not adopted
    eps("mtp", 3, true);
    r = c.setFunctions(kMtp | kAdb, 2500);
    CHECK(r == Controller::Result::kSuccess && c.adoptCount() == 1 && c.bindCount() == 2, "mtp,adb: normal re-bind");
    // EACCES on an id attribute (r6b: root-owned configfs files, HAL = uid system)
    if (geteuid() != 0) {
        chmod((G() + "/idVendor").c_str(), 0444);
        r = c.setFunctions(kPtp | kAdb, 500);
        CHECK(r == Controller::Result::kApplyFailed, "unwritable gadget -> kApplyFailed (hand back to init)");
        CHECK(!g.bound() && !isLink(G() + "/configs/b.1/f1"), "kApplyFailed leaves the gadget unbound and unlinked");
        chmod((G() + "/idVendor").c_str(), 0644);
        // UDC itself not writable: unbind fails -> kApplyFailed too
        r = c.setFunctions(kAdb, 2500);
        CHECK(r == Controller::Result::kSuccess && g.bound(), "writable again -> adb bound");
        chmod((G() + "/UDC").c_str(), 0444);
        r = c.setFunctions(kMtp | kAdb, 500);
        CHECK(r == Controller::Result::kApplyFailed, "unbind impossible -> kApplyFailed");
        chmod((G() + "/UDC").c_str(), 0644);
    } else {
        fprintf(stderr, "note: running as root, EACCES cases skipped\n");
    }
    c.stop();
    std::string cmd = "rm -rf '" + R + "'";
    if (system(cmd.c_str()) != 0) fprintf(stderr, "cleanup failed\n");
}

int main() {
    testCompose();
    testFlows();
    testAdoptAndHandBack();
    printf("%s %d/%d\n", fails ? "A6L_USB_GADGET_TEST FAIL" : "A6L_USB_GADGET_TEST PASS", checks - fails, checks);
    return fails ? 1 : 0;
}
