// Host test of libwifi-hal-a6l's lifecycle logic (driver-ready gate, init/event-loop/cleanup, interface enumeration)
// against a fake /sys/class/net and fake properties. nl80211/ethtool paths need a real cfg80211 phy: attended test.
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <thread>

#include <hardware_legacy/wifi_hal.h>

std::map<std::string, std::string> gProps;
extern "C" int property_get(const char* k, char* v, const char* d) {
    auto it = gProps.find(k);
    strcpy(v, it != gProps.end() ? it->second.c_str() : (d ? d : ""));
    return strlen(v);
}
extern "C" int32_t property_get_int32(const char* k, int32_t d) {
    auto it = gProps.find(k);
    return it != gProps.end() ? atoi(it->second.c_str()) : d;
}
extern "C" int __android_log_print(int, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "  [%s] ", tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    return 0;
}

static int fails = 0;
#define EXPECT(c, what)                                   \
    do {                                                  \
        if (c) printf("ok   %s\n", what);                 \
        else { printf("FAIL %s\n", what); fails++; }      \
    } while (0)

static std::atomic<int> gCleaned{0};
static wifi_handle gExpect;
static void onCleaned(wifi_handle h) {
    if (h == gExpect) gCleaned++;
}

int main() {
    const std::string root = A6L_SYSFS_NET;
    wifi_hal_fn fn;
    memset(&fn, 0, sizeof(fn));
    EXPECT(init_wifi_vendor_hal_func_table(&fn) == WIFI_SUCCESS, "func table");
    EXPECT(fn.wifi_initialize && fn.wifi_wait_for_driver_ready && fn.wifi_event_loop && fn.wifi_cleanup &&
                   fn.wifi_get_ifaces && fn.wifi_get_iface_name && fn.wifi_set_country_code,
           "lifecycle + country functions set");
    EXPECT(fn.wifi_get_link_stats == nullptr && fn.wifi_get_packet_filter_capabilities == nullptr &&
                   fn.wifi_virtual_interface_create == nullptr && fn.wifi_start_sending_offloaded_packet == nullptr,
           "no fake link-stats/APF/virtual-iface/keep-alive (left to NOT_SUPPORTED stubs)");

    // driver-ready gate
    gProps["ro.vendor.a6l.wifi.driver_wait_s"] = "1";
    EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_ERROR_UNKNOWN, "no wlan0 + radio off -> UNKNOWN (fatal, fast)");
    gProps["persist.vendor.a6l.radio"] = "1";
    auto t0 = std::chrono::steady_clock::now();
    EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_ERROR_TIMED_OUT, "no wlan0 + radio on -> TIMED_OUT");
    EXPECT(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(900), "  ... after the wait");
    gProps["vendor.a6l.wlan.state"] = "missing";
    t0 = std::chrono::steady_clock::now();
    EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_ERROR_TIMED_OUT &&
                   std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(300),
           "state=missing -> TIMED_OUT at once");
    mkdir((root + "/wlan0").c_str(), 0755);
    mkdir((root + "/wlan0/phy80211").c_str(), 0755);
    mkdir((root + "/rmnet_ipa0").c_str(), 0755);  // not wireless
    gProps["vendor.a6l.wlan.state"] = "";
    std::thread later([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        gProps["vendor.a6l.wlan.state"] = "ready";
    });
    t0 = std::chrono::steady_clock::now();
    wifi_error r = fn.wifi_wait_for_driver_ready();
    later.join();
    EXPECT(r == WIFI_SUCCESS && std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(350),
           "wlan0 present: waits for vendor.a6l.wlan.state=ready (MAC step) then SUCCESS");
    // r5 review fix F38: after the deadline a present wlan0 without the handshake is a failure (was fail-open)
    for (const char* st : {"", "starting", "stopped"}) {
        gProps["vendor.a6l.wlan.state"] = st;
        EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_ERROR_TIMED_OUT,
               (std::string("wlan0 present, state '") + st + "' never ready -> TIMED_OUT after wait (F38)").c_str());
    }
    gProps.erase("vendor.a6l.wlan.state");
    EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_ERROR_TIMED_OUT, "wlan0 present, state unset -> TIMED_OUT (F38)");
    gProps["vendor.a6l.wlan.state"] = "ready:mac-failed";
    EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_SUCCESS, "state ready:mac-failed (explicit MAC outcome) -> SUCCESS");
    gProps["vendor.a6l.wlan.state"] = "";
    gProps["ro.vendor.a6l.wifi.ungated"] = "1";
    EXPECT(fn.wifi_wait_for_driver_ready() == WIFI_SUCCESS, "opt-in ungated legacy fallback -> SUCCESS after wait");
    gProps.erase("ro.vendor.a6l.wifi.ungated");

    // start/stop cycles, as WifiLegacyHal::start()/stop() drive them
    for (int cycle = 0; cycle < 3; cycle++) {
        wifi_handle h = nullptr;
        EXPECT(fn.wifi_initialize(&h) == WIFI_SUCCESS && h != nullptr, "initialize");
        std::atomic<bool> loopDone{false};
        std::thread loop([&] { fn.wifi_event_loop(h); loopDone = true; });
        int n = 0;
        wifi_interface_handle* ifs = nullptr;
        EXPECT(fn.wifi_get_ifaces(h, &n, &ifs) == WIFI_SUCCESS && n == 1, "get_ifaces: only the cfg80211 netdev");
        char name[IFNAMSIZ] = {};
        EXPECT(n == 1 && fn.wifi_get_iface_name(ifs[0], name, sizeof(name)) == WIFI_SUCCESS &&
                       strcmp(name, "wlan0") == 0, "iface name wlan0");
        char tiny[3];
        EXPECT(n == 1 && fn.wifi_get_iface_name(ifs[0], tiny, sizeof(tiny)) == WIFI_ERROR_INVALID_ARGS,
               "short name buffer rejected");
        wifi_interface_handle first = n ? ifs[0] : nullptr;
        EXPECT(fn.wifi_get_ifaces(h, &n, &ifs) == WIFI_SUCCESS && n == 1 && ifs[0] == first, "handle stable");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        EXPECT(!loopDone, "event loop blocks while started");
        gExpect = h;
        int before = gCleaned;
        fn.wifi_cleanup(h, onCleaned);
        EXPECT(gCleaned == before + 1, "cleanup calls the handler synchronously with the handle");
        loop.join();
        EXPECT(loopDone, "event loop returns after cleanup");
    }
    // cleanup before the loop thread even started (detached thread scheduled late)
    {
        wifi_handle h = nullptr;
        fn.wifi_initialize(&h);
        gExpect = h;
        fn.wifi_cleanup(h, onCleaned);
        std::thread loop([&] { fn.wifi_event_loop(h); });
        loop.join();
        EXPECT(true, "late event loop returns at once");
    }
    EXPECT(fn.wifi_set_country_code(nullptr, "F") == WIFI_ERROR_INVALID_ARGS &&
                   fn.wifi_set_country_code(nullptr, "F$") == WIFI_ERROR_INVALID_ARGS,
           "country code validated before nl80211");
    printf(fails ? "A6L_WIFI_HAL_HOST_TEST FAIL %d\n" : "A6L_WIFI_HAL_HOST_TEST PASS\n", fails);
    return fails != 0;
}
