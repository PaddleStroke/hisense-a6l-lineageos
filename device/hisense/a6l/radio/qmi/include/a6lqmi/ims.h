// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent volte2, 25 Sep 2026): IMS QMI services for modem-centric VoLTE (plan B).
//
// * IMSDCM (QMI service 770 = 0x302, IDL major 1 / minor 15) is hosted by the AP: on stock the
//   vendor imsdatadaemon is its QCSI server and the modem IMS stack (QIMF) is the client that asks
//   for the IMS / emergency PDN. Message ids and TLV layouts below were decoded from the stock
//   vendor/lib64/libqmiservices.so IDL tables (tools/volte/idldump.py) and match libqmi 1.38
//   data/qmi-service-imsdcm.json where libqmi has them (0x20/0x21). Names of the other ids come
//   from imsdatadaemon's log strings matched by TLV shape (marked "inferred").
// * IMSA (33) is a modem service (IMS application): registration + service status. TLVs from
//   the same IDL tables (resp 0x20: 0x10 bool, 0x11 err u16, 0x12 status u32, 0x13 string,
//   0x14 tech; ind 0x23: 0x01 bool, 0x10 err, 0x11 status, 0x12 string, 0x13 tech).
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/message.h>

#include <cstdint>
#include <optional>
#include <string>

