// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): minimal 3GPP TS 23.040 PDU helpers for the test CLI.
// (The HAL passes Android's PDUs through unchanged.)
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace a6l::sms {

// SMS-SUBMIT, GSM 7-bit default alphabet (ASCII letters/digits/basic punctuation only),
// prefixed with 00 (use the SIM's SMSC). Returns nullopt if the text is not encodable or > 160.
std::optional<std::vector<uint8_t>> buildSubmit(const std::string& number, const std::string& text,
                                                uint8_t messageRef = 0);

struct Deliver {
    std::string smsc, originator, text;
    uint8_t firstOctet = 0, pid = 0, dcs = 0;
    std::string timestamp;  // YYMMDDhhmmss+tz
    bool statusReport = false;
};
// Form of an MT GW PDU. Real A6L captures (24 Sep 2026): the WMS event report (TLV 0x11, format 6
// GW_PP, transfer-only route) carries the bare TPDU WITHOUT the SMSC length prefix, e.g.
// 040B913366336322F8... Android's newSms() and the classic +CMGR form have "SMSC (length-prefixed) +
// TPDU". Auto tries both and keeps the one whose fields are valid and whose length matches exactly.
enum class SmscForm { Auto, Prefixed, Bare };
// Parses SMS-DELIVER / SMS-STATUS-REPORT. `detected` (optional) receives the form actually used.
std::optional<Deliver> parseDeliver(const std::vector<uint8_t>& pdu, SmscForm form = SmscForm::Auto,
                                    SmscForm* detected = nullptr);
// Returns Prefixed or Bare (Auto if neither form parses).
SmscForm detectForm(const std::vector<uint8_t>& pdu);
// Android newSms()/newSmsStatusReport() want "SMSC + TPDU": prepends 00 (no SMSC) to a bare TPDU,
// returns the input unchanged when it already has an SMSC prefix (or cannot be parsed).
std::vector<uint8_t> toSmscPrefixed(const std::vector<uint8_t>& pdu);

std::vector<uint8_t> gsm7Pack(const std::vector<uint8_t>& septets);
std::string decodeBcdNumber(const uint8_t* p, size_t octets, uint8_t toa, size_t digits);

}  // namespace a6l::sms
