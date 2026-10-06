// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): minimal SMS PDU helpers.
#include <a6lqmi/message.h>
#include <a6lqmi/sms.h>

#include <algorithm>
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
void appendUtf8(std::string& o, uint32_t cp);
std::string gsm7SeptetsToUtf8(const std::vector<uint8_t>& ud, size_t septets, size_t skip);
}  // namespace

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
        d.srMessageRef = r.u8();  // MR of the SMS-SUBMIT this report is about
        uint8_t n = r.u8(), toa = r.u8();
        if (strict && (n > 20 || !(toa & 0x80))) return std::nullopt;
        auto b = r.bytes((n + 1) / 2);
        if (!r.good()) return std::nullopt;
        if (strict && r.remaining() < 15) return std::nullopt;
        d.originator = decodeBcdNumber(b.data(), b.size(), toa, n);
        r.bytes(14);  // SCTS + DT
        uint8_t st = r.u8();
        if (r.good()) d.srStatus = st;
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
    if (udhi && !ud.empty()) {
        udhOctets = ud[0] + 1;
        // information elements: 00 = concat 8-bit ref (ref, total, seq), 08 = 16-bit ref
        for (size_t i = 1; i + 1 < udhOctets && i + 1 < ud.size();) {
            uint8_t iei = ud[i], len = ud[i + 1];
            if (i + 2 + len > ud.size()) break;
            if (iei == 0x00 && len == 3) {
                d.concatRef = ud[i + 2];
                d.concatTotal = ud[i + 3];
                d.concatSeq = ud[i + 4];
            } else if (iei == 0x08 && len == 4) {
                d.concatRef = static_cast<unsigned>(ud[i + 2] << 8 | ud[i + 3]);
                d.concatTotal = ud[i + 4];
                d.concatSeq = ud[i + 5];
            }
            i += 2u + len;
        }
    }
    if (ucs2 || eightbit) {
        for (size_t i = udhOctets; i < ud.size() && i < udl; i += ucs2 ? 2 : 1) {
            unsigned cp = ucs2 ? (i + 1 < ud.size() ? (ud[i] << 8 | ud[i + 1]) : 0) : ud[i];
            if (ucs2 && cp >= 0xD800 && cp < 0xDC00 && i + 3 < ud.size() && i + 3 < udl) {
                unsigned lo = ud[i + 2] << 8 | ud[i + 3];
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    i += 2;
                }
            }
            if (cp >= 0x10000) {
                d.text += static_cast<char>(0xF0 | (cp >> 18));
                d.text += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                d.text += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                d.text += static_cast<char>(0x80 | (cp & 0x3F));
            } else if (cp < 0x80) d.text += static_cast<char>(cp);
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
        // skip the septets covered by the UDH (+fill bits); decode per septet (escape table too)
        size_t skip = udhOctets ? (udhOctets * 8 + 6) / 7 : 0;
        d.text = gsm7SeptetsToUtf8(ud, udl, skip);
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


// ---------------------------------------------------------------- ril3: GSM7/UCS2 + concatenation
namespace {
// TS 23.038 default alphabet, index = septet, value = Unicode code point (0x1B = escape)
const uint16_t kGsm7Basic[128] = {
    0x40,  0xA3,  0x24,  0xA5,  0xE8,  0xE9,  0xF9,  0xEC,  0xF2,  0xC7,  0x0A,  0xD8,  0xF8,  0x0D,  0xC5,  0xE5,
    0x394, 0x5F,  0x3A6, 0x393, 0x39B, 0x3A9, 0x3A0, 0x3A8, 0x3A3, 0x398, 0x39E, 0x1B,  0xC6,  0xE6,  0xDF,  0xC9,
    0x20,  0x21,  0x22,  0x23,  0xA4,  0x25,  0x26,  0x27,  0x28,  0x29,  0x2A,  0x2B,  0x2C,  0x2D,  0x2E,  0x2F,
    0x30,  0x31,  0x32,  0x33,  0x34,  0x35,  0x36,  0x37,  0x38,  0x39,  0x3A,  0x3B,  0x3C,  0x3D,  0x3E,  0x3F,
    0xA1,  0x41,  0x42,  0x43,  0x44,  0x45,  0x46,  0x47,  0x48,  0x49,  0x4A,  0x4B,  0x4C,  0x4D,  0x4E,  0x4F,
    0x50,  0x51,  0x52,  0x53,  0x54,  0x55,  0x56,  0x57,  0x58,  0x59,  0x5A,  0xC4,  0xD6,  0xD1,  0xDC,  0xA7,
    0xBF,  0x61,  0x62,  0x63,  0x64,  0x65,  0x66,  0x67,  0x68,  0x69,  0x6A,  0x6B,  0x6C,  0x6D,  0x6E,  0x6F,
    0x70,  0x71,  0x72,  0x73,  0x74,  0x75,  0x76,  0x77,  0x78,  0x79,  0x7A,  0xE4,  0xF6,  0xF1,  0xFC,  0xE0,
};
struct Ext { uint16_t cp; uint8_t sept; };
const Ext kGsm7Ext[] = {{0x0C, 0x0A}, {'^', 0x14}, {'{', 0x28}, {'}', 0x29}, {'\\', 0x2F},
                        {'[', 0x3C}, {'~', 0x3D}, {']', 0x3E}, {'|', 0x40}, {0x20AC, 0x65}};

std::optional<std::vector<uint32_t>> utf8Decode(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        uint32_t cp;
        size_t n;
        if (c < 0x80) { cp = c; n = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; n = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; n = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; n = 4; }
        else return std::nullopt;
        if (i + n > s.size()) return std::nullopt;
        for (size_t k = 1; k < n; k++) {
            unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80) return std::nullopt;
            cp = cp << 6 | (cc & 0x3F);
        }
        if (cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return std::nullopt;
        out.push_back(cp);
        i += n;
    }
    return out;
}

std::optional<std::string> bcdAddress(const std::string& number, uint8_t* ndigits, uint8_t* toa) {
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
    std::string b;
    for (size_t i = 0; i < digits.size(); i += 2) {
        uint8_t lo = digits[i] - '0';
        uint8_t hi = i + 1 < digits.size() ? digits[i + 1] - '0' : 0x0f;
        b += static_cast<char>(hi << 4 | lo);
    }
    *ndigits = static_cast<uint8_t>(digits.size());
    *toa = intl ? 0x91 : 0x81;
    return b;
}

// septets packed starting at bit `startBit` of a zeroed buffer of `total` octets
void packAt(std::vector<uint8_t>& out, size_t startBit, const std::vector<uint8_t>& s) {
    for (size_t i = 0; i < s.size(); i++) {
        size_t bit = startBit + i * 7, byte = bit / 8, sh = bit % 8;
        out[byte] |= static_cast<uint8_t>((s[i] & 0x7f) << sh);
        if (sh > 1) out[byte + 1] |= static_cast<uint8_t>((s[i] & 0x7f) >> (8 - sh));
    }
}
}  // namespace

