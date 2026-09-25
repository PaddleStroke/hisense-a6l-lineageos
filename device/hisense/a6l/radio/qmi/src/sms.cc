// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): minimal SMS PDU helpers.
#include <a6lqmi/message.h>
#include <a6lqmi/sms.h>

#include <cstdio>

namespace a6l::sms {

namespace {
// ASCII -> GSM 7-bit default alphabet (subset that maps 1:1)
int toGsm7(char c) {
    if (c >= 'A' && c <= 'Z') return c;
    if (c >= 'a' && c <= 'z') return c;
    if (c >= '0' && c <= '9') return c;
    switch (c) {
        case ' ': case '!': case '"': case '#': case '%': case '&': case '\'': case '(': case ')':
        case '*': case '+': case ',': case '-': case '.': case '/': case ':': case ';': case '<':
        case '=': case '>': case '?':
            return c;
        case '@': return 0x00;
        case '$': return 0x02;
        case '_': return 0x11;
        case '\n': return 0x0A;
        default: return -1;
    }
}
}  // namespace

std::vector<uint8_t> gsm7Pack(const std::vector<uint8_t>& s) {
    std::vector<uint8_t> out((s.size() * 7 + 7) / 8, 0);
    for (size_t i = 0; i < s.size(); i++) {
        size_t bit = i * 7, byte = bit / 8, sh = bit % 8;
        out[byte] |= static_cast<uint8_t>((s[i] & 0x7f) << sh);
        if (sh > 1) out[byte + 1] |= static_cast<uint8_t>((s[i] & 0x7f) >> (8 - sh));
    }
    return out;
}

std::optional<std::vector<uint8_t>> buildSubmit(const std::string& number, const std::string& text,
                                                uint8_t mr) {
    std::string digits;
    bool intl = false;
    for (size_t i = 0; i < number.size(); i++) {
        char c = number[i];
        if (i == 0 && c == '+') {
            intl = true;
            continue;
        }
        if (c < '0' || c > '9') return std::nullopt;
        digits += c;
    }
    if (digits.empty() || digits.size() > 20) return std::nullopt;
    std::vector<uint8_t> sept;
    for (char c : text) {
        int g = toGsm7(c);
        if (g < 0) return std::nullopt;
        sept.push_back(static_cast<uint8_t>(g));
    }
    if (sept.size() > 160) return std::nullopt;
    std::vector<uint8_t> p;
    p.push_back(0x00);  // SMSC: use default
    p.push_back(0x01);  // SMS-SUBMIT, no VP, no SRR
    p.push_back(mr);
    p.push_back(static_cast<uint8_t>(digits.size()));
    p.push_back(intl ? 0x91 : 0x81);
    for (size_t i = 0; i < digits.size(); i += 2) {
        uint8_t lo = digits[i] - '0';
        uint8_t hi = i + 1 < digits.size() ? digits[i + 1] - '0' : 0x0f;
        p.push_back(static_cast<uint8_t>(hi << 4 | lo));
    }
    p.push_back(0x00);  // PID
    p.push_back(0x00);  // DCS GSM7
    p.push_back(static_cast<uint8_t>(sept.size()));
    auto ud = gsm7Pack(sept);
    p.insert(p.end(), ud.begin(), ud.end());
    return p;
}

std::string decodeBcdNumber(const uint8_t* p, size_t octets, uint8_t toa, size_t ndigits) {
    std::string s;
    if ((toa & 0x70) == 0x50) {  // alphanumeric originator (GSM7 packed)
        return qmi::gsm7Unpack(p, octets, ndigits * 4 / 7);
    }
    if ((toa & 0x70) == 0x10) s += '+';
    for (size_t i = 0; i < octets; i++) {
        uint8_t lo = p[i] & 0x0f, hi = p[i] >> 4;
        if (lo <= 9) s += static_cast<char>('0' + lo);
        if (hi <= 9) s += static_cast<char>('0' + hi);
    }
    return s;
}

namespace {
// Parses the TPDU starting at `off`. strict: every field must be plausible and, for SMS-DELIVER,
// the PDU length must match UDL exactly (used for form detection).
std::optional<Deliver> parseTpdu(const std::vector<uint8_t>& pdu, size_t off, bool strict,
                                 Deliver d) {
    if (off >= pdu.size()) return std::nullopt;
    qmi::Reader r(pdu.data() + off, pdu.size() - off);
    d.firstOctet = r.u8();
    uint8_t mti = d.firstOctet & 0x03;
    if (mti == 0x02) {  // SMS-STATUS-REPORT: MR, RA, SCTS(7), DT(7), ST
        d.statusReport = true;
        r.u8();  // MR
        uint8_t n = r.u8(), toa = r.u8();
        if (strict && (n > 20 || !(toa & 0x80))) return std::nullopt;
        auto b = r.bytes((n + 1) / 2);
        if (!r.good()) return std::nullopt;
        if (strict && r.remaining() < 15) return std::nullopt;
        d.originator = decodeBcdNumber(b.data(), b.size(), toa, n);
        return d;
    }
    if (mti != 0x00) return std::nullopt;
    bool udhi = d.firstOctet & 0x40;
    uint8_t n = r.u8(), toa = r.u8();
    if (strict && (n > 20 || !(toa & 0x80))) return std::nullopt;
    auto oa = r.bytes((n + 1) / 2);
    if (!r.good()) return std::nullopt;
    d.originator = decodeBcdNumber(oa.data(), oa.size(), toa, n);
    d.pid = r.u8();
    d.dcs = r.u8();
    auto ts = r.bytes(7);
    uint8_t udl = r.u8();
    if (!r.good()) return std::nullopt;
    bool ucs2 = (d.dcs & 0x0c) == 0x08;
    bool eightbit = (d.dcs & 0x0c) == 0x04;
    size_t udOctets = (ucs2 || eightbit) ? udl : (udl * 7u + 7u) / 8u;
    if (strict && (udOctets > 140 || r.remaining() != udOctets)) return std::nullopt;
    if (!strict && r.remaining() < udOctets) udOctets = r.remaining();
    char tb[32];
    auto sw = [&](uint8_t b) { return (b & 0x0f) * 10 + (b >> 4); };
    snprintf(tb, sizeof tb, "%02d%02d%02d%02d%02d%02d", sw(ts[0]), sw(ts[1]), sw(ts[2]), sw(ts[3]),
             sw(ts[4]), sw(ts[5]));
    d.timestamp = tb;
    size_t udStart = pdu.size() - r.remaining();
    std::vector<uint8_t> ud(pdu.begin() + udStart, pdu.begin() + udStart + udOctets);
    size_t udhOctets = 0;
    if (udhi && !ud.empty()) udhOctets = ud[0] + 1;
    if (ucs2 || eightbit) {
        for (size_t i = udhOctets; i < ud.size() && i < udl; i += ucs2 ? 2 : 1) {
            unsigned cp = ucs2 ? (i + 1 < ud.size() ? (ud[i] << 8 | ud[i + 1]) : 0) : ud[i];
            if (cp < 0x80) d.text += static_cast<char>(cp);
            else if (cp < 0x800) {
                d.text += static_cast<char>(0xC0 | (cp >> 6));
                d.text += static_cast<char>(0x80 | (cp & 0x3F));
            } else {
                d.text += static_cast<char>(0xE0 | (cp >> 12));
                d.text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                d.text += static_cast<char>(0x80 | (cp & 0x3F));
            }
        }
    } else {
        std::string all = qmi::gsm7Unpack(ud.data(), ud.size(), udl);
        // skip the septets covered by the UDH (+fill bits)
        size_t skip = udhOctets ? (udhOctets * 8 + 6) / 7 : 0;
        // gsm7Unpack returns UTF-8; UDH septets are 1 byte each only if ASCII; best effort
        d.text = skip < all.size() ? all.substr(skip) : std::string();
    }
    return d;
}

std::optional<Deliver> parsePrefixed(const std::vector<uint8_t>& pdu, bool strict) {
    if (pdu.empty()) return std::nullopt;
    Deliver d;
    uint8_t smscLen = pdu[0];
    if (smscLen) {
        if (1u + smscLen > pdu.size()) return std::nullopt;
        uint8_t toa = pdu[1];
        if (strict && (smscLen > 11 || !(toa & 0x80))) return std::nullopt;
        d.smsc = decodeBcdNumber(pdu.data() + 2, smscLen - 1, toa, (smscLen - 1) * 2);
    }
    return parseTpdu(pdu, 1u + smscLen, strict, d);
}
}  // namespace

std::optional<Deliver> parseDeliver(const std::vector<uint8_t>& pdu, SmscForm form,
                                    SmscForm* detected) {
    if (detected) *detected = SmscForm::Auto;
    auto done = [&](std::optional<Deliver> d, SmscForm f) {
        if (d && detected) *detected = f;
        return d;
    };
    if (form == SmscForm::Prefixed) return done(parsePrefixed(pdu, false), SmscForm::Prefixed);
    if (form == SmscForm::Bare) return done(parseTpdu(pdu, 0, false, Deliver{}), SmscForm::Bare);
    if (auto d = parsePrefixed(pdu, true)) return done(d, SmscForm::Prefixed);
    if (auto d = parseTpdu(pdu, 0, true, Deliver{})) return done(d, SmscForm::Bare);
    // lenient fallbacks (truncated / padded PDUs): SMSC-prefixed first (historic behaviour)
    if (auto d = parsePrefixed(pdu, false)) return done(d, SmscForm::Prefixed);
    return done(parseTpdu(pdu, 0, false, Deliver{}), SmscForm::Bare);
}

SmscForm detectForm(const std::vector<uint8_t>& pdu) {
    if (parsePrefixed(pdu, true)) return SmscForm::Prefixed;
    if (parseTpdu(pdu, 0, true, Deliver{})) return SmscForm::Bare;
    return SmscForm::Auto;
}

std::vector<uint8_t> toSmscPrefixed(const std::vector<uint8_t>& pdu) {
    if (detectForm(pdu) != SmscForm::Bare) return pdu;
    std::vector<uint8_t> out;
    out.reserve(pdu.size() + 1);
    out.push_back(0x00);
    out.insert(out.end(), pdu.begin(), pdu.end());
    return out;
}

}  // namespace a6l::sms
