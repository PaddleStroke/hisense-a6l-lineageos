// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): mobile data calls = WDA data format + WDS (one QMI client per IP
// family, bound to a QMAP mux id) + an rmnet link on top of the IPA netdev.
//
// DNS: Get Current Settings is asked with the DNS bit (qrild never did) and the IPv4 values are
// converted from QMI's le32 numeric form. Android gets them in SetupDataCallResult.dnses.
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/services.h>

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
    using LostFn = std::function<void(int cid)>;

    DataCallManager(DataConfig cfg, ClientFactory factory, qmi::Client* control /*WDA*/);
    ~DataCallManager();

    SetupOutcome setup(const DataRequest& req);
    bool deactivate(int cid);
    void deactivateAll();
    std::vector<DataCall> list() const;
    // Called on a QMI dispatch thread when the network drops a call. The callback must NOT call
    // deactivate()/setup() synchronously (that would join the calling thread): post the work.
    void onLost(LostFn fn) { mLost = std::move(fn); }
    // Modem restarted: every call is gone, the data format must be sent again.
    void modemReset();

    static std::string settingsSummary(const qmi::wds::Settings& s);
    static void applySettings(const qmi::wds::Settings& s, bool v6, DataCall* out);
    static int failCauseFrom(const qmi::wds::StartResult& r, const qmi::Result& res);

  private:
    struct Leg {  // one IP family
        std::unique_ptr<qmi::Client> client;
        uint32_t handle = 0;
        bool v6 = false;
    };
    struct Entry {
        DataCall call;
        std::vector<Leg> legs;
    };
    bool ensureFormat();
    int allocMux() const;
    bool startLeg(Entry& e, bool v6, const DataRequest& req, std::string* detail, int* cause);
    void stopEntry(Entry& e);

    DataConfig mCfg;
    ClientFactory mFactory;
    qmi::Client* mControl;
    mutable std::mutex mLock;
    std::map<int, Entry> mCalls;
    bool mFormatDone = false;
    LostFn mLost;
};

}  // namespace a6l::radio
