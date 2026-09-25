// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI codec implementation.
#include "qmi.h"

#include <cstring>

namespace a6l {
namespace qmi {

Writer& Writer::f32(float v) {
    uint32_t u;
    static_assert(sizeof(u) == sizeof(v), "float size");
    std::memcpy(&u, &v, 4);
    return u32(u);
}

Writer& Writer::f64(double v) {
    uint64_t u;
    static_assert(sizeof(u) == sizeof(v), "double size");
    std::memcpy(&u, &v, 8);
    return u64(u);
}

bool Reader::take(void* out, size_t n) {
    if (left() < n) return false;
    std::memcpy(out, p_ + off_, n);
    off_ += n;
    return true;
}

bool Reader::u8(uint8_t* v) { return take(v, 1); }

bool Reader::u16(uint16_t* v) {
    uint8_t b[2];
    if (!take(b, 2)) return false;
    *v = uint16_t(b[0] | (b[1] << 8));
    return true;
}

bool Reader::u32(uint32_t* v) {
    uint8_t b[4];
    if (!take(b, 4)) return false;
    *v = uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
    return true;
}

bool Reader::u64(uint64_t* v) {
    uint32_t lo, hi;
    if (left() < 8) return false;
    u32(&lo);
    u32(&hi);
    *v = (uint64_t(hi) << 32) | lo;
    return true;
}

bool Reader::f32(float* v) {
    uint32_t u;
    if (!u32(&u)) return false;
    std::memcpy(v, &u, 4);
    return true;
}

bool Reader::f64(double* v) {
    uint64_t u;
    if (!u64(&u)) return false;
    std::memcpy(v, &u, 8);
    return true;
}

const std::vector<uint8_t>* Message::find(uint8_t t) const {
    for (const auto& tlv : tlvs)
        if (tlv.type == t) return &tlv.value;
    return nullptr;
}

#define A6L_QMI_GETTER(NAME, TYPE, READ)                 \
    bool Message::NAME(uint8_t t, TYPE* v) const {       \
        const auto* b = find(t);                         \
        if (!b) return false;                            \
        Reader r(*b);                                    \
        return r.READ(v);                                \
    }
A6L_QMI_GETTER(getU8, uint8_t, u8)
A6L_QMI_GETTER(getU16, uint16_t, u16)
A6L_QMI_GETTER(getU32, uint32_t, u32)
A6L_QMI_GETTER(getU64, uint64_t, u64)
A6L_QMI_GETTER(getF32, float, f32)
A6L_QMI_GETTER(getF64, double, f64)
#undef A6L_QMI_GETTER

bool Message::getString(uint8_t t, std::string* v) const {
    const auto* b = find(t);
    if (!b) return false;
    size_t n = 0;
    while (n < b->size() && (*b)[n] != 0) n++;
    v->assign(reinterpret_cast<const char*>(b->data()), n);
    return true;
}

bool Message::getResult(uint16_t* result, uint16_t* error) const {
    const auto* b = find(kResultTlv);
    if (!b) return false;
    Reader r(*b);
    return r.u16(result) && r.u16(error);
}

std::vector<uint8_t> encode(const Message& m) {
    Writer w;
    size_t len = 0;
    for (const auto& t : m.tlvs) len += 3 + t.value.size();
    w.u8(m.type).u16(m.txn).u16(m.msgId).u16(uint16_t(len));
    for (const auto& t : m.tlvs) {
        w.u8(t.type).u16(uint16_t(t.value.size()));
        w.bytes(t.value.data(), t.value.size());
    }
    return w.b;
}

bool decode(const uint8_t* p, size_t n, Message* m, std::string* err) {
    Reader r(p, n);
    uint16_t len;
    if (!r.u8(&m->type) || !r.u16(&m->txn) || !r.u16(&m->msgId) || !r.u16(&len)) {
        if (err) *err = "short QMI header";
        return false;
    }
    if (r.left() < len) {
        if (err) *err = "QMI length exceeds packet";
        return false;
    }
    m->tlvs.clear();
    const uint8_t* body = p + 7;
    size_t off = 0;
    while (off < len) {
        if (len - off < 3) {
            if (err) *err = "truncated TLV header";
            return false;
        }
        uint8_t t = body[off];
        uint16_t l = uint16_t(body[off + 1] | (body[off + 2] << 8));
        off += 3;
        if (len - off < l) {
            if (err) *err = "TLV length exceeds message";
            return false;
        }
        m->tlvs.push_back({t, std::vector<uint8_t>(body + off, body + off + l)});
        off += l;
    }
    return true;
}

std::string hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

bool unhex(const std::string& s, std::vector<uint8_t>* out) {
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out->clear();
    int hi = -1;
    for (char c : s) {
        if (c == ' ' || c == ':' || c == '\t') continue;
        int v = nib(c);
        if (v < 0) return false;
        if (hi < 0) {
            hi = v;
        } else {
            out->push_back(uint8_t(hi << 4 | v));
            hi = -1;
        }
    }
    return hi < 0;
}

}  // namespace qmi
}  // namespace a6l
