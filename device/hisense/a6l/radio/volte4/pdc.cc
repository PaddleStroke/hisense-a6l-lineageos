// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): QMI PDC client (see pdc.h).
#include "pdc.h"

#include <chrono>
#include <cstring>

namespace a6l::volte4 {

using qmi::Message;
using qmi::Reader;

namespace pdc {
std::vector<uint8_t> typeWithId(uint32_t type, const std::vector<uint8_t>& id) {
    std::vector<uint8_t> v;
    for (int i = 0; i < 4; i++) v.push_back((type >> (8 * i)) & 0xff);
    v.push_back(static_cast<uint8_t>(id.size()));
    v.insert(v.end(), id.begin(), id.end());
    return v;
}
std::vector<uint8_t> loadChunk(uint32_t type, const std::vector<uint8_t>& id, uint32_t total, const uint8_t* chunk,
                               size_t len) {
    auto v = typeWithId(type, id);
    for (int i = 0; i < 4; i++) v.push_back((total >> (8 * i)) & 0xff);
    v.push_back(len & 0xff);
    v.push_back((len >> 8) & 0xff);
    v.insert(v.end(), chunk, chunk + len);
    return v;
}
}  // namespace pdc

std::string idHex(const std::vector<uint8_t>& id) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : id) { s += d[b >> 4]; s += d[b & 15]; }
    return s.empty() ? "(none)" : s;
}

Pdc::Pdc(qmi::Client& c, int timeoutMs) : mC(c), mTimeout(timeoutMs), mQ(std::make_shared<Q>()) {
    auto q = mQ;
    for (uint16_t id : {pdc::kGetSelected, pdc::kSetSelected, pdc::kListConfigs, pdc::kLoadConfig, pdc::kActivate,
                        pdc::kGetInfo, pdc::kDeactivate}) {
        mC.onIndication(pdc::kSvcPdc, id, [q](const Message& m) {
            std::lock_guard<std::mutex> l(q->m);
            q->q.push_back(m);
            q->cv.notify_all();
        });
    }
}

std::optional<Message> Pdc::waitInd(uint16_t msgId, uint32_t token, int timeoutMs) {
    std::unique_lock<std::mutex> l(mQ->m);
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        for (auto it = mQ->q.begin(); it != mQ->q.end(); ++it) {
            if (it->msgId != msgId) continue;
            Reader r(it->get(0x10));
            uint32_t t = r.u32();
            if (r.good() && t != token) continue;  // someone else's (stale) indication
            Message m = *it;
            mQ->q.erase(it);
            return m;
        }
        if (mQ->cv.wait_until(l, end) == std::cv_status::timeout) return std::nullopt;
    }
}

int Pdc::call(uint16_t msgId, Message req, Message* ind) {
    uint32_t token = ++mToken;
    req.u32(0x10, token);
    auto r = mC.request(pdc::kSvcPdc, req, mTimeout);
    if (!r.ok()) {
        mErr = "msg 0x" + std::to_string(msgId) + " response: " + r.describe();
        return r.status == qmi::Result::QmiFailure ? 0x10000 + r.qmiError : -1;
    }
    auto m = waitInd(msgId, token, mTimeout);
    if (!m) { mErr = "no indication for msg " + std::to_string(msgId); return -1; }
    Reader rr(m->get(0x01));
    int res = rr.u16();
    if (!rr.good()) { mErr = "indication without result TLV"; return -1; }
    if (ind) *ind = *m;
    if (res) mErr = "indication result " + std::to_string(res);
    return res;
}

bool Pdc::registerReporting() {
    Message m = Message::request(pdc::kRegister);
    m.u8(0x10, 1);
    return mC.request(pdc::kSvcPdc, m, mTimeout).ok();
}

int Pdc::getSelected(uint32_t type, PdcSelected* out) {
    Message req = Message::request(pdc::kGetSelected), ind;
    req.u32(0x01, type);
    int rc = call(pdc::kGetSelected, req, &ind);
    out->indResult = rc;
    if (rc == 0 || rc == 16 /* not provisioned / none */) {
        if (auto v = ind.get(0x11); v && !v->empty()) { Reader r(*v); out->active = r.bytes8(); if (!r.good()) out->active.reset(); }
        if (auto v = ind.get(0x12); v && !v->empty()) { Reader r(*v); out->pending = r.bytes8(); if (!r.good()) out->pending.reset(); }
    }
    return rc;
}

int Pdc::list(uint32_t type, std::vector<std::vector<uint8_t>>* ids) {
    Message req = Message::request(pdc::kListConfigs), ind;
    req.u32(0x11, type);
    int rc = call(pdc::kListConfigs, req, &ind);
    if (rc) return rc;
    ids->clear();
    if (auto v = ind.get(0x11)) {
        Reader r(*v);
        int n = r.u8();
        for (int i = 0; i < n && r.good(); i++) {
            r.u32();
            auto id = r.bytes8();
            if (r.good()) ids->push_back(id);
        }
        if (!r.good()) { mErr = "malformed config list"; return -1; }
    }
    return 0;
}

int Pdc::info(uint32_t type, const std::vector<uint8_t>& id, PdcInfo* out) {
    Message req = Message::request(pdc::kGetInfo), ind;
    req.raw(0x01, pdc::typeWithId(type, id));
    int rc = call(pdc::kGetInfo, req, &ind);
    out->id = id;
    if (rc) return rc;
    Reader a(ind.get(0x11));
    out->size = a.u32();
    if (auto v = ind.get(0x12)) { Reader b(*v); out->desc = b.str8(); }
    Reader c(ind.get(0x13));
    out->version = c.u32();
    out->ok = true;
    return 0;
}

