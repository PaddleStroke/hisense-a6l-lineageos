// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): IMSDCM (AP-hosted, 770) + IMSA (33) codecs.
// Layouts: see a6lqmi/ims.h (decoded from the stock libqmiservices.so IDL tables).
#include <a6lqmi/ims.h>

#include <arpa/inet.h>

#include <cstring>
#include <ctime>

namespace a6l::qmi {

static void put32(std::vector<uint8_t>& o, uint32_t v) {
    for (int i = 0; i < 4; i++) o.push_back((v >> (8 * i)) & 0xff);
}
static Message withResult(MsgType t, uint16_t id, uint16_t err) {
    Message m(t, id);
    m.raw(kTlvResult, {static_cast<uint8_t>(err ? 1 : 0), 0, static_cast<uint8_t>(err & 0xff),
                       static_cast<uint8_t>(err >> 8)});
    return m;
}

namespace imsdcm {

const char* msgName(uint16_t id) {
    switch (id) {
        case kPdpActivate: return "PDP_ACTIVATE";
        case kPdpDeactivate: return "PDP_DEACTIVATE";
        case kGetIpAddress: return "GET_IP_ADDRESS";
        case kLinkAddr: return "LINK_ADDR";
        case kAddressChangeInd: return "ADDRESS_CHANGE";
        // volte5: names from the stock imsdatadaemon handler table (0x4f560) + its log strings
        case 0x0025: return "WIFI_QUALITY";
        case 0x0026: return "STOP_WIFI_QUALITY";
        case 0x0027: return "UPDATE_WIFI_QUALITY";
        case 0x0028: return "HO_MEASUREMENT_INIT";
        case 0x0029: return "HO_MEASUREMENT_START";
        case 0x002A: return "HO_MEASUREMENT_STOP";
        case 0x002B: return "HO_MEASUREMENT_REPORT_IND";
        case 0x002C: return "HO_MEASUREMENT_STATUS";
        case 0x002D: return "HO_MEASUREMENT_UNINIT";
        case kAppStateReq: return "REGISTER_APP_STATE";
        case kAppStateInd: return "APP_STATE_IND";
        case kModemLlInd: return "MODEM_LL_IND";
        case 0x0031: return "UNKNOWN_0x31";
        case kWlanTz: return "WLAN_TZ";
        case kSubDestroyInstance: return "SUB_DESTROY_INSTANCE";
        case kServiceEnableStatus: return "SERVICE_ENABLE_STATUS";
        default: return "UNKNOWN";
    }
}
const char* instanceName(uint32_t inst) {
    switch (inst) {
        case kInstanceGlobal: return "GLOBAL";
        case kInstanceId1: return "ID1/sub0";
        case kInstanceId2: return "ID2/sub1";
        case kInstanceNone: return "NONE";
    }
    return "?";
}

const char* apnTypeName(uint32_t t) {
    static const char* n[] = {"ims", "internet", "emergency", "rcs", "ut", "wlan"};
    return t < 6 ? n[t] : "?";
}
const char* ratName(uint32_t r) {
    static const char* n[] = {"ehrpd", "lte", "epc", "wlan"};
    return r < 4 ? n[r] : "?";
}

std::optional<PdpActivateReq> parsePdpActivate(const Message& m) {
    auto* v = m.get(0x01);
    if (!v) return std::nullopt;
    Reader r(*v);
    PdpActivateReq q;
    uint8_t n = r.u8();
    if (n > kMaxApn) return std::nullopt;
    q.apn = r.str(n);
    q.apnType = r.u32();
    q.rat = r.u32();
    q.family = r.u32();
    q.profile = r.u32();
    if (!r.good()) return std::nullopt;
    auto opt = [&](uint8_t t) -> std::optional<uint32_t> {
        auto* x = m.get(t);
        if (!x || x->size() < 4) return std::nullopt;
        return Reader(*x).u32();
    };
    q.seq = opt(0x10);
    q.subscription = opt(0x11);
    q.slot = opt(0x12);
    q.instance = opt(0x13);
    return q;
}

Message buildPdpActivateReq(const PdpActivateReq& q) {
    Message m = Message::request(kPdpActivate);
    std::vector<uint8_t> v;
    v.push_back(static_cast<uint8_t>(q.apn.size()));
    v.insert(v.end(), q.apn.begin(), q.apn.end());
    for (uint32_t x : {q.apnType, q.rat, q.family, q.profile}) put32(v, x);
    m.raw(0x01, v);
    if (q.seq) m.u32(0x10, *q.seq);
    if (q.subscription) m.u32(0x11, *q.subscription);
    if (q.slot) m.u32(0x12, *q.slot);
    if (q.instance) m.u32(0x13, *q.instance);
    return m;
}

Message buildPdpActivateResp(uint16_t err, uint8_t pdpId, std::optional<uint32_t> seq,
                             std::optional<uint32_t> instance) {
    Message m = withResult(MsgType::Response, kPdpActivate, err);
    if (pdpId) m.u8(0x10, pdpId);
    if (seq) m.u32(0x11, *seq);
    if (instance) m.u32(0x12, *instance);
    return m;
}

std::vector<uint8_t> encodeAddress(uint32_t family, const std::string& text, bool asText) {
    std::vector<uint8_t> o;
    put32(o, family);
    std::vector<uint8_t> a;
    if (asText) {
        a.assign(text.begin(), text.end());
    } else {
        uint8_t buf[16];
        if (family == kFamilyV6 && inet_pton(AF_INET6, text.c_str(), buf) == 1)
            a.assign(buf, buf + 16);
        else if (family == kFamilyV4 && inet_pton(AF_INET, text.c_str(), buf) == 1)
            a.assign(buf, buf + 4);
    }
    if (a.size() > kMaxAddr) a.resize(kMaxAddr);
    o.push_back(static_cast<uint8_t>(a.size()));
    o.insert(o.end(), a.begin(), a.end());
    return o;
}

Message buildPdpActivateInd(uint16_t err, uint8_t pdpId, std::optional<uint32_t> seq,
                            std::optional<std::vector<uint8_t>> addr, std::optional<uint32_t> instance) {
    Message m = withResult(MsgType::Indication, kPdpActivate, err);
    m.u8(0x01, pdpId);
    if (seq) m.u32(0x10, *seq);
    if (addr) m.raw(0x11, *addr);
    if (instance) m.u32(0x12, *instance);
    return m;
}

Message buildAddressInd(uint16_t msgId, uint8_t pdpId, const std::vector<uint8_t>& addr,
                        std::optional<uint32_t> instance) {
    Message m(MsgType::Indication, msgId);
    m.u8(0x01, pdpId);
    m.raw(0x10, addr);
    if (instance) m.u32(0x11, *instance);
    return m;
}

Message buildWlanTzResp(uint8_t pdpId, uint32_t seq, int64_t utc) {
    Message m = withResult(MsgType::Response, kWlanTz, 0);
    m.u8(0x03, pdpId);
    m.u32(0x04, seq);
    time_t t = static_cast<time_t>(utc);
    struct tm tmv {};
    localtime_r(&t, &tmv);
    std::vector<uint8_t> v;
    auto p16 = [&](int x) {
        v.push_back(static_cast<uint8_t>(x & 0xff));
        v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
    };
    p16(tmv.tm_sec);
    p16(tmv.tm_min);
    p16(tmv.tm_hour);
    p16(tmv.tm_mday);
    p16(tmv.tm_mon + 1);
    p16(tmv.tm_year + 1900);
    p16(tmv.tm_wday);
    p16(static_cast<int>(tmv.tm_gmtoff / 900));
    for (int i = 0; i < 8; i++) v.push_back(static_cast<uint8_t>((static_cast<uint64_t>(utc) >> (8 * i)) & 0xff));
    m.raw(0x05, v);
    return m;
}

Message buildSimpleResp(uint16_t msgId, uint16_t err, std::optional<uint8_t> pdpId,
                        std::optional<uint32_t> instance) {
    Message m = withResult(MsgType::Response, msgId, err);
    if (pdpId) m.u8(0x10, *pdpId);
    if (instance) m.u32(0x11, *instance);
    return m;
}
}  // namespace imsdcm

namespace imsa {

const char* regStatusName(uint32_t s) {
    static const char* n[] = {"not-registered", "registering", "registered", "limited-registered"};
    return s < 4 ? n[s] : "?";
}
const char* serviceStatusName(uint32_t s) {
    static const char* n[] = {"unavailable", "limited", "available"};
    return s < 3 ? n[s] : "?";
}
const char* techName(uint32_t t) {
    static const char* n[] = {"wlan", "wwan", "iwlan"};
    return t < 3 ? n[t] : "?";
}

static std::optional<uint32_t> tu32(const Message& m, uint8_t t) {
    auto* v = m.get(t);
    if (!v || v->size() < 4) return std::nullopt;
    return Reader(*v).u32();
}

RegStatus parseRegStatus(const Message& m) {
    RegStatus s;
    bool ind = m.type == MsgType::Indication;
    uint8_t tFlag = ind ? 0x01 : 0x10, tErr = ind ? 0x10 : 0x11, tSt = ind ? 0x11 : 0x12,
            tTxt = ind ? 0x12 : 0x13, tTech = ind ? 0x13 : 0x14;
    if (auto* v = m.get(tFlag); v && !v->empty()) s.registeredFlag = (*v)[0] != 0;
    if (auto* v = m.get(tErr); v && v->size() >= 2) s.errorCode = Reader(*v).u16();
    s.status = tu32(m, tSt);
    if (auto* v = m.get(tTxt); v && !v->empty()) {
        // Top-level string TLV: its length is the TLV length, with no inner prefix.
        s.errorText.assign(v->begin(), v->end());
    }
    s.tech = tu32(m, tTech);
    return s;
}

std::string RegStatus::summary() const {
    std::string o = "status=";
    o += status ? regStatusName(*status) : (registeredFlag ? (*registeredFlag ? "registered(flag)" : "not-registered(flag)") : "-");
    if (tech) o += std::string(" tech=") + techName(*tech);
    if (errorCode) o += " err=" + std::to_string(*errorCode);
    if (!errorText.empty()) o += " text='" + errorText + "'";
    return o;
}

ServicesStatus parseServicesStatus(const Message& m) {
    ServicesStatus s;
    s.sms = tu32(m, 0x10);
    s.voice = tu32(m, 0x11);
    s.vt = tu32(m, 0x12);
    s.smsTech = tu32(m, 0x13);
    s.voiceTech = tu32(m, 0x14);
    s.vtTech = tu32(m, 0x15);
    s.ut = tu32(m, 0x16);
    s.utTech = tu32(m, 0x17);
    return s;
}

std::string ServicesStatus::summary() const {
    auto f = [](const char* n, const std::optional<uint32_t>& st, const std::optional<uint32_t>& t) {
        std::string o = std::string(n) + "=" + (st ? serviceStatusName(*st) : "-");
        if (t) o += std::string("/") + techName(*t);
        return o;
    };
    return f("voice", voice, voiceTech) + " " + f("sms", sms, smsTech) + " " + f("vt", vt, vtTech) +
           " " + f("ut", ut, utTech);
}

Result getRegStatus(Client& c, RegStatus* out) {
    auto r = c.request(kSvcImsa, Message::request(kGetRegStatus));
    if (r.ok()) *out = parseRegStatus(r.msg);
    return r;
}
Result getServicesStatus(Client& c, ServicesStatus* out) {
    auto r = c.request(kSvcImsa, Message::request(kGetServicesStatus));
    if (r.ok()) *out = parseServicesStatus(r.msg);
    return r;
}
Result registerIndications(Client& c, bool reg, bool services) {
    Message m = Message::request(kRegisterIndications);
    m.u8(0x10, reg ? 1 : 0);
    m.u8(0x11, services ? 1 : 0);
    return c.request(kSvcImsa, m);
}
Message buildBindSubscription(uint32_t sub) {
    Message m = Message::request(kBindSubscription);
    m.u32(0x10, sub);
    return m;
}
Result bindSubscription(Client& c, uint32_t sub) { return c.request(kSvcImsa, buildBindSubscription(sub)); }
}  // namespace imsa

namespace imss {
static std::optional<uint8_t> tu8(const Message& m, uint8_t t) {
    auto* v = m.get(t);
    if (!v || v->empty()) return std::nullopt;
    return (*v)[0];
}
ServiceEnableConfig parseGetServiceEnableConfig(const Message& m) {
    ServiceEnableConfig s;
    s.settingsResp = tu8(m, 0x10);
    s.volte = tu8(m, 0x11);
    s.vt = tu8(m, 0x12);
    s.wifi = tu8(m, 0x15);
    s.imsService = tu8(m, 0x19);
    return s;
}
ServiceEnableConfig parseServiceEnableConfigInd(const Message& m) {
    ServiceEnableConfig s;
    s.volte = tu8(m, 0x10);
    s.vt = tu8(m, 0x11);
    s.wifi = tu8(m, 0x14);
    s.imsService = tu8(m, 0x18);
    return s;
}
std::string ServiceEnableConfig::summary() const {
    auto f = [](const char* n, const std::optional<uint8_t>& v) {
        return std::string(n) + "=" + (v ? std::to_string(*v) : std::string("-"));
    };
    std::string o = f("ims_service_enabled", imsService) + " " + f("volte", volte) + " " + f("vt", vt) + " " +
                    f("wifi", wifi);
    if (settingsResp) o += " settings_resp=" + std::to_string(*settingsResp);
    return o;
}
Message buildBindSubscription(uint32_t sub) {
    Message m = Message::request(kBindSubscription);
    m.u32(0x01, sub);
    return m;
}
Message buildSetServiceEnableConfig(std::optional<bool> volte, std::optional<bool> imsService) {
    Message m = Message::request(kSetServiceEnableConfig);
    if (volte) m.u8(0x10, *volte ? 1 : 0);
    if (imsService) m.u8(0x18, *imsService ? 1 : 0);
    return m;
}
Result bindSubscription(Client& c, uint32_t sub) { return c.request(kSvcImss, buildBindSubscription(sub)); }
Result getServiceEnableConfig(Client& c, ServiceEnableConfig* out) {
    auto r = c.request(kSvcImss, Message::request(kGetServiceEnableConfig));
    if (r.ok()) *out = parseGetServiceEnableConfig(r.msg);
    return r;
}
Result setServiceEnableConfig(Client& c, std::optional<bool> volte, std::optional<bool> imsService) {
    return c.request(kSvcImss, buildSetServiceEnableConfig(volte, imsService));
}
}  // namespace imss

}  // namespace a6l::qmi
