// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): QMI message codec.
#include <a6lqmi/message.h>

#include <cstdio>

namespace a6l::qmi {

namespace {
void put16(std::vector<uint8_t>& o, uint16_t v) {
    o.push_back(v & 0xff);
    o.push_back(v >> 8);
}
}  // namespace

std::string errorName(uint16_t err) {
    switch (err) {
        case kErrNone: return "NONE";
        case kErrMalformedMessage: return "MALFORMED_MESSAGE";
        case kErrNoMemory: return "NO_MEMORY";
        case kErrInternal: return "INTERNAL";
        case kErrInvalidHandle: return "INVALID_HANDLE";
        case kErrIncorrectPin: return "INCORRECT_PIN";
        case kErrNoNetworkFound: return "NO_NETWORK_FOUND";
        case kErrCallFailed: return "CALL_FAILED";
        case kErrOutOfCall: return "OUT_OF_CALL";
        case kErrMissingArgument: return "MISSING_ARGUMENT";
        case kErrDeviceUnsupported: return "DEVICE_UNSUPPORTED";
        case kErrNoEffect: return "NO_EFFECT";
        case kErrAuthenticationFailed: return "AUTHENTICATION_FAILED";
        case kErrPinBlocked: return "PIN_BLOCKED";
        case kErrPinAlwaysBlocked: return "PIN_ALWAYS_BLOCKED";
        case kErrUimUninitialized: return "UIM_UNINITIALIZED";
        case kErrInterfaceNotFound: return "INTERFACE_NOT_FOUND";
        case kErrGeneralError: return "GENERAL_ERROR";
        case kErrInvalidArgument: return "INVALID_ARGUMENT";
        case kErrDeviceNotReady: return "DEVICE_NOT_READY";
        case kErrNetworkNotReady: return "NETWORK_NOT_READY";
        case kErrWmsCauseCode: return "WMS_CAUSE_CODE";
        case kErrWmsMessageNotSent: return "WMS_MESSAGE_NOT_SENT";
        case kErrWmsMessageDeliveryFailure: return "WMS_MESSAGE_DELIVERY_FAILURE";
        case kErrInvalidTransition: return "INVALID_TRANSITION";
        case kErrInvalidOperation: return "INVALID_OPERATION";
        case kErrInvalidQmiCommand: return "INVALID_QMI_COMMAND";
        case kErrInformationUnavailable: return "INFORMATION_UNAVAILABLE";
        case kErrSimFileNotFound: return "SIM_FILE_NOT_FOUND";
        case kErrAccessDenied: return "ACCESS_DENIED";
        case kErrIncompatibleState: return "INCOMPATIBLE_STATE";
        case kErrFdnRestrict: return "FDN_RESTRICT";
        case kErrNoRadio: return "NO_RADIO";
        case kErrNotSupported: return "NOT_SUPPORTED";
        case kErrNoSubscription: return "NO_SUBSCRIPTION";
        case kErrNoSim: return "NO_SIM";
        default: {
            char b[16];
            snprintf(b, sizeof b, "ERR_%u", err);
            return b;
        }
    }
}

std::vector<uint8_t> Message::encode() const {
    std::vector<uint8_t> body;
    for (const auto& [t, v] : tlvs) {
        body.push_back(t);
        put16(body, static_cast<uint16_t>(v.size()));
        body.insert(body.end(), v.begin(), v.end());
    }
    std::vector<uint8_t> out;
    out.reserve(7 + body.size());
    out.push_back(static_cast<uint8_t>(type));
    put16(out, txn);
    put16(out, msgId);
    put16(out, static_cast<uint16_t>(body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<Message> Message::decode(const uint8_t* d, size_t n) {
    if (d == nullptr || n < 7) return std::nullopt;
    Message m;
    m.type = static_cast<MsgType>(d[0]);
    m.txn = d[1] | (d[2] << 8);
    m.msgId = d[3] | (d[4] << 8);
    size_t len = d[5] | (d[6] << 8);
    if (7 + len > n) return std::nullopt;  // trailing bytes beyond len are tolerated
    size_t pos = 7, end = 7 + len;
    while (pos < end) {
        if (end - pos < 3) return std::nullopt;
        uint8_t t = d[pos];
        size_t l = d[pos + 1] | (d[pos + 2] << 8);
        pos += 3;
        if (end - pos < l) return std::nullopt;
        m.tlvs[t] = std::vector<uint8_t>(d + pos, d + pos + l);
        pos += l;
    }
    return m;
}

Message& Message::u8(uint8_t t, uint8_t v) {
    tlvs[t] = {v};
    return *this;
}
Message& Message::u16(uint8_t t, uint16_t v) {
    tlvs[t] = {static_cast<uint8_t>(v), static_cast<uint8_t>(v >> 8)};
    return *this;
}
Message& Message::u32(uint8_t t, uint32_t v) {
    std::vector<uint8_t> b(4);
    for (int i = 0; i < 4; i++) b[i] = (v >> (8 * i)) & 0xff;
    tlvs[t] = b;
    return *this;
}
Message& Message::u64(uint8_t t, uint64_t v) {
    std::vector<uint8_t> b(8);
    for (int i = 0; i < 8; i++) b[i] = (v >> (8 * i)) & 0xff;
    tlvs[t] = b;
    return *this;
}
Message& Message::raw(uint8_t t, std::vector<uint8_t> v) {
    tlvs[t] = std::move(v);
    return *this;
}
Message& Message::str8(uint8_t t, const std::string& s) {
    std::vector<uint8_t> b;
    b.push_back(static_cast<uint8_t>(s.size() > 255 ? 255 : s.size()));
    b.insert(b.end(), s.begin(), s.begin() + b[0]);
    tlvs[t] = b;
    return *this;
}
Message& Message::strNoLen(uint8_t t, const std::string& s) {
    tlvs[t] = std::vector<uint8_t>(s.begin(), s.end());
    return *this;
}

const std::vector<uint8_t>* Message::get(uint8_t t) const {
    auto it = tlvs.find(t);
    return it == tlvs.end() ? nullptr : &it->second;
}

bool Message::hasResult() const {
    auto* r = get(kTlvResult);
    return r && r->size() >= 4;
}
bool Message::resultOk() const {
    auto* r = get(kTlvResult);
    if (!r || r->size() < 4) return false;
    return ((*r)[0] | ((*r)[1] << 8)) == 0;
}
uint16_t Message::error() const {
    auto* r = get(kTlvResult);
    if (!r || r->size() < 4) return kErrMalformedMessage;
    return (*r)[2] | ((*r)[3] << 8);
}

std::string Message::dump() const {
    char head[64];
    snprintf(head, sizeof head, "type=%u txn=%u msg=0x%04x", static_cast<unsigned>(type), txn,
             msgId);
    std::string s = head;
    for (const auto& [t, v] : tlvs) {
        char tb[16];
        snprintf(tb, sizeof tb, " [%02x]", t);
        s += tb;
        s += hex(v);
    }
    return s;
}

bool Reader::need(size_t n) {
    if (!mOk || mP == nullptr || mN - mPos < n) {
        mOk = false;
        return false;
    }
    return true;
}
uint8_t Reader::u8() {
    if (!need(1)) return 0;
    return mP[mPos++];
}
uint16_t Reader::u16() {
    if (!need(2)) return 0;
    uint16_t v = mP[mPos] | (mP[mPos + 1] << 8);
    mPos += 2;
    return v;
}
uint32_t Reader::u32() {
    if (!need(4)) return 0;
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(mP[mPos + i]) << (8 * i);
    mPos += 4;
    return v;
}
uint64_t Reader::u64() {
    if (!need(8)) return 0;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= static_cast<uint64_t>(mP[mPos + i]) << (8 * i);
    mPos += 8;
    return v;
}
std::vector<uint8_t> Reader::bytes(size_t n) {
    if (!need(n)) return {};
    std::vector<uint8_t> v(mP + mPos, mP + mPos + n);
    mPos += n;
    return v;
}
std::string Reader::str(size_t n) {
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(mP + mPos), n);
    mPos += n;
    return s;
}
void Reader::skip(size_t n) {
    if (need(n)) mPos += n;
}

std::string hex(const uint8_t* p, size_t n, char sep) {
    static const char* d = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        if (sep && i) s += sep;
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

std::optional<std::vector<uint8_t>> unhex(const std::string& s) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::vector<uint8_t> v;
    std::string t;
    for (char c : s)
        if (c != ':' && c != ' ') t += c;
    if (t.size() % 2) return std::nullopt;
    for (size_t i = 0; i < t.size(); i += 2) {
        int a = nib(t[i]), b = nib(t[i + 1]);
        if (a < 0 || b < 0) return std::nullopt;
        v.push_back(static_cast<uint8_t>(a << 4 | b));
    }
    return v;
}

std::string ipv4ToString(uint32_t v) {
    char b[20];
    snprintf(b, sizeof b, "%u.%u.%u.%u", (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff,
             v & 0xff);
    return b;
}

std::string ipv6ToString(const uint8_t* p) {
    // RFC 5952 compact form (longest run of >=2 zero groups collapsed)
    uint16_t g[8];
    for (int i = 0; i < 8; i++) g[i] = static_cast<uint16_t>(p[2 * i] << 8 | p[2 * i + 1]);
    int bestS = -1, bestL = 0;
    for (int i = 0; i < 8;) {
        if (g[i] == 0) {
            int j = i;
            while (j < 8 && g[j] == 0) j++;
            if (j - i > bestL && j - i >= 2) {
                bestS = i;
                bestL = j - i;
            }
            i = j;
        } else {
            i++;
        }
    }
    std::string s;
    char b[8];
    for (int i = 0; i < 8; i++) {
        if (i == bestS) {
            s += "::";
            i += bestL - 1;
            continue;
        }
        if (!s.empty() && s.back() != ':') s += ':';
        snprintf(b, sizeof b, "%x", g[i]);
        s += b;
    }
    if (s.empty()) s = "::";
    return s;
}

int maskToPrefix(uint32_t mask) {
    int n = 0;
    while (mask & 0x80000000u) {
        n++;
        mask <<= 1;
    }
    return n;
}

// GSM 03.38 default alphabet, basic table (0x00-0x7F) mapped to UTF-8.
static const char* const kGsm7[128] = {
        "@", "£", "$", "¥", "è", "é", "ù", "ì", "ò", "Ç", "\n", "Ø", "ø", "\r", "Å", "å",
        "Δ", "_", "Φ", "Γ", "Λ", "Ω", "Π", "Ψ", "Σ", "Θ", "Ξ", "\x1b", "Æ", "æ", "ß", "É",
        " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
        "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
        "¡", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
        "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "Ä", "Ö", "Ñ", "Ü", "§",
        "¿", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
        "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "ä", "ö", "ñ", "ü", "à"};

std::string gsm7Unpack(const uint8_t* p, size_t n, size_t septets) {
    if (septets == 0) septets = n * 8 / 7;
    std::string out;
    for (size_t i = 0; i < septets; i++) {
        size_t bit = i * 7;
        size_t byte = bit / 8, sh = bit % 8;
        if (byte >= n) break;
        unsigned v = p[byte] >> sh;
        if (sh > 1 && byte + 1 < n) v |= p[byte + 1] << (8 - sh);
        v &= 0x7f;
        out += kGsm7[v];
    }
    return out;
}

std::string decodeNetworkDescription(const std::string& raw) {
    bool printable = !raw.empty();
    for (unsigned char c : raw)
        if (c < 0x20 || c > 0x7e) printable = false;
    if (printable) return raw;
    if (raw.empty()) return raw;
    // Try GSM 7-bit packed; drop a trailing '@' produced by padding bits.
    std::string u = gsm7Unpack(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
    while (!u.empty() && (u.back() == '@' || u.back() == '\r')) u.pop_back();
    bool ok = !u.empty();
    for (unsigned char c : u)
        if (c < 0x20 && c != '\n') ok = false;
    return ok ? u : std::string();
}

}  // namespace a6l::qmi
