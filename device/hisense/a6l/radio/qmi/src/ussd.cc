// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (telephony-flows, 29 Sep 2026): QMI VOICE USSD codec + requests (see a6lqmi/ussd.h).
#include <a6lqmi/log.h>
#include <a6lqmi/ussd.h>

namespace a6l::qmi::voice::ussd {

namespace {
void putUtf8(std::string* out, uint32_t cp) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Strict UTF-8 decoder (no overlongs, no surrogates, <= U+10FFFF). false on any error.
bool utf8ToCodepoints(const std::string& s, std::vector<uint32_t>* out) {
    size_t i = 0;
    while (i < s.size()) {
        uint8_t b = static_cast<uint8_t>(s[i]);
        uint32_t cp;
        int n;
        if (b < 0x80) { cp = b; n = 0; }
        else if ((b & 0xE0) == 0xC0) { cp = b & 0x1F; n = 1; }
        else if ((b & 0xF0) == 0xE0) { cp = b & 0x0F; n = 2; }
        else if ((b & 0xF8) == 0xF0) { cp = b & 0x07; n = 3; }
        else return false;
        if (i + static_cast<size_t>(n) >= s.size()) return false;  // truncated sequence
        for (int k = 1; k <= n; k++) {
            uint8_t c = static_cast<uint8_t>(s[i + k]);
            if ((c & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3F);
        }
        static const uint32_t kMin[] = {0, 0x80, 0x800, 0x10000};
        if (cp < kMin[n] || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return false;
        out->push_back(cp);
        i += n + 1;
    }
    return true;
}

// {u16 code units} -> UTF-8; lone surrogates -> false
bool utf16ToUtf8(const std::vector<uint16_t>& u, std::string* out) {
    for (size_t i = 0; i < u.size(); i++) {
        uint32_t c = u[i];
        if (c >= 0xD800 && c <= 0xDBFF) {
            if (i + 1 >= u.size() || u[i + 1] < 0xDC00 || u[i + 1] > 0xDFFF) return false;
            c = 0x10000 + ((c - 0xD800) << 10) + (u[i + 1] - 0xDC00);
            i++;
        } else if (c >= 0xDC00 && c <= 0xDFFF) {
            return false;
        }
        putUtf8(out, c);
    }
    return true;
}
}  // namespace

std::optional<Encoded> encode(const std::string& utf8) {
    if (utf8.empty()) return std::nullopt;
    std::vector<uint32_t> cps;
    if (!utf8ToCodepoints(utf8, &cps)) return std::nullopt;
    Encoded e;
    bool ascii = true;
    for (uint32_t cp : cps)
        if (cp == 0 || cp > 0x7F) ascii = false;
    if (ascii) {
        e.dcs = kDcsAscii;
        e.data.assign(utf8.begin(), utf8.end());
    } else {
        e.dcs = kDcsUcs2;
        for (uint32_t cp : cps) {
            if (cp > 0xFFFF || cp == 0) return std::nullopt;  // UCS2 = BMP only
            e.data.push_back(static_cast<uint8_t>(cp >> 8));
            e.data.push_back(static_cast<uint8_t>(cp & 0xFF));
        }
    }
    if (e.data.size() > kMaxUssBytes) return std::nullopt;
    return e;
}

Text decodeUssData(const std::vector<uint8_t>& tlv) {
    Text t;
    t.present = true;
    Reader r(tlv);
    uint8_t dcs = r.u8();
    uint8_t len = r.u8();
    auto data = r.bytes(len);
    if (!r.good() || !r.atEnd()) {
        t.decodeError = true;
        return t;
    }
    switch (dcs) {
        case kDcsAscii:
            for (uint8_t b : data) {
                if (b > 0x7F) {  // not ASCII: keep the text readable as Latin-1 rather than emit invalid UTF-8
                    putUtf8(&t.utf8, b);
                } else {
                    t.utf8.push_back(static_cast<char>(b));
                }
            }
            break;
        case kDcs8bit:
            for (uint8_t b : data) putUtf8(&t.utf8, b);
            break;
        case kDcsUcs2: {
            if (data.size() % 2) {
                t.decodeError = true;
                return t;
            }
            std::vector<uint16_t> u;
            for (size_t i = 0; i < data.size(); i += 2) u.push_back(static_cast<uint16_t>(data[i] << 8 | data[i + 1]));
            if (!utf16ToUtf8(u, &t.utf8)) {
                t.utf8.clear();
                t.decodeError = true;
            }
            break;
        }
        default:
            t.decodeError = true;
    }
    return t;
}

Text decodeUtf16(const std::vector<uint8_t>& tlv) {
    Text t;
    t.present = true;
    Reader r(tlv);
    uint8_t n = r.u8();
    std::vector<uint16_t> u;
    for (int i = 0; i < n && r.good(); i++) u.push_back(r.u16());
    if (!r.good() || !r.atEnd() || !utf16ToUtf8(u, &t.utf8)) {
        t.utf8.clear();
        t.decodeError = true;
    }
    return t;
}

Text textOf(const Message& m, uint8_t ussTlv, uint8_t utf16Tlv) {
    if (auto* v = m.get(utf16Tlv)) {
        Text t = decodeUtf16(*v);
        if (!t.decodeError) return t;
    }
    if (auto* v = m.get(ussTlv)) return decodeUssData(*v);
    if (m.has(utf16Tlv)) return decodeUtf16(*m.get(utf16Tlv));  // only a broken UTF-16 TLV: report the error
    return {};
}

std::optional<Event> parseIndication(const Message& ind) {
    Event e;
    switch (ind.msgId) {
        case kUssdInd: {
            e.kind = Event::Notification;
            auto* a = ind.get(0x01);
            if (!a || a->size() != 1) return std::nullopt;  // mandatory TLV
            e.userAction = (*a)[0];
            e.text = textOf(ind, 0x10, 0x11);
            return e;
        }
        case kReleaseInd:
            e.kind = Event::Released;
            return e;
        case kOriginateNoWait: {
            e.kind = Event::OriginateResult;
            if (auto* v = ind.get(0x10); v && v->size() == 2) e.error = static_cast<uint16_t>((*v)[0] | (*v)[1] << 8);
            if (auto* v = ind.get(0x11); v && v->size() == 2)
                e.failureCause = static_cast<uint16_t>((*v)[0] | (*v)[1] << 8);
            e.text = textOf(ind, 0x12, 0x14);
            return e;
        }
        default:
            return std::nullopt;
    }
}

Event fromOriginateResponse(const Result& r) {
    Event e;
    e.kind = Event::OriginateResult;
    if (!r.ok()) e.error = r.status == Result::QmiFailure ? r.qmiError : static_cast<uint16_t>(kErrInternal);
    if (auto* v = r.msg.get(0x10); v && v->size() == 2) e.failureCause = static_cast<uint16_t>((*v)[0] | (*v)[1] << 8);
    if (r.ok()) e.text = textOf(r.msg, 0x12, 0x16);
    return e;
}

Message buildUssRequest(uint16_t msgId, const Encoded& e) {
    std::vector<uint8_t> v{e.dcs, static_cast<uint8_t>(e.data.size())};
    v.insert(v.end(), e.data.begin(), e.data.end());
    return Message::request(msgId).raw(0x01, v);
}

Result originateNoWait(Client& c, const Encoded& e) { return c.request(kSvcVoice, buildUssRequest(kOriginateNoWait, e)); }
Result originate(Client& c, const Encoded& e, int timeoutMs) {
    return c.request(kSvcVoice, buildUssRequest(kOriginate, e), timeoutMs);
}
Result answer(Client& c, const Encoded& e) { return c.request(kSvcVoice, buildUssRequest(kAnswer, e), 10000); }
Result cancel(Client& c) { return c.request(kSvcVoice, Message::request(kCancel), 10000); }

bool isUnsupported(const Result& r) {
    return r.status == Result::QmiFailure &&
           (r.qmiError == kErrInvalidQmiCommand || r.qmiError == kErrNotSupported || r.qmiError == kErrDeviceUnsupported);
}

}  // namespace a6l::qmi::voice::ussd
