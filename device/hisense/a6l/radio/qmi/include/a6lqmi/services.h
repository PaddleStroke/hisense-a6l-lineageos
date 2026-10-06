// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): typed QMI requests/parsers for DMS, UIM, NAS, WMS, VOICE, WDS, WDA.
// Message ids / TLV layouts follow libqmi data/qmi-service-*.json (checked 24 Sep 2026).
// Rule (libqmi): a string that is a whole TLV has no length prefix; a string inside a struct has a
// u8 length prefix unless noted.
#pragma once

#include <a6lqmi/client.h>

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace a6l::qmi {

// ---------------------------------------------------------------- DMS
namespace dms {
enum : uint16_t {
    kSetEventReport = 0x0001,
    kGetRevision = 0x0023,
    kGetMsisdn = 0x0024,
    kGetIds = 0x0025,
    kGetOperatingMode = 0x002D,
    kSetOperatingMode = 0x002E,
    kUimGetIccid = 0x003C,
    kUimGetImsi = 0x0043,
};
enum OperatingMode : uint8_t {
    kOnline = 0,
    kLowPower = 1,
    kFactoryTest = 2,
    kOffline = 3,
    kReset = 4,
    kShuttingDown = 5,
    kPersistentLowPower = 6,
    kModeOnlyLowPower = 7,
    kUnknownMode = 0xFF,
};
struct Ids {
    std::string esn, imei, meid, imeisv;
};
std::optional<Ids> parseIds(const Message& m);
Result getIds(Client& c, Ids* out);
Result getRevision(Client& c, std::string* out);
Result getMsisdn(Client& c, std::string* out);
Result getOperatingMode(Client& c, uint8_t* mode);
Result setOperatingMode(Client& c, uint8_t mode);
Result uimGetIccid(Client& c, std::string* out);
Result uimGetImsi(Client& c, std::string* out);
}  // namespace dms

// ---------------------------------------------------------------- UIM
namespace uim {
enum : uint16_t {
    kReadTransparent = 0x0020,
    kReadRecord = 0x0021,
    kWriteTransparent = 0x0022,
    kWriteRecord = 0x0023,
    kGetFileAttributes = 0x0024,
    kSetPinProtection = 0x0025,
    kVerifyPin = 0x0026,
    kUnblockPin = 0x0027,
    kChangePin = 0x0028,
    kRegisterEvents = 0x002E,
    kGetCardStatus = 0x002F,
    kCardStatusInd = 0x0032,
    kGetSlotStatus = 0x0047,
};
enum SessionType : uint8_t {
    kSessionPrimaryGw = 0,
    kSessionCardSlot1 = 6,
};
enum CardState : uint8_t { kCardAbsent = 0, kCardPresent = 1, kCardError = 2 };
enum PinState : uint8_t {
    kPinNotInitialized = 0,
    kPinEnabledNotVerified = 1,
    kPinEnabledVerified = 2,
    kPinDisabled = 3,
    kPinBlocked = 4,
    kPinPermBlocked = 5,
};
enum AppType : uint8_t { kAppUnknown = 0, kAppSim = 1, kAppUsim = 2, kAppRuim = 3, kAppCsim = 4, kAppIsim = 5 };
enum AppState : uint8_t {
    kAppStateUnknown = 0,
    kAppStateDetected = 1,
    kAppStatePin = 2,
    kAppStatePuk = 3,
    kAppStatePerso = 4,
    kAppStatePinBlocked = 5,
    kAppStateIllegal = 6,
    kAppStateReady = 7,
};
enum FileType : uint8_t { kFileTransparent = 0, kFileCyclic = 1, kFileLinearFixed = 2, kFileDf = 3, kFileMf = 4 };
enum PinId : uint8_t { kPin1 = 1, kPin2 = 2, kUpin = 3 };

struct App {
    uint8_t type = 0, state = 0, persoState = 0, persoFeature = 0, persoRetries = 0,
            persoUnblockRetries = 0;
    std::vector<uint8_t> aid;
    bool upinReplacesPin1 = false;
    uint8_t pin1State = 0, pin1Retries = 0, puk1Retries = 0;
    uint8_t pin2State = 0, pin2Retries = 0, puk2Retries = 0;
};
struct Card {
    uint8_t state = kCardAbsent, upinState = 0, upinRetries = 0, upukRetries = 0, error = 0;
    std::vector<App> apps;
};
struct CardStatus {
    uint16_t indexGwPrimary = 0xFFFF, index1xPrimary = 0xFFFF, indexGwSecondary = 0xFFFF,
             index1xSecondary = 0xFFFF;
    std::vector<Card> cards;
    // Convenience: the primary GW app (card = index >> 8, app = index & 0xff)
    const App* primaryGwApp() const;
    const Card* primaryCard() const;
};
std::optional<CardStatus> parseCardStatus(const std::vector<uint8_t>& tlv);
Result getCardStatus(Client& c, CardStatus* out);
Result registerEvents(Client& c, uint32_t mask = 1 /*card status*/);

struct Session {
    uint8_t type = kSessionPrimaryGw;
    std::vector<uint8_t> aid;
};
// path: list of 16-bit file ids from MF, e.g. {0x3F00, 0x7FFF}
struct FilePath {
    uint16_t fileId = 0;
    std::vector<uint16_t> path;
};
struct IoResult {
    uint8_t sw1 = 0, sw2 = 0;
    bool hasSw = false;
    std::vector<uint8_t> data;
};
struct FileAttributes {
    uint16_t fileSize = 0, fileId = 0;
    uint8_t fileType = 0;
    uint16_t recordSize = 0, recordCount = 0;
    std::vector<uint8_t> raw;  // FCP template as returned by the card
    uint8_t sw1 = 0, sw2 = 0;
    bool hasSw = false;
};
Message buildReadTransparent(const Session& s, const FilePath& f, uint16_t offset, uint16_t len);
Result readTransparent(Client& c, const Session& s, const FilePath& f, uint16_t offset,
                       uint16_t len, IoResult* out);
Result readRecord(Client& c, const Session& s, const FilePath& f, uint16_t record, uint16_t len,
                  IoResult* out);
Result writeTransparent(Client& c, const Session& s, const FilePath& f, uint16_t offset,
                        const std::vector<uint8_t>& data, IoResult* out);
Result writeRecord(Client& c, const Session& s, const FilePath& f, uint16_t record,
                   const std::vector<uint8_t>& data, IoResult* out);
Result getFileAttributes(Client& c, const Session& s, const FilePath& f, FileAttributes* out);
std::optional<FileAttributes> parseFileAttributes(const Message& m);
// Build the TS 51.011 GET RESPONSE (15 bytes) that Android's IccFileHandler expects from an
// EF's attributes (qcril does the same conversion for USIM FCP templates).
std::vector<uint8_t> toGsmGetResponse(const FileAttributes& a);

struct PinResult {
    int verifyLeft = -1, unblockLeft = -1;
};
Result verifyPin(Client& c, const Session& s, uint8_t pinId, const std::string& pin, PinResult* out);
Result unblockPin(Client& c, const Session& s, uint8_t pinId, const std::string& puk,
                  const std::string& newPin, PinResult* out);
Result changePin(Client& c, const Session& s, uint8_t pinId, const std::string& oldPin,
                 const std::string& newPin, PinResult* out);
Result setPinProtection(Client& c, const Session& s, uint8_t pinId, bool enable,
                        const std::string& pin, PinResult* out);

// EF decoders
std::string decodeIccid(const std::vector<uint8_t>& ef);  // EF_ICCID (2FE2), BCD swapped, F pad
std::string decodeImsi(const std::vector<uint8_t>& ef);   // EF_IMSI (6F07)
// High level helpers (UIM first, DMS fallback)
Result readIccid(Client& c, std::string* out);
Result readImsi(Client& c, const std::vector<uint8_t>& aid, std::string* out);
// ril3 (DSDS): same, for a given card session (6 slot 1, 7 slot 2) / provisioning session
// (0 primary GW, 2 secondary GW). dmsFallback: DMS UIM Get ICCID/IMSI (bound-subscription aware).
Result readIccid(Client& c, std::string* out, uint8_t cardSession, bool dmsFallback);
Result readImsi(Client& c, uint8_t provSession, const std::vector<uint8_t>& aid, std::string* out,
                bool dmsFallback);
}  // namespace uim

// ---------------------------------------------------------------- NAS
namespace nas {
enum : uint16_t {
    kRegisterIndications = 0x0003,
    kGetSignalStrength = 0x0020,
    kNetworkScan = 0x0021,
    kInitiateNetworkRegister = 0x0022,
    kGetServingSystem = 0x0024,  // also the indication id
    kGetHomeNetwork = 0x0025,
    kSetSystemSelectionPreference = 0x0033,
    kGetSystemSelectionPreference = 0x0034,
    kGetOperatorName = 0x0039,
    kOperatorNameInd = 0x003A,
    kGetCellLocationInfo = 0x0043,
    kNetworkTimeInd = 0x004C,
    kGetSystemInfo = 0x004D,
    kSystemInfoInd = 0x004E,
    kGetSignalInfo = 0x004F,
    kConfigSignalInfo = 0x0050,
    kSignalInfoInd = 0x0051,
};
enum RadioIf : int8_t {
    kRifNone = 0,
    kRifCdma1x = 1,
    kRifEvdo = 2,
    kRifAmps = 3,
    kRifGsm = 4,
    kRifUmts = 5,
    kRifLte = 8,
    kRifTdscdma = 9,
    kRif5gnr = 12,
};
enum RegState : uint8_t {
    kNotRegistered = 0,
    kRegistered = 1,
    kSearching = 2,
    kDenied = 3,
    kRegUnknown = 4,
};
enum AttachState : uint8_t { kAttachUnknown = 0, kAttached = 1, kDetached = 2 };
// QmiNasDataCapability
enum DataCap : uint8_t {
    kCapGprs = 1,
    kCapEdge = 2,
    kCapHsdpa = 3,
    kCapHsupa = 4,
    kCapWcdma = 5,
    kCapGsm = 9,
    kCapLte = 11,
    kCapHsdpaPlus = 12,
    kCapDcHsdpaPlus = 13,
};
struct ServingSystem {
    uint8_t regState = kRegUnknown, csAttach = 0, psAttach = 0, selectedNetwork = 0;
    std::vector<int8_t> radioIfs;
    std::optional<bool> roaming;  // TLV 0x10: 0 = ON (roaming), 1 = OFF
    std::vector<uint8_t> dataCaps;
    bool hasPlmn = false;
    uint16_t mcc = 0, mnc = 0;
    bool mnc3Digits = false;
    std::string description;  // decoded (GSM7 heuristics)
    std::optional<uint16_t> lac, tac;
    std::optional<uint32_t> cid;
    int8_t primaryRat() const { return radioIfs.empty() ? static_cast<int8_t>(kRifNone) : radioIfs[0]; }
    std::string mccStr() const;
    std::string mncStr() const;
};
// Response and indication share semantics but some TLV ids differ after 0x18.
std::optional<ServingSystem> parseServingSystem(const Message& m, bool isIndication);
Result getServingSystem(Client& c, ServingSystem* out);
const char* radioIfName(int8_t rif);  // "lte", "umts", "gsm", ...
std::string radioIfList(const ServingSystem& s);  // "lte" or "umts+gsm"

struct SignalInfo {
    std::optional<int8_t> gsmRssi;
    std::optional<int8_t> wcdmaRssi;
    std::optional<int16_t> wcdmaEcio;  // -0.5 dB units
    std::optional<int16_t> wcdmaRscp;
    bool hasLte = false;
    int8_t lteRssi = 0, lteRsrq = 0;
    int16_t lteRsrp = 0, lteSnr = 0;  // snr in 0.1 dB
};
SignalInfo parseSignalInfo(const Message& m);
Result getSignalInfo(Client& c, SignalInfo* out);
Result configSignalInfo(Client& c);
Result registerIndications(Client& c);

struct OperatorName {
    std::string spn, longName, shortName, operatorString;
};
Result getOperatorName(Client& c, OperatorName* out);
OperatorName parseOperatorName(const Message& m);

struct LteCell {
    bool valid = false;
    uint16_t tac = 0, earfcn = 0, pci = 0;
    uint32_t globalCellId = 0;
    std::vector<uint8_t> plmn;
    int16_t rsrp = 0, rsrq = 0, rssi = 0;
    std::optional<uint32_t> timingAdvance;
};
Result getLteCell(Client& c, LteCell* out);

// mode preference bits (QmiNasRatModePreference)
enum : uint16_t { kModeGsm = 1 << 2, kModeUmts = 1 << 3, kModeLte = 1 << 4 };
Result setModePreference(Client& c, uint16_t modeMask);
Result getModePreference(Client& c, uint16_t* modeMask);
// r5 round5 F46: manual selection carries the MNC width (NAS Initiate Network Register TLV 0x12 "MNC PCS Digit
// Include Status", libqmi qmi-service-nas.json): 001+01 and 001+001 are different PLMNs with the same integers.
Result setNetworkSelection(Client& c, bool manual, uint16_t mcc, uint16_t mnc, int8_t rat, bool mncThreeDigits = false);

struct ScanEntry {
    uint16_t mcc = 0, mnc = 0;
    uint8_t status = 0;
    std::string description;
    int8_t rat = 0;
    bool mnc3Digits = false;
};
Result networkScan(Client& c, std::vector<ScanEntry>* out, int timeoutMs = 180000);

// volte3 (26 Sep 2026): NAS Get Subscription Info (0x007C) / Subscription Info indication (0x0048). Layout from the
// stock IDL (libqmiservices.so); the meaning of 0x13/0x14/0x15 from stock qcrild (qcril_qmi_nas_get_subscription_info
// feeds them to qcril_am_set_vsid as vs_type VOICE / IMS / IMS-WLAN): the modem tells which ADSP CVD voice session
// (VSID) it uses for CS calls, VoLTE calls and VoWiFi calls of this subscription.
enum : uint16_t { kSubscriptionInfoInd = 0x0048, kGetSubscriptionInfo = 0x007C };
struct SubscriptionInfo {
    std::optional<uint8_t> priority, active, defaultData;
    std::optional<uint32_t> voiceVsid, lteVoiceVsid, wlanVoiceVsid;
    std::string summary() const;
};
SubscriptionInfo parseSubscriptionInfo(const Message& m);  // response 0x007C or indication 0x0048 (same TLV ids)
Result getSubscriptionInfo(Client& c, SubscriptionInfo* out);
const char* vsidName(uint32_t vsid);  // "VoiceMMode1" for 0x11C05000, "VoLTE" for 0x10C02000, ...

// volte3: IMS voice support advertised by the LTE cell (Get System Info 0x004D, TLVs 0x29 / 0x2A, as volte-probe)
struct ImsVoiceSupport {
    std::optional<uint8_t> lteImsVoice;      // 0x29: 1 = the network supports IMS voice over PS on LTE
    std::optional<uint32_t> lteVoiceDomain;  // 0x2A: 0 none, 1 IMS, 2 1X, 3 3GPP CS
};
Result getImsVoiceSupport(Client& c, ImsVoiceSupport* out);
}  // namespace nas

// ---------------------------------------------------------------- WMS
namespace wms {
enum : uint16_t {
    kSetEventReport = 0x0001,  // also the event report indication id
    kRawSend = 0x0020,
    kRawWrite = 0x0021,
    kRawRead = 0x0022,
    kDelete = 0x0024,
    kSetRoutes = 0x0032,
    kGetRoutes = 0x0033,
    kGetSmscAddress = 0x0034,
    kSendAck = 0x0037,
    kSetBroadcastActivation = 0x003C,
    kSetBroadcastConfig = 0x003D,  // r5 F12
};
enum : uint8_t { kFormatCdma = 0, kFormatGwPp = 6, kFormatGwBc = 7 };
enum : uint8_t { kStorageUim = 0, kStorageNv = 1, kStorageNone = 0xFF };
enum : uint8_t {
    kReceiptDiscard = 0,
    kReceiptStoreAndNotify = 1,
    kReceiptTransferOnly = 2,
    kReceiptTransferAndAck = 3,
};
struct SendResult {
    std::optional<uint16_t> messageRef;
    std::optional<uint16_t> rpCause;
    std::optional<uint8_t> tpCause;
    std::optional<uint8_t> failureType;  // 0 temporary, 1 permanent
};
// smscAndTpdu: "SMSC address (length-prefixed, 00 = default) + TPDU" exactly like Android's
// GsmSmsMessage smscPdu+pdu and +CMGS PDU mode.
Result rawSend(Client& c, const std::vector<uint8_t>& smscAndTpdu, bool expectMore,
               SendResult* out);
Result setEventReport(Client& c, bool enable);
// transfer-only routes for classes 0,1,2,3,none; storeFallback = store-and-notify on NV
Result setRoutes(Client& c, bool storeInsteadOfTransfer);
// ril3: explicit receipt action for classes 0-3,none (kReceiptTransferAndAck = the modem acks itself)
Result setRoutesAction(Client& c, uint8_t storage, uint8_t receiptAction);
// WMS Send Ack (0x0037). TLV 0x01 {txn u32, protocol u8 (0 CDMA, 1 WCDMA/GW), success u8}, 0x11 3GPP
// failure {rp, tp} only on a NACK, 0x12 "SMS on IMS" echoed from the event report. On
// QMI_ERR_ACK_NOT_SENT (84 = 0x54) the response TLV 0x10 carries the cause (ackFailureCauseName).
struct AckOptions {
    uint8_t protocol = 1;  // messageProtocolFor(format)
    std::optional<bool> smsOnIms;
};
Result sendAck(Client& c, uint32_t txn, bool success, uint8_t rpCause, uint8_t tpCause,
               const AckOptions& o = {}, int* failureCause = nullptr);
uint8_t messageProtocolFor(uint8_t format);  // CDMA format -> 0, GW formats -> 1 (like ModemManager)
const char* ackFailureCauseName(int cause);  // 0 no network response, 1 network released link, 2 not sent
// Bind this client's WMS to a subscription (0 primary, 1 secondary). IDL: TLV 0x01 enum8.
enum : uint16_t { kBindSubscription = 0x004C };
Result bindSubscription(Client& c, uint8_t sub);
Result rawRead(Client& c, uint8_t storage, uint32_t index, uint8_t* format,
               std::vector<uint8_t>* data);
Result deleteMessage(Client& c, uint8_t storage, uint32_t index);
Result setBroadcastActivation(Client& c, bool activate);
// r5 review fix F12 (28 Sep 2026): 3GPP cell broadcast channel configuration. WMS Set Broadcast Config (0x003D):
// TLV 0x01 message mode u8 (1 = GW), TLV 0x10 3GPP config {u8 count, count x {u16 from, u16 to, u8 selected}}
// (QMI WMS IDL wms_3gpp_broadcast_config_info, max 50 entries). Activation stays a separate request (0x003C).
// Layout from the public IDL, not yet checked against a stock qcril capture: a modem refusal is returned as is.
struct BroadcastRange {
    uint16_t from = 0, to = 0;
    bool selected = true;
    bool operator==(const BroadcastRange& o) const { return from == o.from && to == o.to && selected == o.selected; }
};
constexpr size_t kMaxBroadcastRanges = 50;
// Sorted, overlapping/adjacent ranges with the same `selected` merged; ranges with from > to are dropped by the
// caller (validation). Returns false when more than kMaxBroadcastRanges entries remain.
bool normalizeBroadcastRanges(std::vector<BroadcastRange> in, std::vector<BroadcastRange>* out);
Message buildSetBroadcastConfig(const std::vector<BroadcastRange>& ranges);
Result setBroadcastConfig(Client& c, const std::vector<BroadcastRange>& ranges);
Result getSmscAddress(Client& c, std::string* number, std::string* type);

struct EventReport {
    std::optional<std::pair<uint8_t, uint32_t>> stored;  // storage, index
    bool hasTransfer = false;
    uint8_t ackIndicator = 0;  // 0 = modem expects an ACK from us, 1 = do not send
    uint32_t txn = 0;
    uint8_t format = 0;
    std::vector<uint8_t> data;
    std::optional<uint8_t> messageMode;
    std::optional<std::string> smsc;
    std::optional<bool> smsOnIms;  // TLV 0x16
    // r5 deep F25: TLV 0x13 ETWS message {u8 notification type (0 primary, 1 secondary GSM, 2 secondary UMTS),
    // u16 length + raw data} -> Android newBroadcastSms as is (primary notification = 56-byte ETWS PDU)
    std::optional<uint8_t> etwsType;
    std::vector<uint8_t> etws;
    bool isBroadcast() const { return hasTransfer && format == kFormatGwBc; }
    // An ACK is owed only for transfer-route messages whose ack indicator is 0 (SEND). Stored
    // (store-and-notify) messages and ack indicator 1 must never be acked (-> ACK_NOT_SENT).
    bool needsAck() const { return hasTransfer && ackIndicator == 0; }
};
EventReport parseEventReport(const Message& m);
}  // namespace wms

// ---------------------------------------------------------------- VOICE
namespace voice {
enum : uint16_t {
    kIndicationRegister = 0x0003,
    kDialCall = 0x0020,
    kEndCall = 0x0021,
    kAnswerCall = 0x0022,
    kBurstDtmf = 0x0028,
    kStartContDtmf = 0x0029,
    kStopContDtmf = 0x002A,
    kAllCallStatusInd = 0x002E,
    kGetAllCallInfo = 0x002F,
    kManageCalls = 0x0031,
    kSuppServiceInd = 0x0032,
};
enum CallState : uint8_t {
    kStateUnknown = 0,
    kStateOrigination = 1,
    kStateIncoming = 2,
    kStateConversation = 3,
    kStateCcInProgress = 4,
    kStateAlerting = 5,
    kStateHold = 6,
    kStateWaiting = 7,
    kStateDisconnecting = 8,
    kStateEnd = 9,
    kStateSetup = 10,
};
enum CallDirection : uint8_t { kDirUnknown = 0, kDirMo = 1, kDirMt = 2 };
enum CallType : uint8_t { kTypeVoice = 0, kTypeVoiceIp = 2, kTypeEmergency = 9 };
enum Sups : uint8_t {
    kSupsReleaseHeldOrWaiting = 1,
    kSupsReleaseActiveAcceptHeldOrWaiting = 2,
    kSupsHoldActiveAcceptWaitingOrHeld = 3,
    kSupsHoldAllExceptSpecified = 4,
    kSupsMakeConference = 5,
    kSupsExplicitCallTransfer = 6,
    kSupsEndAllCalls = 8,
    kSupsReleaseSpecified = 9,
};
struct CallInfo {
    uint8_t id = 0, state = 0, type = 0, direction = 0, mode = 0;
    bool multiparty = false;
    uint8_t als = 0;
    std::string number;
    uint8_t presentation = 0;
    bool hasNumber = false;
};
std::vector<CallInfo> parseCalls(const Message& m, bool isIndication);
Result indicationRegister(Client& c);
Result dial(Client& c, const std::string& number, bool emergency, uint8_t* callId);
Result endCall(Client& c, uint8_t callId);
Result answer(Client& c, uint8_t callId);
Result getAllCalls(Client& c, std::vector<CallInfo>* out);
Result manageCalls(Client& c, uint8_t sups, std::optional<uint8_t> callId = std::nullopt);
Result startDtmf(Client& c, uint8_t callId, char digit);
Result stopDtmf(Client& c, uint8_t callId);
Result burstDtmf(Client& c, uint8_t callId, const std::string& digits);

// volte3 (26 Sep 2026): call domain (IMS/PS vs CS) from the call info (type + mode), VoLTE-related indications.
enum CallMode : uint8_t {
    kModeNoSrv = 0, kModeCdma = 1, kModeGsm = 2, kModeUmts = 3, kModeLte = 4, kModeTdscdma = 5,
    kModeUnknown = 6, kModeWlan = 7, kModeNr5g = 8,
};
enum : uint16_t { kAudioRatChangeInd = 0x0063 };  // stock IDL: 0x10 u32 audio session info, 0x11 u8 RAT (call mode)
const char* callTypeName(uint8_t type);
const char* callModeName(uint8_t mode);
// "ims" (VoLTE / VoWiFi / VoNR: type VOICE_IP/VT/EMERGENCY_IP or mode LTE/WLAN/NR5G), "cs" (mode GSM/UMTS/CDMA/TD),
// "unknown" (mode not reported yet, e.g. early ORIGINATION)
const char* callDomain(const CallInfo& c);
bool isImsCall(const CallInfo& c);
// dial with an explicit call type (TLV 0x10): kTypeVoice lets the modem choose the domain (voice_domain_pref),
// kTypeVoiceIp forces IMS (debug)
Result dialTyped(Client& c, const std::string& number, uint8_t callType, uint8_t* callId);
// indication register incl. the VoLTE extras: handover (0x14), speech codec (0x15), audio RAT change (0x23 per the
// stock qcrild request layout). The HAL keeps using indicationRegister().
Result indicationRegisterVolte(Client& c);
struct AudioRatInfo {
    std::optional<uint32_t> sessionInfo;  // qcrild: 1 + rat LTE -> treat as IMS over WLAN
    std::optional<uint8_t> rat;           // CallMode
};
AudioRatInfo parseAudioRatChange(const Message& m);

// r5 deep review F23/F24 (28 Sep 2026): typed dial request carried end-to-end from the HAL to the QMI encoding.
// QMI VOICE Dial Call (0x0020) optional TLVs per the public QMI VOICE IDL (voice_dial_call_req_msg): 0x10 call type,
// 0x11 CLIR type (1 = suppression / allow CLI, 2 = invocation / restrict CLI), 0x14 emergency service category (u8,
// 3GPP 24.008 10.5.4.33 bits = Android EmergencyServiceCategory bits 0-4). TLV ids from the IDL, not yet compared with
// a stock qcril capture: a modem refusal is returned (never replaced by a silent default).
enum : uint8_t { kClirSuppression = 1, kClirInvocation = 2 };
struct DialRequest {
    std::string number;
    bool emergency = false;                    // TLV 0x10 = kTypeEmergency; absent = modem default (voice)
    std::optional<uint8_t> clir;               // TLV 0x11; absent = subscription default (Dial.CLIR_DEFAULT)
    std::optional<uint8_t> emergencyCategory;  // TLV 0x14 (emergency only)
};
Message buildDial(const DialRequest& d);
Result dial(Client& c, const DialRequest& d, uint8_t* callId);
// Android Dial.clir (0 DEFAULT, 1 INVOCATION = restrict, 2 SUPPRESSION = allow) -> TLV 0x11; false = invalid value
bool clirFromAndroid(int32_t androidClir, std::optional<uint8_t>* out);
// numbers a modem/network treats as emergency whatever the call type (a test call must never be placed to them)
bool isWellKnownEmergencyNumber(const std::string& number);
// definitive QMI refusal of the request layout (optional TLV not understood by an older VOICE): the only failure
// after which an emergency request may be re-encoded. Timeouts/transport errors are never retried (call may exist).
bool isEncodingRejection(const Result& r);
enum class EmergencyRouting : int32_t { Unknown = 0, Emergency = 1, Normal = 2 };  // == AIDL EmergencyCallRouting
struct EmergencyDialRequest {
    std::string number;
    int32_t androidClir = 0;
    int32_t categories = 0;  // AIDL bitfield<EmergencyServiceCategory>
    EmergencyRouting routing = EmergencyRouting::Unknown;
    bool hasKnownUserIntentEmergency = false;
    bool isTesting = false;
};
struct EmergencyDialOutcome {
    Result result;
    bool refusedTestToEmergencyNumber = false;  // isTesting with a real emergency number: nothing sent
    std::vector<DialRequest> attempts;          // what was sent, in order
};
// IRadioVoice.emergencyDial semantics:
//  - isTesting: never an emergency call type / category; a normal dial of the (test) number, refused without any
//    request when the number is a well-known emergency number; no emergency fallback.
//  - routing NORMAL without known user intent: normal routing first; on a definitive modem refusal (no call created)
//    emergency routing ("if service cannot support normal routing, use emergency routing").
//  - routing EMERGENCY / UNKNOWN, or known user intent: emergency call type (+ category). Re-encoded only after an
//    encoding rejection: without the category, then (legacy VOICE without call type) a plain dial.
EmergencyDialOutcome emergencyDial(Client& c, const EmergencyDialRequest& req, uint8_t* callId);

// r5 review round7 (28 Sep 2026) F56-F58. Offline-only: TLV ids/values from the public QMI VOICE IDL and libqmi
// (qmi-enums-voice.h); not yet compared with a capture of this MPSS.
// F58: All Call Status Indication TLV 0x14 "Call End Reason" (voice_all_call_status_ind_msg: 0x10 remote number,
// 0x11 remote name, 0x12 alerting type, 0x13 service option, 0x14 call end reason): u8 count + {u8 call id, u16 reason}.
// A TLV whose length does not match exactly is ignored (reasons unknown) rather than misread.
std::vector<std::pair<uint8_t, uint16_t>> parseCallEndReasons(const Message& ind);
// Android LastCallFailCause values (IRadioVoice AIDL) used by the mapping.
enum : int32_t {
    kLcfNormal = 16, kLcfNormalUnspecified = 31, kLcfCallRejected = 21, kLcfRadioOff = 247, kLcfOutOfService = 248,
    kLcfNoValidSim = 249, kLcfRadioInternalError = 250, kLcfNetworkRespTimeout = 251, kLcfNetworkReject = 252,
    kLcfRadioAccessFailure = 253, kLcfRadioLinkLost = 255, kLcfAccessClassBlocked = 260, kLcfErrorUnspecified = 0xffff,
};
// Explicit QMI call end reason (QmiVoiceCallEndReason) -> Android LastCallFailCause. Never a raw cast; an unknown or
// unmapped reason is ERROR_UNSPECIFIED, never NORMAL.
int32_t lastCallFailCauseFromQmi(uint16_t qmiReason);

// F57: IRadioVoice.rejectCall = UDUB to the ringing or waiting call only. The call list is queried first; with
// neither an INCOMING nor a WAITING call nothing is sent (NoTarget) and a failed query sends nothing either.
// INCOMING -> End Call(id) (unchanged); WAITING -> Manage Calls RELEASE_HELD_OR_WAITING with that call id (TLV 0x10).
// Only if the modem refuses the call-id TLV itself (encoding rejection) and a fresh query still shows the same call
// waiting is the plain CHLD=0 form sent.
struct RejectOutcome {
    enum Kind { NoTarget, QueryFailed, EndedIncoming, ReleasedWaiting } kind = NoTarget;
    Result result;
    uint8_t callId = 0;
};
RejectOutcome rejectRingingOrWaiting(Client& c);

// F56: finite DTMF (IRadioVoice.sendDtmf) = START continuous + tone time + STOP. Both results count; a rejected or
// timed-out STOP is retried (bounded) while the same call id is still a live call; if the call ended meanwhile the
// tone ended with it (callGone). ok() only when START and a STOP succeeded.
struct FiniteDtmfOutcome {
    Result start, stop;
    int stopAttempts = 0;
    bool callGone = false;
    bool ok() const { return start.ok() && stop.ok(); }
};
bool isDtmfDigit(char c);  // the 12 keys IRadioVoice.sendDtmf allows: 0-9 * #
FiniteDtmfOutcome finiteDtmf(Client& c, uint8_t callId, char digit, int toneMs = 150, int stopTries = 3,
                             int retryMs = 100);
}  // namespace voice

// ---------------------------------------------------------------- WDS / WDA
namespace wds {
enum : uint16_t {
    kSetEventReport = 0x0001,
    kIndicationRegister = 0x0003,
    kStartNetwork = 0x0020,
    kStopNetwork = 0x0021,
    kPacketServiceStatus = 0x0022,  // request + indication
    kGetCurrentSettings = 0x002D,
    kSetIpFamily = 0x004D,
    kBindMuxDataPort = 0x00A2,
};
enum : uint32_t {
    kReqProfileId = 1u << 0,
    kReqProfileName = 1u << 1,
    kReqPdpType = 1u << 2,
    kReqApnName = 1u << 3,
    kReqDnsAddress = 1u << 4,
    kReqGrantedQos = 1u << 5,
    kReqUsername = 1u << 6,
    kReqAuthProtocol = 1u << 7,
    kReqIpAddress = 1u << 8,
    kReqGatewayInfo = 1u << 9,
    kReqPcscfAddress = 1u << 10,
    kReqPcscfServerList = 1u << 11,
    kReqPcscfDomainList = 1u << 12,
    kReqMtu = 1u << 13,
    kReqDomainNameList = 1u << 14,
    kReqIpFamily = 1u << 15,
};
// What Android needs for SetupDataCallResult. qrild never asked for DNS (mask without bit 4) and
// hard-coded 1.1.1.1; this mask is the fix.
constexpr uint32_t kSettingsMask = kReqDnsAddress | kReqIpAddress | kReqGatewayInfo | kReqMtu |
                                   kReqIpFamily | kReqPcscfServerList | kReqApnName |
                                   kReqDomainNameList;
enum : uint8_t { kFamilyV4 = 4, kFamilyV6 = 6, kFamilyUnspec = 8 };
enum : uint32_t { kEpEmbedded = 4 };
enum : uint8_t { kConnDisconnected = 1, kConnConnected = 2, kConnSuspended = 3, kConnAuthenticating = 4 };

struct StartParams {
    std::string apn, user, password;
    uint8_t auth = 0;  // bit0 PAP, bit1 CHAP
    uint8_t ipFamily = kFamilyV4;
    std::optional<uint8_t> profileIndex3gpp;
};
struct StartResult {
    uint32_t handle = 0;
    std::optional<uint16_t> callEndReason;
    std::optional<std::pair<uint16_t, int16_t>> verboseReason;  // type, reason
};
struct Settings {
    std::optional<uint32_t> ipv4, gw4, mask4, dns4a, dns4b;
    std::optional<std::array<uint8_t, 16>> ipv6, gw6, dns6a, dns6b;
    uint8_t ipv6Prefix = 0, gw6Prefix = 0;
    std::optional<uint32_t> mtu;
    std::optional<uint8_t> family;
    std::vector<uint32_t> pcscf4;
    std::string apn;
    std::vector<std::string> domains;
};
Result bindMuxDataPort(Client& c, uint32_t epType, uint32_t iface, uint8_t muxId);
Result setIpFamily(Client& c, uint8_t fam);
Result startNetwork(Client& c, const StartParams& p, StartResult* out, int timeoutMs = 60000);
Result stopNetwork(Client& c, uint32_t handle);
Result getCurrentSettings(Client& c, Settings* out, uint32_t mask = kSettingsMask);
Settings parseSettings(const Message& m);
Result getPacketServiceStatus(Client& c, uint8_t* status);
struct PacketStatus {
    uint8_t status = 0;
    bool reconfig = false;
    std::optional<uint16_t> callEndReason;
    std::optional<std::pair<uint16_t, int16_t>> verboseReason;
    std::optional<uint8_t> family;
};
PacketStatus parsePacketStatus(const Message& m);
}  // namespace wds

namespace wda {
enum : uint16_t { kSetDataFormat = 0x0020, kGetDataFormat = 0x0021 };
enum : uint32_t { kLlpRawIp = 2 };
// QmiWdaDataAggregationProtocol. SDM660 stock netmgr (listitem "Starlord") asks QMAPv3 (7) = QMAP +
// MAPv4 checksum offload; ipa2-lite has no checksum offload, so plain QMAP (5) is the default.
enum : uint32_t { kAggDisabled = 0, kAggQmap = 5, kAggQmapV3 = 7, kAggQmapV4 = 8, kAggQmapV5 = 9 };
struct DataFormat {
    uint32_t epType = wds::kEpEmbedded, iface = 1;
    uint32_t llp = kLlpRawIp;
    uint32_t ulAgg = kAggQmap, dlAgg = kAggQmap;
    uint32_t dlMaxDatagrams = 32, dlMaxSize = 32768;
};
Message buildSetDataFormat(const DataFormat& f);
Result setDataFormat(Client& c, const DataFormat& f, DataFormat* granted);
// Get Data Format (0x21) with the endpoint TLV 0x10; fills llp/ul/dl/dl_max from the response.
Result getDataFormat(Client& c, uint32_t epType, uint32_t iface, DataFormat* out);
}  // namespace wda

// ---------------------------------------------------------------- DPM (Data Port Mapper, 0x2F)
// data2 agent (25 Sep): on SDM660 the modem only accepts WDA/WDS bindings for an endpoint that was
// opened with DPM Open Port first (stock netmgr: qmi_dpm_enabled=1 -> netmgr_qmi_dpm_port_open;
// with the QRTR "shim" it opens NO control port, only the hardware data port; ModemManager does
// the same for driver "ipa"). Without it WDA Set Data Format answers INVALID_OPERATION.
// Wire (libqmi qmi-service-dpm.json, QMI IDL dpm_hardware_data_port_type_v01):
//   TLV 0x11: u8 count, then per port { le32 ep_type, le32 iface_id, le32 rx_ep, le32 tx_ep }
//   rx_ep = IPA consumer pipe = the AP TX endpoint (APPS_LAN_WAN_PROD, 4 on SDM660),
//   tx_ep = IPA producer pipe = the AP RX endpoint (APPS_WAN_CONS, 5) — downstream
//   RMNET_IOCTL_GET_EP_PAIR consumer/producer, MM "hw_port.rx_endpoint_number = tx_id".
namespace dpm {
enum : uint16_t { kOpenPort = 0x0020, kClosePort = 0x0021 };
struct HwDataPort {
    uint32_t epType = wds::kEpEmbedded, iface = 1;
    uint32_t rxEp = 4, txEp = 5;
};
struct CtlPort {  // only for SMD/QMUX control channels (not used over QRTR)
    std::string name;
    uint32_t epType = wds::kEpEmbedded, iface = 1;
};
Message buildOpenPort(const std::vector<HwDataPort>& hw, const std::vector<CtlPort>& ctl = {});
Result openPort(Client& c, const std::vector<HwDataPort>& hw, const std::vector<CtlPort>& ctl = {});
Result closePort(Client& c);
}  // namespace dpm

}  // namespace a6l::qmi
