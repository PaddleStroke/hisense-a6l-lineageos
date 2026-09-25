// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): minimal QMI message codec for QMI-over-QRTR.
// Wire format (no QMUX header on QRTR): u8 type, u16 txn, u16 msg_id, u16 len, then TLVs (u8 type, u16 len, value).
// All integers little-endian. Plain C++17, no Android dependencies (shared by the HAL, a6l_gnss_test and host tests).
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace a6l {
namespace qmi {

enum MsgType : uint8_t { kRequest = 0, kResponse = 2, kIndication = 4 };

constexpr uint8_t kResultTlv = 0x02;   // standard response result TLV: u16 result, u16 error

struct Tlv {
    uint8_t type;
    std::vector<uint8_t> value;
};

// Little-endian builder helpers for TLV values.
struct Writer {
    std::vector<uint8_t> b;
    Writer& u8(uint8_t v) { b.push_back(v); return *this; }
    Writer& u16(uint16_t v) { for (int i = 0; i < 2; i++) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Writer& u32(uint32_t v) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Writer& u64(uint64_t v) { for (int i = 0; i < 8; i++) b.push_back(uint8_t(v >> (8 * i))); return *this; }
    Writer& f32(float v);
    Writer& f64(double v);
    Writer& bytes(const void* p, size_t n) {
        const uint8_t* c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
        return *this;
    }
};

// Bounds-checked little-endian reader over one TLV value.
class Reader {
  public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    explicit Reader(const std::vector<uint8_t>& v) : p_(v.data()), n_(v.size()) {}
    bool u8(uint8_t* v);
    bool u16(uint16_t* v);
    bool u32(uint32_t* v);
    bool u64(uint64_t* v);
    bool f32(float* v);
    bool f64(double* v);
    size_t left() const { return n_ - off_; }
    bool skip(size_t n) {
        if (left() < n) return false;
        off_ += n;
        return true;
    }

  private:
    bool take(void* out, size_t n);
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
};

struct Message {
    uint8_t type = kRequest;
    uint16_t txn = 0;
    uint16_t msgId = 0;
    std::vector<Tlv> tlvs;

    const std::vector<uint8_t>* find(uint8_t t) const;
    Message& add(uint8_t t, std::vector<uint8_t> v) {
        tlvs.push_back({t, std::move(v)});
        return *this;
    }
    Message& add(uint8_t t, const Writer& w) { return add(t, w.b); }

    // Typed getters: return false when the TLV is absent or too short.
    bool getU8(uint8_t t, uint8_t* v) const;
    bool getU16(uint8_t t, uint16_t* v) const;
    bool getU32(uint8_t t, uint32_t* v) const;
    bool getU64(uint8_t t, uint64_t* v) const;
    bool getF32(uint8_t t, float* v) const;
    bool getF64(uint8_t t, double* v) const;
    bool getString(uint8_t t, std::string* v) const;   // raw bytes up to the first NUL
    // Standard result TLV (0x02). Returns false if absent.
    bool getResult(uint16_t* result, uint16_t* error) const;
};

std::vector<uint8_t> encode(const Message& m);
// Returns false (and sets *err) on a malformed buffer. Trailing bytes beyond msg_len are ignored.
bool decode(const uint8_t* p, size_t n, Message* m, std::string* err);

std::string hex(const uint8_t* p, size_t n);
bool unhex(const std::string& s, std::vector<uint8_t>* out);

}  // namespace qmi
}  // namespace a6l
