// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (telephony-flows, 29 Sep 2026): QMI VOICE USSD (IRadioVoice sendUssd / cancelPendingUssd / onUssd).
//
// Message ids and TLV layouts from the public QMI VOICE IDL as published by libqmi (data/qmi-service-voice.json,
// qmi-enums-voice.h) and used by ModemManager (mm-broadband-modem-qmi.c ussd_encode/ussd_decode). Offline only: not
// yet compared with a capture of this MPSS (attended test: *#100# / Orange *144#).
//   0x003A Originate USSD (sync, the answer is in the response; ModemManager waits up to 100 s)
//          req 0x01 USS data {u8 dcs, u8 len, len bytes}
//          resp 0x10 failure cause u16, 0x11 alpha id, 0x12 USS data, 0x13 CC result type, 0x14 call id, 0x16 UTF-16
//   0x003B Answer USSD      req 0x01 USS data
//   0x003C Cancel USSD      (no TLV)
//   0x003D Release USSD indication (no TLV): the network ended the session
//   0x003E USSD indication  0x01 user action u8 (1 not required, 2 required), 0x10 USS data, 0x11 UTF-16 (u8 count +
//          count x le16)
//   0x0043 Originate USSD No Wait (response = result only) + indication 0x0043: 0x10 error u16, 0x11 failure cause u16,
//          0x12 USS data, 0x13 alpha id, 0x14 UTF-16
//   Indication Register (0x0003) TLV 0x16 = USSD notification events.
// USS data coding scheme: 1 ASCII, 2 8-bit, 3 UCS2 (big endian, as ModemManager's MM_MODEM_CHARSET_UCS2).
#pragma once

#include <a6lqmi/client.h>
#include <a6lqmi/message.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a6l::qmi::voice::ussd {

enum : uint16_t {
    kOriginate = 0x003A,
    kAnswer = 0x003B,
    kCancel = 0x003C,
    kReleaseInd = 0x003D,
    kUssdInd = 0x003E,
    kOriginateNoWait = 0x0043,  // request and indication share the id
};
enum : uint8_t { kDcsUnknown = 0, kDcsAscii = 1, kDcs8bit = 2, kDcsUcs2 = 3 };
enum : uint8_t { kActionUnknown = 0, kActionNotRequired = 1, kActionRequired = 2 };
constexpr uint8_t kTlvRegisterUssdEvents = 0x16;  // Indication Register
constexpr size_t kMaxUssBytes = 182;              // QMI_VOICE_USS_DATA_MAX (160 GSM 7-bit chars packed)

struct Encoded {
    uint8_t dcs = kDcsAscii;
    std::vector<uint8_t> data;
};
// UTF-8 USSD string from Android -> QMI USS data. ASCII when every byte is 0x01..0x7F, else UCS2 (BMP only).
// nullopt: empty, invalid UTF-8, a character outside the BMP, or longer than kMaxUssBytes once encoded.
std::optional<Encoded> encode(const std::string& utf8);

struct Text {
    bool present = false;      // a USS data / UTF-16 TLV was there
    bool decodeError = false;  // present but not decodable (unknown DCS, odd UCS2 length, bad framing)
    std::string utf8;
};
// USS data TLV value {u8 dcs, u8 len, bytes} -> text. ASCII and 8-bit (Latin-1) and UCS2-BE are decoded.
Text decodeUssData(const std::vector<uint8_t>& tlv);
// UTF-16 TLV value {u8 count, count x le16} -> text (surrogate pairs combined; lone surrogates -> decodeError).
Text decodeUtf16(const std::vector<uint8_t>& tlv);
// Prefer the UTF-16 TLV (the modem's own conversion) and fall back to the USS data TLV, as ModemManager does.
Text textOf(const Message& m, uint8_t ussTlv, uint8_t utf16Tlv);

// What the HAL gets from ModemCore for every USSD-related indication (and the sync originate fallback).
struct Event {
    enum Kind { Notification, Released, OriginateResult } kind = Notification;
    uint8_t userAction = kActionUnknown;  // Notification
    Text text;                            // Notification / OriginateResult
    std::optional<uint16_t> error;        // OriginateResult: QMI error of the network operation
    std::optional<uint16_t> failureCause; // OriginateResult: QmiVoiceCallEndReason
};
// nullopt for an indication id that is not USSD-related or a malformed message
std::optional<Event> parseIndication(const Message& ind);
// Sync Originate USSD response -> OriginateResult event (error = the QMI error when the result TLV says failure)
Event fromOriginateResponse(const Result& r);

Message buildUssRequest(uint16_t msgId, const Encoded& e);
Result originateNoWait(Client& c, const Encoded& e);
Result originate(Client& c, const Encoded& e, int timeoutMs = 100000);
Result answer(Client& c, const Encoded& e);
Result cancel(Client& c);
// The modem does not know the request (older VOICE): the only case where the HAL falls back to the sync originate.
bool isUnsupported(const Result& r);

}  // namespace a6l::qmi::voice::ussd