int Pdc::setSelected(uint32_t type, const std::vector<uint8_t>& id) {
    Message req = Message::request(pdc::kSetSelected);
    req.raw(0x01, pdc::typeWithId(type, id));
    return call(pdc::kSetSelected, req, nullptr);
}

int Pdc::activate(uint32_t type) {
    Message req = Message::request(pdc::kActivate);
    req.u32(0x01, type);
    return call(pdc::kActivate, req, nullptr);
}

int Pdc::deactivate(uint32_t type) {
    Message req = Message::request(pdc::kDeactivate);
    req.u32(0x01, type);
    return call(pdc::kDeactivate, req, nullptr);
}

int Pdc::load(uint32_t type, const std::vector<uint8_t>& id, const std::vector<uint8_t>& data, size_t chunk,
              const std::function<void(uint32_t, uint32_t)>& progress) {
    if (chunk == 0 || chunk > 32768) chunk = 1024;
    if (data.empty() || id.empty() || id.size() > 124) { mErr = "bad load arguments"; return -1; }
    size_t off = 0;
    int guard = 0;
    while (off < data.size()) {
        size_t n = std::min(chunk, data.size() - off);
        Message req = Message::request(pdc::kLoadConfig), ind;
        req.raw(0x01, pdc::loadChunk(type, id, static_cast<uint32_t>(data.size()), data.data() + off, n));
        int rc = call(pdc::kLoadConfig, req, &ind);
        if (rc) return rc;
        Reader fr(ind.get(0x13));
        if (fr.good() && fr.u8()) { mErr = "modem asked for a frame reset at offset " + std::to_string(off); return -2; }
        Reader rv(ind.get(0x11)), rm(ind.get(0x12));
        uint32_t received = rv.u32(), remaining = rm.u32();
        off += n;
        if (progress) progress(rv.good() ? received : static_cast<uint32_t>(off), rm.good() ? remaining : static_cast<uint32_t>(data.size() - off));
        if (rm.good() && remaining == 0) break;
        if (rm.good() && rv.good() && received + remaining != data.size()) {
            mErr = "modem byte count mismatch received=" + std::to_string(received) + " remaining=" + std::to_string(remaining);
            return -3;
        }
        if (++guard > 40000) { mErr = "too many chunks"; return -1; }
    }
    if (off < data.size()) { mErr = "modem reported remaining=0 before the end of the file"; return -3; }
    return 0;
}

// ---- SHA-1 --------------------------------------------------------------------------------------------------
std::vector<uint8_t> sha1(const uint8_t* p, size_t n) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    std::vector<uint8_t> m(p, p + n);
    uint64_t bits = static_cast<uint64_t>(n) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; i--) m.push_back((bits >> (8 * i)) & 0xff);
    auto rol = [](uint32_t x, int s) { return (x << s) | (x >> (32 - s)); };
    for (size_t o = 0; o < m.size(); o += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = (m[o + 4 * i] << 24) | (m[o + 4 * i + 1] << 16) | (m[o + 4 * i + 2] << 8) | m[o + 4 * i + 3];
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::vector<uint8_t> out;
    for (uint32_t x : h) for (int i = 3; i >= 0; i--) out.push_back((x >> (8 * i)) & 0xff);
    return out;
}

// ---- MBN ----------------------------------------------------------------------------------------------------
MbnInfo parseMbn(const std::vector<uint8_t>& f) {
    MbnInfo r;
    auto u16 = [&](size_t o) -> uint32_t { return o + 2 <= f.size() ? f[o] | (f[o + 1] << 8) : 0; };
    auto u32 = [&](size_t o) -> uint32_t { return o + 4 <= f.size() ? f[o] | (f[o + 1] << 8) | (f[o + 2] << 16) | (static_cast<uint32_t>(f[o + 3]) << 24) : 0; };
    if (f.size() < 0x34 || memcmp(f.data(), "\x7f" "ELF", 4) || f[4] != 1) { r.err = "not an ELF32 MBN"; return r; }
    uint32_t phoff = u32(0x1c), phn = u16(0x2c);
    size_t seg = 0, segLen = 0;
    for (uint32_t i = 0; i < phn && i < 16; i++) {
        size_t ph = phoff + i * 32;
        if (ph + 32 > f.size()) break;
        uint32_t off = u32(ph + 4), fs = u32(ph + 16);
        if (off + 16 <= f.size() && fs >= 16 && off + fs <= f.size() && !memcmp(&f[off], "MCFG", 4)) { seg = off; segLen = fs; }
    }
    if (!segLen) { r.err = "no MCFG segment"; return r; }
    r.format = u16(seg + 4);
    r.type = u16(seg + 6);
    r.items = u32(seg + 8);
    const char trl[] = "MCFG_TRL";
    for (size_t i = seg + 16; i + 8 <= seg + segLen; i++) {
        if (memcmp(&f[i], trl, 8)) continue;
        size_t q = i + 8;
        while (q + 3 <= seg + segLen) {
            uint8_t sid = f[q];
            uint32_t sl = u16(q + 1);
            if (q + 3 + sl > seg + segLen) break;
            if (sid == 1 && sl == 4) r.version = u32(q + 3);
            if (sid == 3) r.name.assign(reinterpret_cast<const char*>(&f[q + 3]), sl);
            q += 3 + sl;
        }
        break;
    }
    r.ok = r.type <= 1 && r.items > 0 && !r.name.empty();
    if (!r.ok) r.err = "MCFG header/trailer not understood";
    return r;
}

}  // namespace a6l::volte4