namespace a6l::qmi {

namespace imsdcm {
constexpr uint32_t kService = 0x302;  // 770
constexpr uint32_t kIdlMajor = 1;     // QRTR NEW_SERVER instance word = major | (instance << 8)

enum : uint16_t {
    kPdpActivate = 0x0020,       // req {0x01 conn params, 0x10 seq, 0x11 sub, 0x12 slot, 0x13 inst}; ind
    kPdpDeactivate = 0x0021,     // req {0x01 pdp id, 0x10 inst}
    kGetIpAddress = 0x0022,      // req {0x01 pdp id, 0x10 inst}; ind {0x01 pdp, 0x10 addr, 0x11 inst} (inferred)
    kLinkAddr = 0x0023,          // req {0x01 {u16 port, u32 family, u8 len + addr}} (inferred)
    kAddressChangeInd = 0x0024,  // ind {0x01 pdp, 0x10 addr, 0x11 inst} (inferred)
    // volte5 (28 Sep): 0x2e..0x34 confirmed from the stock imsdatadaemon (handler table at 0x4f560,
    // QMI_IMS_DCM_*_V01 log strings, disassembly of each handler):
    //  0x2e REGISTER_APP_STATE {0x10 u8, 0x11 u32 instance}: stock stores the client, sets
    //       vendor.ims.ENABLE_HELPER and answers plain success (no 0x10 in the response).
    //  0x2f APP_STATE_IND {0x10 u32, 0x11 u32}: AP -> modem, only "BT_CRITERIA_MET/NOT_MET" (Wi-Fi/BT).
    //  0x30 MODEM_LL_IND {0x10 u8 logging_event_supported, 0x11 u32 instance}: AP -> modem, only when
    //       an AP IMS client asks datad for the modem link-local address (DATAD_MODEMLL).
    //  0x32 WLAN_TZ {0x01 u8 pdp, 0x02 u32 seq}: resp 0x03 pdp, 0x04 seq, 0x05 {sec,min,hour,mday,
    //       mon(1-12),year,wday,tz in 15 min (u16 each), u64 utc seconds}.
    //  0x33 SUB_DESTROY_INSTANCE {0x01 u32 instance}: stock only answers success.
    //  0x34 SERVICE_ENABLE_STATUS {0x01 u64 RCS service type mask}: stock answers success and
    //       restarts its RCS daemon when mask & 3. Not a VoLTE switch.
    // None of them gates the IMS PDN request: stock sends nothing unsolicited to the modem.
    kAppStateReq = 0x002E,
    kAppStateInd = 0x002F,
    kModemLlInd = 0x0030,
    kWlanTz = 0x0032,
    kSubDestroyInstance = 0x0033,
    kServiceEnableStatus = 0x0034,
};
// QmiImsDcmApnType / RatType / IpFamily / InstanceId (libqmi 1.38 qmi-enums-imsdcm.h)
enum : uint32_t { kApnIms = 0, kApnInternet = 1, kApnEmergency = 2, kApnRcs = 3, kApnUt = 4, kApnWlan = 5 };
enum : uint32_t { kRatEhrpd = 0, kRatLte = 1, kRatEpc = 2, kRatWlan = 3 };
enum : uint32_t { kFamilyV4 = 0, kFamilyV6 = 1 };
constexpr size_t kMaxApn = 100, kMaxAddr = 40;

const char* msgName(uint16_t id);
// volte6 (29 Sep): the u32 "instance" in 0x2e/0x33 and PDP_ACTIVATE 0x13 is QmiImsDcmInstanceId (libqmi
// 1.37 qmi-enums-imsdcm.h): 1 = GLOBAL, 2 = ID1 (subscription 0), 3 = ID2 (subscription 1), 0xff = none.
// 29 Sep attended: 0x33 SUB_DESTROY_INSTANCE instance=1 = the modem tore down its GLOBAL IMS instance.
enum : uint32_t { kInstanceGlobal = 1, kInstanceId1 = 2, kInstanceId2 = 3, kInstanceNone = 0xff };
const char* instanceName(uint32_t inst);
const char* apnTypeName(uint32_t t);
const char* ratName(uint32_t r);

struct PdpActivateReq {
    std::string apn;
    uint32_t apnType = 0, rat = 0, family = 0, profile = 0;
    std::optional<uint32_t> seq, subscription, slot, instance;
};
// nullopt = mandatory TLV 0x01 missing or malformed (answer MISSING_ARG / MALFORMED).
std::optional<PdpActivateReq> parsePdpActivate(const Message& m);
Message buildPdpActivateReq(const PdpActivateReq& r);  // for tests / the fake modem

// Response to PDP Activate: result + 0x10 pdp id + 0x11 seq + 0x12 instance
Message buildPdpActivateResp(uint16_t err, uint8_t pdpId, std::optional<uint32_t> seq,
                             std::optional<uint32_t> instance);
// Address struct {u32 family, u8 len, bytes}. text=true: "10.1.2.3" / "2a01::1" (stock log prints
// it with %s); text=false: 4/16 raw bytes (fallback A6L_IMSDCM_ADDR=bin).
std::vector<uint8_t> encodeAddress(uint32_t family, const std::string& text, bool asText);
// PDP Activate indication: 0x02 result (err 0 = success), 0x01 pdp id, 0x10 seq, 0x11 addr, 0x12 inst
Message buildPdpActivateInd(uint16_t err, uint8_t pdpId, std::optional<uint32_t> seq,
                            std::optional<std::vector<uint8_t>> addr, std::optional<uint32_t> instance);
// GET_IP_ADDRESS / ADDRESS_CHANGE indication: 0x01 pdp, 0x10 addr, 0x11 instance
Message buildAddressInd(uint16_t msgId, uint8_t pdpId, const std::vector<uint8_t>& addr,
                        std::optional<uint32_t> instance);
// WLAN_TZ response (0x32): 0x03 pdp, 0x04 seq, 0x05 {8 x u16 local time, u64 utc} (stock layout)
Message buildWlanTzResp(uint8_t pdpId, uint32_t seq, int64_t utcSeconds);
// Generic response: result + optional 0x10 u8 pdp id (+0x11 u32 instance where the IDL has it)
Message buildSimpleResp(uint16_t msgId, uint16_t err, std::optional<uint8_t> pdpId = std::nullopt,
                        std::optional<uint32_t> instance = std::nullopt);
}  // namespace imsdcm

namespace imsa {
enum : uint16_t {
    kGetRegStatus = 0x0020,
    kGetServicesStatus = 0x0021,
    kRegisterIndications = 0x0022,
    kRegStatusInd = 0x0023,
    kServicesStatusInd = 0x0024,
    // volte5 (28 Sep): stock qcril ImsaModemEndPointModule::handleQmiBinding sends 0x0033 {0x10 u32 sub}
    // (0 primary, 1 secondary, 2 tertiary) right after the client is up. Without it every IMSA
    // request answers INVALID_OPERATION on this DSDS modem (seen 28 Sep, err 70).
    kBindSubscription = 0x0033,
};
enum : uint32_t { kNotRegistered = 0, kRegistering = 1, kRegistered = 2, kLimitedRegistered = 3 };
enum : uint32_t { kSvcUnavailable = 0, kSvcLimited = 1, kSvcAvailable = 2 };
enum : uint32_t { kTechWlan = 0, kTechWwan = 1, kTechIwlan = 2 };

struct RegStatus {
    std::optional<bool> registeredFlag;  // resp 0x10 / ind 0x01 (older boolean)
    std::optional<uint32_t> status;      // resp 0x12 / ind 0x11
    std::optional<uint16_t> errorCode;   // resp 0x11 / ind 0x10 (SIP code on failure)
    std::string errorText;               // resp 0x13 / ind 0x12
    std::optional<uint32_t> tech;        // resp 0x14 / ind 0x13
    bool registered() const {
        return status ? (*status == kRegistered) : (registeredFlag && *registeredFlag);
    }
    std::string summary() const;
};
struct ServicesStatus {
    std::optional<uint32_t> sms, voice, vt, smsTech, voiceTech, vtTech, ut, utTech;
    bool voiceAvailable() const { return voice && *voice == kSvcAvailable; }
    std::string summary() const;
};
RegStatus parseRegStatus(const Message& m);  // response 0x20 or indication 0x23 (by type/msgId)
ServicesStatus parseServicesStatus(const Message& m);  // resp 0x21 / ind 0x24 (same TLV ids)
Result getRegStatus(Client& c, RegStatus* out);
Result getServicesStatus(Client& c, ServicesStatus* out);
Result registerIndications(Client& c, bool reg = true, bool services = true);
Message buildBindSubscription(uint32_t sub);  // 0x0033 {0x10 u32}
Result bindSubscription(Client& c, uint32_t sub = 0);
const char* regStatusName(uint32_t s);
const char* serviceStatusName(uint32_t s);
const char* techName(uint32_t t);
}  // namespace imsa

// volte5 (28 Sep): IMSS = IMS settings (QMI 18, IDL 1.70), a modem service. What the stock qcril
// (vendor/lib64/libril-qc-hal-qmi.so) sends, decoded by disassembly + the IDL tables:
//  * ImssModemEndPointModule::handleQmiBinding: 0x0098 {0x01 u32 sub} (0 primary). Unbound clients
//    get INVALID_OPERATION (28 Sep: IMSS "services enabled" query err 70).
//  * qcril_qmi_imss_request_set_ims_registration_v02 (Android ImsService turnOnIms/turnOffIms) ->
//    SET_IMS_SERVICE_ENABLE_CONFIG 0x008F with TLV 0x18 ims_service_enabled (req struct offset 48).
//    qcril_qmi_imss_request_set_ims_srv_status_v02 sets 0x10 (volte), 0x11 (vt), 0x14 (wifi calling).
//  * GET_IMS_SERVICE_ENABLE_CONFIG 0x0090 (empty) -> resp 0x10 enum8 settings_resp, then the req
//    fields shifted by one TLV id: 0x11 volte, 0x12 vt, 0x15 wifi, 0x19 ims_service_enabled
//    (qcril_qmi_imss_store_get_ims_service_enable_resp reads "IMS has_state" at offset 0x3f/0x40).
//  * IND 0x0091 carries the req layout (0x10 volte ... 0x18 ims_service_enabled).
namespace imss {
enum : uint16_t {
    kSetServiceEnableConfig = 0x008F,
    kGetServiceEnableConfig = 0x0090,
    kServiceEnableConfigInd = 0x0091,
    kBindSubscription = 0x0098,
};
struct ServiceEnableConfig {
    std::optional<uint8_t> settingsResp, volte, vt, wifi, imsService;
    std::string summary() const;
};
ServiceEnableConfig parseGetServiceEnableConfig(const Message& resp);  // 0x0090 response
ServiceEnableConfig parseServiceEnableConfigInd(const Message& ind);   // 0x0091 indication
Message buildBindSubscription(uint32_t sub);                           // 0x0098 {0x01 u32}
Message buildSetServiceEnableConfig(std::optional<bool> volte, std::optional<bool> imsService);
Result bindSubscription(Client& c, uint32_t sub = 0);
Result getServiceEnableConfig(Client& c, ServiceEnableConfig* out);
Result setServiceEnableConfig(Client& c, std::optional<bool> volte, std::optional<bool> imsService);
}  // namespace imss

}  // namespace a6l::qmi