std::optional<std::vector<uint8_t>> utf8ToGsm7(const std::string& utf8) {
    auto cps = utf8Decode(utf8);
    if (!cps) return std::nullopt;
    std::vector<uint8_t> out;
    for (uint32_t cp : *cps) {
        int found = -1;
        for (int i = 0; i < 128; i++)
            if (i != 0x1B && kGsm7Basic[i] == cp) {
                found = i;
                break;
            }
        if (found >= 0) {
            out.push_back(static_cast<uint8_t>(found));
            continue;
        }
        bool ext = false;
        for (const auto& e : kGsm7Ext)
            if (e.cp == cp) {
                out.push_back(0x1B);
                out.push_back(e.sept);
                ext = true;
                break;
            }
        if (!ext) return std::nullopt;
    }
    return out;
}

std::optional<std::vector<uint16_t>> utf8ToUtf16(const std::string& utf8) {
    auto cps = utf8Decode(utf8);
    if (!cps) return std::nullopt;
    std::vector<uint16_t> out;
    for (uint32_t cp : *cps) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back(static_cast<uint16_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<uint16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<uint16_t>(cp));
        }
    }
    return out;
}

std::optional<SubmitParts> buildSubmitParts(const std::string& number, const std::string& utf8,
                                            const SubmitOptions& o) {
    uint8_t nd = 0, toa = 0;
    auto addr = bcdAddress(number, &nd, &toa);
    if (!addr || utf8.empty()) return std::nullopt;
    SubmitParts sp;
    std::optional<std::vector<uint8_t>> g7 = o.forceUcs2 ? std::nullopt : utf8ToGsm7(utf8);
    std::vector<std::vector<uint8_t>> chunks7;
    std::vector<std::vector<uint16_t>> chunks16;
    if (g7) {
        sp.coding = Coding::Gsm7;
        sp.units = g7->size();
        size_t per = g7->size() <= 160 ? 160 : 153;
        for (size_t i = 0; i < g7->size();) {
            size_t n = std::min(per, g7->size() - i);
            if (n < g7->size() - i && (*g7)[i + n - 1] == 0x1B) n--;  // never split an escape pair
            chunks7.emplace_back(g7->begin() + i, g7->begin() + i + n);
            i += n;
        }
    } else {
        auto u16 = utf8ToUtf16(utf8);
        if (!u16) return std::nullopt;
        sp.coding = Coding::Ucs2;
        sp.units = u16->size();
        size_t per = u16->size() <= 70 ? 70 : 67;
        for (size_t i = 0; i < u16->size();) {
            size_t n = std::min(per, u16->size() - i);
            uint16_t last = (*u16)[i + n - 1];
            if (n < u16->size() - i && last >= 0xD800 && last < 0xDC00) n--;  // keep surrogate pairs
            chunks16.emplace_back(u16->begin() + i, u16->begin() + i + n);
            i += n;
        }
    }
    size_t total = g7 ? chunks7.size() : chunks16.size();
    if (total == 0 || total > o.maxParts || total > 255) return std::nullopt;
    bool multi = total > 1;
    for (size_t k = 0; k < total; k++) {
        std::vector<uint8_t> p;
        p.push_back(0x00);  // SMSC: use the default (SIM) one
        p.push_back(static_cast<uint8_t>(0x01 | (multi ? 0x40 : 0) | (o.statusReport ? 0x20 : 0)));
        p.push_back(0x00);  // TP-MR (the modem assigns it)
        p.push_back(nd);
        p.push_back(toa);
        p.insert(p.end(), addr->begin(), addr->end());
        p.push_back(0x00);                  // PID
        p.push_back(g7 ? 0x00 : 0x08);      // DCS: GSM7 / UCS2
        std::vector<uint8_t> udh;
        if (multi) udh = {0x05, 0x00, 0x03, o.concatRef, static_cast<uint8_t>(total), static_cast<uint8_t>(k + 1)};
        if (g7) {
            const auto& c = chunks7[k];
            size_t hdrSept = multi ? 7 : 0;  // 6 octets UDH + 1 fill bit = 7 septets
            p.push_back(static_cast<uint8_t>(hdrSept + c.size()));
            std::vector<uint8_t> ud(((hdrSept + c.size()) * 7 + 7) / 8, 0);
            std::copy(udh.begin(), udh.end(), ud.begin());
            packAt(ud, hdrSept * 7, c);
            p.insert(p.end(), ud.begin(), ud.end());
        } else {
            const auto& c = chunks16[k];
            p.push_back(static_cast<uint8_t>(udh.size() + c.size() * 2));
            p.insert(p.end(), udh.begin(), udh.end());
            for (uint16_t u : c) {
                p.push_back(static_cast<uint8_t>(u >> 8));
                p.push_back(static_cast<uint8_t>(u & 0xff));
            }
        }
        sp.pdus.push_back(std::move(p));
    }
    return sp;
}


