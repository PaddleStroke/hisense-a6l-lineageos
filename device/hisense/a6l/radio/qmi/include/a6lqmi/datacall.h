// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): mobile data calls = WDA data format + WDS (one QMI client per IP
// family, bound to a QMAP mux id) + an rmnet link on top of the IPA netdev.
//
// DNS: Get Current Settings is asked with the DNS bit (qrild never did) and the IPv4 values are
// converted from QMI's le32 numeric form. Android gets them in SetupDataCallResult.dnses.
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/services.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace a6l::radio {

struct DataConfig {
    std::string parentIface = "rmnet_ipa0";  // mainline drivers/net/ipa netdev name
    std::string ifPrefix = "rmnet_data";
    uint32_t epType = qmi::wds::kEpEmbedded;
    uint32_t epIface = 1;
    uint8_t muxBase = 1;
    // IPA v2.6L (SDM660) modem endpoints have QMAP but no checksum offload (msm8953 v2.6L data):
    // ingress deaggregation only. Mainline IPA v3.1+ would use 0x0d (+MAPv4 checksum).
    uint32_t rmnetFlags = 0x01;
    int maxCalls = 4;
    bool requireNetdev = true;  // false: modem-only (CLI DNS test without IPA)
    bool setDataFormat = true;
    // data2 (25 Sep): open the IPA hardware data port with QMI DPM before WDA (stock SDM660 netmgr
    // qmi_dpm_enabled=1; ModemManager for "ipa"). Without it the phone answered WDA Set Data Format
    // with INVALID_OPERATION. Endpoints 0 = read <parent>/device/modem/{tx,rx}_endpoint_id
    // (ipa2-lite exports 4/5), falling back to 4/5 (SDM660 APPS_LAN_WAN_PROD / APPS_WAN_CONS).
    bool dpmOpenPort = true;
    uint32_t dpmRxEp = 0, dpmTxEp = 0;  // DPM names: rx = IPA consumer = AP TX (4); tx = AP RX (5)
    uint32_t wdaUlAgg = qmi::wda::kAggQmap, wdaDlAgg = qmi::wda::kAggQmap;
    uint32_t wdaDlMaxDatagrams = 32, wdaDlMaxSize = 32768;
    // ril3 (DSDS): subscription of the WDS clients (0 primary = default binding, nothing sent;
    // 1 secondary = WDS Bind Subscription before Bind Mux) and the first rmnet_data<N> index, so
    // slot 2's calls use their own mux ids and netdev names.
    uint8_t subscription = 0;
    int ifIndexBase = 0;
    // r5 review F14/F15 (28 Sep 2026): the HAL puts the modem's addresses on rmnet_data<N> (nobody else does on
    // Android) and a setup only succeeds when every prerequisite did. Get Current Settings is retried (bounded) when
    // it fails or lacks the family's address (the modem can publish them a little after START).
    bool assignAddresses = true;
    int settingsTries = 3;
    int settingsRetryMs = 300;
};

// r5 F15: link-layer operations (rtnetlink/ioctl, a6l::rmnet) behind an interface so setup failures are host-testable.
struct LinkOps {
    virtual ~LinkOps() = default;
    virtual bool exists(const std::string& name);
    virtual int createLink(const std::string& parent, const std::string& name, uint16_t muxId, uint32_t flags);
    virtual int deleteLink(const std::string& name);
    virtual int setUp(const std::string& name, bool up);
    virtual int setMtu(const std::string& name, int mtu);
    virtual int addAddress(const std::string& name, const std::string& cidr);
};

enum class Protocol { V4, V6, V4V6 };

struct DataRequest {
    std::string apn, user, password;
    uint8_t auth = 0;  // bit0 PAP, bit1 CHAP
    Protocol protocol = Protocol::V4V6;
};

struct DataCall {
    int cid = 0;  // Android cid (= mux id)
    uint8_t muxId = 0;
    std::string ifname;
    bool v4 = false, v6 = false;
    std::vector<std::string> addresses;  // "a.b.c.d/nn", "x::y/64"
    std::vector<std::string> gateways;
    std::vector<std::string> dnses;
    std::vector<std::string> pcscf;
    int mtuV4 = 0, mtuV6 = 0;
    std::string apn;
    // r5 review F28: connection generation. The cid (= mux id) is reused right after a teardown; delayed work
    // (network loss) carries (cid, generation) and never touches a newer connection with the same cid.
    uint64_t generation = 0;
};

struct SetupOutcome {
    bool ok = false;
    int failCause = 0;  // Android DataCallFailCause value (3GPP cause when known)
    std::string detail;
    DataCall call;
};

class DataCallManager {
  public:
    using ClientFactory = std::function<std::unique_ptr<qmi::Client>(const char* tag)>;
    using LostFn = std::function<void(int cid, uint64_t generation)>;

    DataCallManager(DataConfig cfg, ClientFactory factory, qmi::Client* control /*WDA*/);
    ~DataCallManager();

    SetupOutcome setup(const DataRequest& req);
    // ril3 (DSDS): DPM port + WDA data format once for the shared IPA port. Slot 2's manager has
    // setDataFormat=false and the HAL calls slot 1's prepareFormat() before a slot 2 call.
    bool prepareFormat();
    bool deactivate(int cid);  // explicit teardown (Android's cid = the current connection)
    // r5 F28: teardown for delayed work (loss indication): only if `cid` still is that connection generation.
    bool deactivateIfCurrent(int cid, uint64_t generation);
    void deactivateAll();
    std::vector<DataCall> list() const;
    // Called on a QMI dispatch thread when the network drops a call (WDS packet service status
    // DISCONNECTED) or a leg's WDS service/transport is gone (r5 F17/F16). The callback must NOT call
    // deactivate()/setup() synchronously (that would join the calling thread): post the work.
    void onLost(LostFn fn) { mLost = std::move(fn); }
    // r5 review round8 F59 (28 Sep 2026): the modem flagged a published call CONNECTED + "reconfiguration required"
    // (WDS Packet Service Status TLV 0x01 byte 2). Called on a QMI dispatch thread: post refresh(cid, generation) to a
    // worker (it blocks on QMI).
    void onReconfig(LostFn fn) { mReconfig = std::move(fn); }
    enum class Refresh {
        Gone,        // no such (cid, generation) any more: nothing to do
        Unchanged,   // settings re-read, nothing Android sees changed
        Updated,     // same addresses; DNS / gateway / P-CSCF / MTU replaced: *out = the call to publish again
        Invalidate,  // addresses changed or settings unusable: tear the call down (Android reconnects)
    };
    // Re-reads every leg's current settings (blocking QMI). Addresses cannot be swapped in place on rmnet_data<N>
    // (no address removal here), so an address change invalidates the call instead of keeping stale success.
    Refresh refresh(int cid, uint64_t generation, DataCall* out);
    // Modem restarted: every call is gone, the DPM port and data format must be sent again.
    void modemReset();
    // DPM Open Port (if enabled) + WDA Get/Set Data Format, once per modem boot. setup() calls it;
    // public for tests and the CLI. formatReport() = one line per step, for logs.
    bool prepareDataPath();
    std::string formatReport() const;

    static std::string settingsSummary(const qmi::wds::Settings& s);
    static void applySettings(const qmi::wds::Settings& s, bool v6, DataCall* out);
    static int failCauseFrom(const qmi::wds::StartResult& r, const qmi::Result& res);
    void setLinkOps(std::shared_ptr<LinkOps> ops) { mLink = std::move(ops); }  // host tests

  private:
    // r5 review round8 F59/F60: per-leg event latch shared with the leg's indication callbacks, which are installed
    // before START so an early DISCONNECTED / WDS loss / reconfiguration is never consumed unseen.
    struct LegState {
        std::atomic<bool> ended{false};      // DISCONNECTED or WDS gone (setup fails if seen before publication)
        std::atomic<bool> stopping{false};   // our own STOP: its DISCONNECTED is not a network loss
        std::atomic<bool> reconfig{false};   // CONNECTED + reconfiguration required
        std::atomic<bool> published{false};  // the call is in mCalls (reconfig -> mReconfig)
        std::atomic<int> endReason{0};       // verbose 3GPP cause of the DISCONNECTED, if any
    };
    struct Leg {  // one IP family
        std::unique_ptr<qmi::Client> client;
        uint32_t handle = 0;
        bool v6 = false;
        std::shared_ptr<LegState> state = std::make_shared<LegState>();
    };
    struct Entry {
        DataCall call;
        std::vector<Leg> legs;
    };
    bool ensureFormat();  // mLock held
    bool openDpmPort();   // mLock held
    int allocMux() const;
    bool startLeg(Entry& e, bool v6, const DataRequest& req, std::string* detail, int* cause);
    void stopLeg(Leg& leg);
    void stopEntry(Entry& e);
    static void dropFamily(Entry& e, bool v6);
    bool configureLink(Entry& e, std::string* why);  // mLock held

    DataConfig mCfg;
    ClientFactory mFactory;
    qmi::Client* mControl;
    mutable std::mutex mLock;
    std::map<int, Entry> mCalls;
    bool mFormatDone = false;
    std::unique_ptr<qmi::Client> mDpm;  // kept open: the modem may close the port with its client
    bool mDpmDone = false;
    std::vector<std::string> mReport;
    LostFn mLost;
    LostFn mReconfig;
    std::shared_ptr<LinkOps> mLink = std::make_shared<LinkOps>();
    uint64_t mNextGeneration = 0;
};

}  // namespace a6l::radio
