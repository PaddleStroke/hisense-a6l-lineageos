/*
 * Hisense A6L legacy Wi-Fi vendor HAL for mainline ath10k_snoc (WCN3990) over plain nl80211/cfg80211.
 * r5 review fix F5 (agent wifi-hal, 29 Sep 2026). docs/wifi-hal-20260929.md.
 *
 * Loaded by the AOSP AIDL Wi-Fi HAL (hardware/interfaces/wifi/aidl/default) through its dynamic vendor-HAL path:
 * the linked libwifi-hal-fallback returns WIFI_ERROR_NOT_SUPPORTED, so WifiLegacyHalFactory reads
 * the XML descriptors in /vendor/etc/wifi/vendor_hals and dlopen()s this library.
 *
 * Scope, deliberately small and honest: HAL lifecycle (driver-ready wait, initialize, event loop, cleanup), interface
 * enumeration, feature set / valid channels / regulatory country from nl80211, driver + firmware versions from ethtool.
 * Everything else (gscan, link-layer stats, APF, RSSI monitor, keep-alive offload, roaming, logger rings, RTT, NAN,
 * virtual interface creation, ...) is left to the AIDL HAL's stubs, which return WIFI_ERROR_NOT_SUPPORTED. STA
 * scanning/association/roaming is done by wpa_supplicant + wificond over nl80211, AP by hostapd.
 *
 * Copyright (C) 2026 The LineageOS Project contributors. SPDX-License-Identifier: Apache-2.0
 */
#define LOG_TAG "WifiHAL-a6l"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/ethtool.h>
#include <linux/nl80211.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cutils/properties.h>
#include <log/log.h>
#include <netlink/attr.h>
#include <netlink/genl/ctrl.h>
#include <netlink/genl/genl.h>
#include <netlink/msg.h>

#include <hardware_legacy/wifi_hal.h>

#ifndef A6L_SYSFS_NET  // host tests (tests/host-test.sh) point this at a fake tree
#define A6L_SYSFS_NET "/sys/class/net"
#endif

namespace {

// ---------------------------------------------------------------------------------------------------------------
// state

struct A6lIface {
    std::string name;
};

// One per wifi_initialize(); the handle given to the AIDL HAL. Deleted by the event loop when it exits.
struct A6lSession {
    int stop_fd = -1;  // eventfd written by wifi_cleanup()
};

std::mutex gLock;
// Interface handles stay valid for the life of the process (never freed): the AIDL HAL caches them by name.
std::map<std::string, std::unique_ptr<A6lIface>> gIfaces;
std::vector<wifi_interface_handle> gHandles;  // handles of the wireless netdevs present at the last enumeration

A6lIface* asIface(wifi_interface_handle h) {
    return reinterpret_cast<A6lIface*>(h);
}

std::string primaryIfaceName() {
    char v[PROPERTY_VALUE_MAX];
    property_get("wifi.interface", v, "wlan0");
    return v[0] ? v : "wlan0";
}

bool isWirelessNetdev(const std::string& name) {
    struct stat st;
    return stat((std::string(A6L_SYSFS_NET "/") + name + "/phy80211").c_str(), &st) == 0;
}

// Re-reads /sys/class/net: every cfg80211 netdev (wlan0, and e.g. an AP/P2P netdev hostapd or the supplicant made).
void refreshIfacesLocked() {
    gHandles.clear();
    DIR* d = opendir(A6L_SYSFS_NET);
    if (d == nullptr) {
        ALOGE("opendir " A6L_SYSFS_NET ": %s", strerror(errno));
        return;
    }
    std::vector<std::string> names;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        if (isWirelessNetdev(e->d_name)) names.emplace_back(e->d_name);
    }
    closedir(d);
    for (const auto& n : names) {
        auto& slot = gIfaces[n];
        if (!slot) slot.reset(new A6lIface{n});
        gHandles.push_back(reinterpret_cast<wifi_interface_handle>(slot.get()));
    }
}

