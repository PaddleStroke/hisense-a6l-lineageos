// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril, 24 Sep 2026): QMI message codec for QMI-over-QRTR.
//
// Wire format on a QRTR socket (no QMUX/CTL layer, one socket = one QMI client per service):
//   u8 type (0 request, 2 response, 4 indication) | le16 txn | le16 msg_id | le16 tlv_len | TLVs
//   TLV: u8 type | le16 len | value
// Everything is little-endian except where a service says otherwise (IPv6 addresses are raw bytes).
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace a6l::qmi {

enum class MsgType : uint8_t { Request = 0x00, Response = 0x02, Indication = 0x04 };

// Common TLV of every response.
constexpr uint8_t kTlvResult = 0x02;

// QMI error codes we look at explicitly (subset of QMI_PROTOCOL_ERROR_*).
enum QmiError : uint16_t {  // values verified against libqmi qmi-errors.h
    kErrNone = 0,
    kErrMalformedMessage = 1,
    kErrNoMemory = 2,
    kErrInternal = 3,
    kErrInvalidHandle = 9,
    kErrIncorrectPin = 12,
    kErrNoNetworkFound = 13,
    kErrCallFailed = 14,
    kErrOutOfCall = 15,
    kErrMissingArgument = 17,
    kErrDeviceUnsupported = 25,
    kErrNoEffect = 26,
    kErrAuthenticationFailed = 34,
    kErrPinBlocked = 35,
    kErrPinAlwaysBlocked = 36,
    kErrUimUninitialized = 37,
    kErrInterfaceNotFound = 43,
    kErrGeneralError = 46,
    kErrInvalidArgument = 48,
    kErrDeviceNotReady = 52,
    kErrNetworkNotReady = 53,
    kErrWmsCauseCode = 54,
    kErrWmsMessageNotSent = 55,
    kErrWmsMessageDeliveryFailure = 56,
    kErrInvalidTransition = 60,
    kErrInvalidOperation = 70,
    kErrInvalidQmiCommand = 71,
    kErrInformationUnavailable = 74,
    kErrSimFileNotFound = 80,
    kErrAccessDenied = 82,
    kErrIncompatibleState = 90,
    kErrFdnRestrict = 91,
    kErrNoRadio = 93,
    kErrNotSupported = 94,
    kErrNoSubscription = 95,
    kErrNoSim = 102,
};

std::string errorName(uint16_t err);

class Message {
  public:
    MsgType type = MsgType::Request;
    uint16_t txn = 0;
    uint16_t msgId = 0;
    // QMI forbids duplicate TLV types in one message; ordered for deterministic encoding.
    std::map<uint8_t, std::vector<uint8_t>> tlvs;

    Message() = default;
    Message(MsgType t, uint16_t id) : type(t), msgId(id) {}
    static Message request(uint16_t id) { return Message(MsgType::Request, id); }

    std::vector<uint8_t> encode() const;
    // Returns nullopt on any framing error (short header, TLV overrun, length mismatch).
    static std::optional<Message> decode(const uint8_t* data, size_t len);
    static std::optional<Message> decode(const std::vector<uint8_t>& v) {
        return decode(v.data(), v.size());
    }

    // Builders
    Message& u8(uint8_t t, uint8_t v);
    Message& u16(uint8_t t, uint16_t v);
    Message& u32(uint8_t t, uint32_t v);
    Message& u64(uint8_t t, uint64_t v);
    Message& raw(uint8_t t, std::vector<uint8_t> v);
    Message& str8(uint8_t t, const std::string& s);  // u8 length prefix
    Message& strNoLen(uint8_t t, const std::string& s);  // TLV length is the length

    const std::vector<uint8_t>* get(uint8_t t) const;
    bool has(uint8_t t) const { return tlvs.count(t) != 0; }

    // Response helpers (TLV 0x02: le16 result, le16 error)
    bool hasResult() const;
    bool resultOk() const;
    uint16_t error() const;

    std::string dump() const;  // one-line hex dump for logs
};

// Bounds-checked little-endian reader over a TLV value. Any overrun sets !good() and returns 0/"".
class Reader {
  public:
    Reader(const uint8_t* p, size_t n) : mP(p), mN(n) {}
    explicit Reader(const std::vector<uint8_t>& v) : mP(v.data()), mN(v.size()) {}
    explicit Reader(const std::vector<uint8_t>* v)
        : mP(v ? v->data() : nullptr), mN(v ? v->size() : 0), mOk(v != nullptr) {}

    uint8_t u8();
    int8_t i8() { return static_cast<int8_t>(u8()); }
    uint16_t u16();
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32();
    int32_t i32() { return static_cast<int32_t>(u32()); }
    uint64_t u64();
    std::vector<uint8_t> bytes(size_t n);
    std::string str(size_t n);
    std::string str8() { return str(u8()); }
    std::string str16() { return str(u16()); }
    std::vector<uint8_t> bytes8() { return bytes(u8()); }
    std::vector<uint8_t> bytes16() { return bytes(u16()); }
    void skip(size_t n);

    bool good() const { return mOk; }
    size_t remaining() const { return mOk ? mN - mPos : 0; }
    bool atEnd() const { return remaining() == 0; }

  private:
    bool need(size_t n);
    const uint8_t* mP;
    size_t mN;
    size_t mPos = 0;
    bool mOk = true;
};

// Helpers shared by services
std::string hex(const uint8_t* p, size_t n, char sep = 0);
inline std::string hex(const std::vector<uint8_t>& v, char sep = 0) {
    return hex(v.data(), v.size(), sep);
}
std::optional<std::vector<uint8_t>> unhex(const std::string& s);
// QMI carries IPv4 addresses as le32 numeric values (0x0A000001 == 10.0.0.1).
std::string ipv4ToString(uint32_t v);
std::string ipv6ToString(const uint8_t* p16);
int maskToPrefix(uint32_t mask);
// Operator descriptions in NAS are sometimes GSM 7-bit packed (see libqmi test "Iliad").
std::string decodeNetworkDescription(const std::string& raw);
std::string gsm7Unpack(const uint8_t* p, size_t n, size_t septets = 0);

}  // namespace a6l::qmi