namespace {
void appendUtf8(std::string& o, uint32_t cp) {
    if (cp < 0x80) {
        o += static_cast<char>(cp);
    } else if (cp < 0x800) {
        o += static_cast<char>(0xC0 | (cp >> 6));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        o += static_cast<char>(0xE0 | (cp >> 12));
        o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        o += static_cast<char>(0x80 | (cp & 0x3F));
    }
}
std::string gsm7SeptetsToUtf8(const std::vector<uint8_t>& ud, size_t septets, size_t skip) {
    std::string out;
    bool esc = false;
    for (size_t i = skip; i < septets; i++) {
        size_t bit = i * 7, byte = bit / 8, sh = bit % 8;
        if (byte >= ud.size()) break;
        unsigned v = ud[byte] >> sh;
        if (sh > 1 && byte + 1 < ud.size()) v |= ud[byte + 1] << (8 - sh);
        v &= 0x7f;
        if (esc) {
            esc = false;
            uint32_t cp = kGsm7Basic[v];
            for (const auto& e : kGsm7Ext)
                if (e.sept == v) cp = e.cp;
            appendUtf8(out, cp);
            continue;
        }
        if (v == 0x1B) {
            esc = true;
            continue;
        }
        appendUtf8(out, kGsm7Basic[v]);
    }
    return out;
}
}  // namespace

}  // namespace a6l::sms