int wiphyIndex(const std::string& ifname) {
    int fd = open((std::string(A6L_SYSFS_NET "/") + ifname + "/phy80211/index").c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    char buf[16] = {};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    return atoi(buf);
}

// ---------------------------------------------------------------------------------------------------------------
// nl80211 (one short-lived generic netlink socket per request; nothing here is on a hot path)

class Nl80211 {
  public:
    Nl80211() {
        sk_ = nl_socket_alloc();
        if (sk_ == nullptr) return;
        if (genl_connect(sk_) != 0) {
            ALOGE("genl_connect failed");
            return;
        }
        struct timeval tv = {5, 0};  // never block the HAL forever on a wedged kernel
        setsockopt(nl_socket_get_fd(sk_), SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        id_ = genl_ctrl_resolve(sk_, "nl80211");
        if (id_ < 0) ALOGE("nl80211 family not found (%d)", id_);
    }
    ~Nl80211() {
        if (sk_ != nullptr) nl_socket_free(sk_);
    }
    bool ok() const { return sk_ != nullptr && id_ >= 0; }

    nl_msg* newMsg(uint8_t cmd, int flags) {
        nl_msg* msg = nlmsg_alloc();
        if (msg == nullptr) return nullptr;
        if (genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, id_, 0, flags, cmd, 0) == nullptr) {
            nlmsg_free(msg);
            return nullptr;
        }
        return msg;
    }

    // Sends and consumes the reply. Returns 0 or a negative errno. Takes ownership of msg.
    int transact(nl_msg* msg, int (*valid)(nl_msg*, void*), void* arg) {
        int err = 1;
        nl_cb* cb = nl_cb_alloc(NL_CB_DEFAULT);
        if (cb == nullptr) {
            nlmsg_free(msg);
            return -ENOMEM;
        }
        nl_cb_err(cb, NL_CB_CUSTOM, onError, &err);
        nl_cb_set(cb, NL_CB_FINISH, NL_CB_CUSTOM, onFinish, &err);
        nl_cb_set(cb, NL_CB_ACK, NL_CB_CUSTOM, onAck, &err);
        if (valid != nullptr) nl_cb_set(cb, NL_CB_VALID, NL_CB_CUSTOM, valid, arg);
        int rc = nl_send_auto(sk_, msg);
        nlmsg_free(msg);
        if (rc < 0) {
            nl_cb_put(cb);
            return -EIO;
        }
        while (err > 0) {
            rc = nl_recvmsgs(sk_, cb);
            if (rc < 0) {
                ALOGE("nl_recvmsgs: %d", rc);
                if (err > 0) err = -EIO;
                break;
            }
        }
        nl_cb_put(cb);
        return err;
    }

  private:
    static int onError(sockaddr_nl*, nlmsgerr* e, void* arg) {
        *static_cast<int*>(arg) = e->error;
        return NL_STOP;
    }
    static int onFinish(nl_msg*, void* arg) {
        *static_cast<int*>(arg) = 0;
        return NL_SKIP;
    }
    static int onAck(nl_msg*, void* arg) {
        *static_cast<int*>(arg) = 0;
        return NL_STOP;
    }

    nl_sock* sk_ = nullptr;
    int id_ = -1;
};

struct WiphyInfo {
    uint32_t phy = 0;
    uint32_t features = 0;           // NL80211_ATTR_FEATURE_FLAGS
    std::map<uint32_t, bool> freqs;  // enabled channel MHz -> needs DFS/radar detection
};

int onWiphy(nl_msg* msg, void* arg) {
    auto* info = static_cast<WiphyInfo*>(arg);
    auto* gnlh = static_cast<genlmsghdr*>(nlmsg_data(nlmsg_hdr(msg)));
    nlattr* tb[NL80211_ATTR_MAX + 1];
    nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0), genlmsg_attrlen(gnlh, 0), nullptr);
    if (tb[NL80211_ATTR_WIPHY] == nullptr || nla_get_u32(tb[NL80211_ATTR_WIPHY]) != info->phy) return NL_SKIP;
    if (tb[NL80211_ATTR_FEATURE_FLAGS] != nullptr) info->features |= nla_get_u32(tb[NL80211_ATTR_FEATURE_FLAGS]);
    if (tb[NL80211_ATTR_WIPHY_BANDS] == nullptr) return NL_SKIP;
    nlattr* band;
    int rem_band;
    nla_for_each_nested(band, tb[NL80211_ATTR_WIPHY_BANDS], rem_band) {
        nlattr* tbb[NL80211_BAND_ATTR_MAX + 1];
        nla_parse(tbb, NL80211_BAND_ATTR_MAX, static_cast<nlattr*>(nla_data(band)), nla_len(band), nullptr);
        if (tbb[NL80211_BAND_ATTR_FREQS] == nullptr) continue;
        nlattr* freq;
        int rem_freq;
        nla_for_each_nested(freq, tbb[NL80211_BAND_ATTR_FREQS], rem_freq) {
            nlattr* tf[NL80211_FREQUENCY_ATTR_MAX + 1];
            nla_parse(tf, NL80211_FREQUENCY_ATTR_MAX, static_cast<nlattr*>(nla_data(freq)), nla_len(freq),
                      nullptr);
            if (tf[NL80211_FREQUENCY_ATTR_FREQ] == nullptr) continue;
            if (tf[NL80211_FREQUENCY_ATTR_DISABLED] != nullptr) continue;
            info->freqs[nla_get_u32(tf[NL80211_FREQUENCY_ATTR_FREQ])] = tf[NL80211_FREQUENCY_ATTR_RADAR] != nullptr;
        }
    }
    return NL_SKIP;
}

// Current (regulatory-applied) capabilities of the wiphy behind ifname.
bool queryWiphy(const std::string& ifname, WiphyInfo* info) {
    int phy = wiphyIndex(ifname);
    if (phy < 0) {
        ALOGE("%s: no phy80211", ifname.c_str());
        return false;
    }
    Nl80211 nl;
    if (!nl.ok()) return false;
    nl_msg* msg = nl.newMsg(NL80211_CMD_GET_WIPHY, NLM_F_DUMP);
    if (msg == nullptr) return false;
    if (nla_put_u32(msg, NL80211_ATTR_WIPHY, phy) < 0 || nla_put_flag(msg, NL80211_ATTR_SPLIT_WIPHY_DUMP) < 0) {
        nlmsg_free(msg);
        return false;
    }
    info->phy = phy;
    int rc = nl.transact(msg, onWiphy, info);
    if (rc != 0) {
        ALOGE("GET_WIPHY phy%d: %d", phy, rc);
        return false;
    }
    return true;
}

bool isValidAlpha2(const char* cc) {
    if (cc == nullptr || strlen(cc) != 2) return false;
    for (int i = 0; i < 2; i++) {
        char c = cc[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// HAL functions

// wlan0 exists only once the modem's WLAN firmware service is up (a6l-radio.sh, gated by persist.vendor.a6l.radio),
// and a6l-radio.sh then sets the factory MAC from persist with the link DOWN: wait for its vendor.a6l.wlan.state=ready
// so the HAL never brings wlan0 up in the middle of that.
wifi_error a6l_wait_for_driver_ready() {
    const std::string ifname = primaryIfaceName();
    const int wait_s = property_get_int32("ro.vendor.a6l.wifi.driver_wait_s", 30);
    char radio[PROPERTY_VALUE_MAX];
    property_get("persist.vendor.a6l.radio", radio, "");
    for (int i = 0; i <= wait_s * 10; i++) {
        char state[PROPERTY_VALUE_MAX];
        property_get("vendor.a6l.wlan.state", state, "");
        const bool present = isWirelessNetdev(ifname);
        if (present && strncmp(state, "ready", 5) == 0) {
            ALOGI("driver ready: %s present, vendor.a6l.wlan.state=%s after %d ms", ifname.c_str(), state, i * 100);
            return WIFI_SUCCESS;
        }
        if (!present && strcmp(radio, "1") != 0) {
            ALOGE("%s absent and persist.vendor.a6l.radio!=1: the modem (and so WCN3990 Wi-Fi) is not started",
                  ifname.c_str());
            return WIFI_ERROR_UNKNOWN;
        }
        if (!present && strcmp(state, "missing") == 0) {
            ALOGE("a6l-radio.sh reported %s missing (modem WLAN service never came up)", ifname.c_str());
            return WIFI_ERROR_TIMED_OUT;
        }
        usleep(100 * 1000);
    }
    if (isWirelessNetdev(ifname)) {
        // r5 review fix F38: the deadline is a FAILURE, not success: a present wlan0 without the ready handshake means
        // a6l-radio.sh may still be before/inside its MAC step (link down + address change) and would disrupt a link
        // the HAL had started. Opt-in legacy fallback only (ro.vendor.a6l.wifi.ungated=1, e.g. a ROM without the
        // radio script), logged as such so it cannot be mistaken for completed initialization.
        if (property_get_int32("ro.vendor.a6l.wifi.ungated", 0) == 1) {
            ALOGW("%s present, vendor.a6l.wlan.state never ready in %d s: UNGATED legacy fallback (ro.vendor.a6l.wifi.ungated=1)",
                  ifname.c_str(), wait_s);
            return WIFI_SUCCESS;
        }
        ALOGE("%s present but vendor.a6l.wlan.state never became ready in %d s (MAC step unconfirmed): not starting",
              ifname.c_str(), wait_s);
        return WIFI_ERROR_TIMED_OUT;
    }
    ALOGE("%s did not appear within %d s", ifname.c_str(), wait_s);
    return WIFI_ERROR_TIMED_OUT;
}

wifi_error a6l_initialize(wifi_handle* handle) {
    if (handle == nullptr) return WIFI_ERROR_INVALID_ARGS;
    std::unique_ptr<A6lSession> s(new A6lSession());
    s->stop_fd = eventfd(0, EFD_CLOEXEC);
    if (s->stop_fd < 0) {
        ALOGE("eventfd: %s", strerror(errno));
        return WIFI_ERROR_UNKNOWN;
    }
    {
        std::lock_guard<std::mutex> l(gLock);
        refreshIfacesLocked();
        ALOGI("initialized, %zu wireless netdev(s)", gHandles.size());
    }
    *handle = reinterpret_cast<wifi_handle>(s.release());
    return WIFI_SUCCESS;
}

// Blocks until wifi_cleanup() for this session; no asynchronous vendor events exist in this HAL.
void a6l_event_loop(wifi_handle handle) {
    auto* s = reinterpret_cast<A6lSession*>(handle);
    if (s == nullptr) return;
    for (;;) {
        pollfd p = {s->stop_fd, POLLIN, 0};
        int rc = poll(&p, 1, -1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0) ALOGE("event loop poll: %s", strerror(errno));
        break;
    }
    close(s->stop_fd);
    delete s;
    ALOGI("event loop exited");
}

// Synchronous: the AIDL HAL holds its global (recursive) lock here and the stop callback takes it again on this
// thread. The event loop thread exits asynchronously and must not be waited for (it takes that same lock).
void a6l_cleanup(wifi_handle handle, wifi_cleaned_up_handler handler) {
    auto* s = reinterpret_cast<A6lSession*>(handle);
    if (s != nullptr) {
        uint64_t one = 1;
        if (write(s->stop_fd, &one, sizeof(one)) != sizeof(one)) ALOGE("stop eventfd: %s", strerror(errno));
        // s may be freed by the event loop from here on; only the handle value is passed on.
    }
    if (handler != nullptr) handler(handle);
}

wifi_error a6l_get_ifaces(wifi_handle handle, int* num, wifi_interface_handle** ifaces) {
    if (handle == nullptr || num == nullptr || ifaces == nullptr) return WIFI_ERROR_INVALID_ARGS;
    std::lock_guard<std::mutex> l(gLock);
    refreshIfacesLocked();
    *num = static_cast<int>(gHandles.size());
    *ifaces = gHandles.data();
    return WIFI_SUCCESS;
}

wifi_error a6l_get_iface_name(wifi_interface_handle h, char* name, size_t size) {
    if (h == nullptr || name == nullptr) return WIFI_ERROR_INVALID_ARGS;
    const std::string& n = asIface(h)->name;
    if (size < n.size() + 1) return WIFI_ERROR_INVALID_ARGS;
    strlcpy(name, n.c_str(), size);
    return WIFI_SUCCESS;
}

// Only what the chip/driver really does through this HAL. WPA3/OWE/etc. are reported by the supplicant, scan MAC
// randomization and P2P/AP by cfg80211 capabilities, and concurrency by the chip combination list (BoardConfig).
wifi_error a6l_get_supported_feature_set(wifi_interface_handle h, feature_set* set) {
    if (h == nullptr || set == nullptr) return WIFI_ERROR_INVALID_ARGS;
    // The AIDL HAL fails IWifiChip/IWifiStaIface.getFeatureSet on any error here, so an nl80211 failure degrades to
    // the one feature that is certain (infrastructure STA) instead of breaking Wi-Fi start.
    feature_set f = WIFI_FEATURE_INFRA;
    WiphyInfo info;
    if (!queryWiphy(asIface(h)->name, &info)) {
        ALOGW("%s: wiphy query failed, reporting INFRA only", asIface(h)->name.c_str());
        *set = f;
        return WIFI_SUCCESS;
    }
    for (const auto& [mhz, dfs] : info.freqs) {
        if (mhz >= 4900 && mhz < 5900) {
            f |= WIFI_FEATURE_INFRA_5G;
            break;
        }
    }
    if (info.features & NL80211_FEATURE_SCAN_RANDOM_MAC_ADDR) f |= WIFI_FEATURE_SCAN_RAND;
    *set = f;
    return WIFI_SUCCESS;
}

wifi_error a6l_get_valid_channels(wifi_interface_handle h, int band, int max_channels, wifi_channel* channels,
                                  int* num_channels) {
    if (h == nullptr || channels == nullptr || num_channels == nullptr || max_channels < 0)
        return WIFI_ERROR_INVALID_ARGS;
    WiphyInfo info;
    if (!queryWiphy(asIface(h)->name, &info)) return WIFI_ERROR_UNKNOWN;
    int n = 0;
    for (const auto& [mhz, dfs] : info.freqs) {
        bool want = false;
        if (mhz >= 2400 && mhz < 2500) want = band & WIFI_BAND_BG;
        else if (mhz >= 4900 && mhz < 5900) want = dfs ? (band & WIFI_BAND_A_DFS) : (band & WIFI_BAND_A);
        if (!want) continue;
        if (n >= max_channels) break;
        channels[n++] = static_cast<wifi_channel>(mhz);
    }
    *num_channels = n;
    return WIFI_SUCCESS;
}

// cfg80211 user regulatory hint (ath10k is not self-managed; the signed regulatory.db ships in /vendor/firmware).
wifi_error a6l_set_country_code(wifi_interface_handle /*h*/, const char* code) {
    if (!isValidAlpha2(code)) return WIFI_ERROR_INVALID_ARGS;
    char alpha2[3] = {static_cast<char>(toupper(code[0])), static_cast<char>(toupper(code[1])), 0};
    Nl80211 nl;
    if (!nl.ok()) return WIFI_ERROR_UNKNOWN;
    nl_msg* msg = nl.newMsg(NL80211_CMD_REQ_SET_REG, 0);
    if (msg == nullptr) return WIFI_ERROR_OUT_OF_MEMORY;
    if (nla_put_string(msg, NL80211_ATTR_REG_ALPHA2, alpha2) < 0) {
        nlmsg_free(msg);
        return WIFI_ERROR_UNKNOWN;
    }
    int rc = nl.transact(msg, nullptr, nullptr);
    if (rc != 0) {
        ALOGE("REQ_SET_REG %s: %d", alpha2, rc);
        return WIFI_ERROR_UNKNOWN;
    }
    ALOGI("regulatory hint %s sent", alpha2);
    return WIFI_SUCCESS;
}

bool drvinfo(const std::string& ifname, ethtool_drvinfo* info) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    memset(info, 0, sizeof(*info));
    info->cmd = ETHTOOL_GDRVINFO;
    strlcpy(ifr.ifr_name, ifname.c_str(), sizeof(ifr.ifr_name));
    ifr.ifr_data = reinterpret_cast<char*>(info);
    int rc = ioctl(fd, SIOCETHTOOL, &ifr);
    close(fd);
    return rc == 0;
}

wifi_error copyVersion(const char* s, char* buffer, int size) {
    if (buffer == nullptr || size <= 0) return WIFI_ERROR_INVALID_ARGS;
    strlcpy(buffer, s, size);
    return WIFI_SUCCESS;
}

wifi_error a6l_get_firmware_version(wifi_interface_handle h, char* buffer, int size) {
    if (h == nullptr) return WIFI_ERROR_INVALID_ARGS;
    ethtool_drvinfo info;
    if (!drvinfo(asIface(h)->name, &info)) return WIFI_ERROR_UNKNOWN;
    return copyVersion(info.fw_version, buffer, size);
}

wifi_error a6l_get_driver_version(wifi_interface_handle h, char* buffer, int size) {
    if (h == nullptr) return WIFI_ERROR_INVALID_ARGS;
    ethtool_drvinfo info;
    if (!drvinfo(asIface(h)->name, &info)) return WIFI_ERROR_UNKNOWN;
    std::string v = std::string(info.driver) + " " + info.version;
    return copyVersion(v.c_str(), buffer, size);
}

}  // namespace

extern "C" wifi_error init_wifi_vendor_hal_func_table(wifi_hal_fn* fn) {
    if (fn == nullptr) return WIFI_ERROR_INVALID_ARGS;
    // The AIDL HAL pre-fills fn with NOT_SUPPORTED stubs; only real implementations are overridden here.
    fn->wifi_wait_for_driver_ready = a6l_wait_for_driver_ready;
    fn->wifi_initialize = a6l_initialize;
    fn->wifi_event_loop = a6l_event_loop;
    fn->wifi_cleanup = a6l_cleanup;
    fn->wifi_get_ifaces = a6l_get_ifaces;
    fn->wifi_get_iface_name = a6l_get_iface_name;
    fn->wifi_get_supported_feature_set = a6l_get_supported_feature_set;
    fn->wifi_get_valid_channels = a6l_get_valid_channels;
    fn->wifi_set_country_code = a6l_set_country_code;
    fn->wifi_get_firmware_version = a6l_get_firmware_version;
    fn->wifi_get_driver_version = a6l_get_driver_version;
    return WIFI_SUCCESS;
}
