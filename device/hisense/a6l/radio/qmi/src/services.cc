// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): typed QMI requests/parsers.
#include <a6lqmi/log.h>
#include <a6lqmi/services.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

namespace a6l::qmi {

namespace {
Message req(uint16_t id) { return Message::request(id); }

std::vector<uint8_t> sessionTlv(const uim::Session& s) {
    std::vector<uint8_t> v{s.type, static_cast<uint8_t>(s.aid.size())};
    v.insert(v.end(), s.aid.begin(), s.aid.end());
    return v;
}
std::vector<uint8_t> fileTlv(const uim::FilePath& f) {
    std::vector<uint8_t> v{static_cast<uint8_t>(f.fileId & 0xff), static_cast<uint8_t>(f.fileId >> 8),
                           static_cast<uint8_t>(f.path.size() * 2)};
    for (uint16_t p : f.path) {
        v.push_back(p & 0xff);
        v.push_back(p >> 8);
    }
    return v;
}
void cardResult(const Message& m, uint8_t tlv, uint8_t* sw1, uint8_t* sw2, bool* has) {
    if (auto* t = m.get(tlv); t && t->size() >= 2) {
        *sw1 = (*t)[0];
        *sw2 = (*t)[1];
        *has = true;
    }
}
std::string strTlv(const Message& m, uint8_t t) {
    auto* v = m.get(t);
    return v ? std::string(v->begin(), v->end()) : std::string();
}
}  // namespace

// =============================================================== DMS
namespace dms {
std::optional<Ids> parseIds(const Message& m) {
    Ids ids;
    ids.esn = strTlv(m, 0x10);
    ids.imei = strTlv(m, 0x11);
    ids.meid = strTlv(m, 0x12);
    ids.imeisv = strTlv(m, 0x13);
    return ids;
}
Result getIds(Client& c, Ids* out) {
    auto r = c.request(kSvcDms, req(kGetIds));
    if (r.ok()) *out = *parseIds(r.msg);
    return r;
}
Result getRevision(Client& c, std::string* out) {
    auto r = c.request(kSvcDms, req(kGetRevision));
    if (r.ok()) *out = strTlv(r.msg, 0x01);
    return r;
}
Result getMsisdn(Client& c, std::string* out) {
    auto r = c.request(kSvcDms, req(kGetMsisdn));
    if (r.ok()) *out = strTlv(r.msg, 0x01);
    return r;
}
Result getOperatingMode(Client& c, uint8_t* mode) {
    auto r = c.request(kSvcDms, req(kGetOperatingMode));
    if (r.ok()) {
        Reader rd(r.msg.get(0x01));
        *mode = rd.u8();
        if (!rd.good()) *mode = kUnknownMode;
    }
    return r;
}
Result setOperatingMode(Client& c, uint8_t mode) {
    return c.request(kSvcDms, req(kSetOperatingMode).u8(0x01, mode), 15000);
}
Result uimGetIccid(Client& c, std::string* out) {
    auto r = c.request(kSvcDms, req(kUimGetIccid));
    if (r.ok()) *out = strTlv(r.msg, 0x01);
    return r;
}
Result uimGetImsi(Client& c, std::string* out) {
    auto r = c.request(kSvcDms, req(kUimGetImsi));
    if (r.ok()) *out = strTlv(r.msg, 0x01);
    return r;
}
}  // namespace dms

// =============================================================== UIM
namespace uim {
const Card* CardStatus::primaryCard() const {
    if (indexGwPrimary == 0xFFFF) return cards.empty() ? nullptr : &cards[0];
    unsigned ci = indexGwPrimary >> 8;
    return ci < cards.size() ? &cards[ci] : nullptr;
}
const App* CardStatus::primaryGwApp() const {
    if (indexGwPrimary == 0xFFFF) return nullptr;
    unsigned ci = indexGwPrimary >> 8, ai = indexGwPrimary & 0xff;
    if (ci >= cards.size() || ai >= cards[ci].apps.size()) return nullptr;
    return &cards[ci].apps[ai];
}

std::optional<CardStatus> parseCardStatus(const std::vector<uint8_t>& tlv) {
    Reader r(tlv);
    CardStatus s;
    s.indexGwPrimary = r.u16();
    s.index1xPrimary = r.u16();
    s.indexGwSecondary = r.u16();
    s.index1xSecondary = r.u16();
    uint8_t nc = r.u8();
    for (unsigned i = 0; i < nc && r.good(); i++) {
        Card c;
        c.state = r.u8();
        c.upinState = r.u8();
        c.upinRetries = r.u8();
        c.upukRetries = r.u8();
        c.error = r.u8();
        uint8_t na = r.u8();
        for (unsigned j = 0; j < na && r.good(); j++) {
            App a;
            a.type = r.u8();
            a.state = r.u8();
            a.persoState = r.u8();
            a.persoFeature = r.u8();
            a.persoRetries = r.u8();
            a.persoUnblockRetries = r.u8();
            a.aid = r.bytes8();
            a.upinReplacesPin1 = r.u8() != 0;
            a.pin1State = r.u8();
            a.pin1Retries = r.u8();
            a.puk1Retries = r.u8();
            a.pin2State = r.u8();
            a.pin2Retries = r.u8();
            a.puk2Retries = r.u8();
            c.apps.push_back(a);
        }
        s.cards.push_back(c);
    }
    if (!r.good()) return std::nullopt;
    return s;
}

Result getCardStatus(Client& c, CardStatus* out) {
    auto r = c.request(kSvcUim, req(kGetCardStatus));
    if (r.ok()) {
        auto* t = r.msg.get(0x10);
        auto cs = t ? parseCardStatus(*t) : std::nullopt;
        if (!cs) {
            r.status = Result::QmiFailure;
            r.qmiError = kErrMalformedMessage;
        } else {
            *out = *cs;
        }
    }
    return r;
}

Result registerEvents(Client& c, uint32_t mask) {
    return c.request(kSvcUim, req(kRegisterEvents).u32(0x01, mask));
}

Message buildReadTransparent(const Session& s, const FilePath& f, uint16_t offset, uint16_t len) {
    Message m = req(kReadTransparent);
    m.raw(0x01, sessionTlv(s));
    m.raw(0x02, fileTlv(f));
    m.raw(0x03, {static_cast<uint8_t>(offset & 0xff), static_cast<uint8_t>(offset >> 8),
                 static_cast<uint8_t>(len & 0xff), static_cast<uint8_t>(len >> 8)});
    return m;
}

static void parseIo(const Result& r, IoResult* out) {
    cardResult(r.msg, 0x10, &out->sw1, &out->sw2, &out->hasSw);
    if (auto* t = r.msg.get(0x11)) {
        Reader rd(*t);
        out->data = rd.bytes16();
    }
}

Result readTransparent(Client& c, const Session& s, const FilePath& f, uint16_t offset,
                       uint16_t len, IoResult* out) {
    auto r = c.request(kSvcUim, buildReadTransparent(s, f, offset, len), 10000);
    parseIo(r, out);  // card result is present on failures too (e.g. 6A82)
    return r;
}

Result readRecord(Client& c, const Session& s, const FilePath& f, uint16_t record, uint16_t len,
                  IoResult* out) {
    Message m = req(kReadRecord);
    m.raw(0x01, sessionTlv(s));
    m.raw(0x02, fileTlv(f));
    m.raw(0x03, {static_cast<uint8_t>(record & 0xff), static_cast<uint8_t>(record >> 8),
                 static_cast<uint8_t>(len & 0xff), static_cast<uint8_t>(len >> 8)});
    auto r = c.request(kSvcUim, m, 10000);
    parseIo(r, out);
    return r;
}

Result writeTransparent(Client& c, const Session& s, const FilePath& f, uint16_t offset,
                        const std::vector<uint8_t>& data, IoResult* out) {
    Message m = req(kWriteTransparent);
    m.raw(0x01, sessionTlv(s));
    m.raw(0x02, fileTlv(f));
    std::vector<uint8_t> w{static_cast<uint8_t>(offset & 0xff), static_cast<uint8_t>(offset >> 8),
                           static_cast<uint8_t>(data.size() & 0xff),
                           static_cast<uint8_t>(data.size() >> 8)};
    w.insert(w.end(), data.begin(), data.end());
    m.raw(0x03, w);
    auto r = c.request(kSvcUim, m, 10000);
    cardResult(r.msg, 0x10, &out->sw1, &out->sw2, &out->hasSw);
    return r;
}

Result writeRecord(Client& c, const Session& s, const FilePath& f, uint16_t record,
                   const std::vector<uint8_t>& data, IoResult* out) {
    Message m = req(kWriteRecord);
    m.raw(0x01, sessionTlv(s));
    m.raw(0x02, fileTlv(f));
    std::vector<uint8_t> w{static_cast<uint8_t>(record & 0xff), static_cast<uint8_t>(record >> 8),
                           static_cast<uint8_t>(data.size() & 0xff),
                           static_cast<uint8_t>(data.size() >> 8)};
    w.insert(w.end(), data.begin(), data.end());
    m.raw(0x03, w);
    auto r = c.request(kSvcUim, m, 10000);
    cardResult(r.msg, 0x10, &out->sw1, &out->sw2, &out->hasSw);
    return r;
}

std::optional<FileAttributes> parseFileAttributes(const Message& m) {
    FileAttributes a;
    cardResult(m, 0x10, &a.sw1, &a.sw2, &a.hasSw);
    auto* t = m.get(0x11);
    if (!t) return std::nullopt;
    Reader r(*t);
    a.fileSize = r.u16();
    a.fileId = r.u16();
    a.fileType = r.u8();
    a.recordSize = r.u16();
    a.recordCount = r.u16();
    for (int i = 0; i < 5; i++) {  // read/write/increase/deactivate/activate security attrs
        r.u8();
        r.u16();
    }
    a.raw = r.bytes16();
    if (!r.good()) return std::nullopt;
    return a;
}

Result getFileAttributes(Client& c, const Session& s, const FilePath& f, FileAttributes* out) {
    Message m = req(kGetFileAttributes);
    m.raw(0x01, sessionTlv(s));
    m.raw(0x02, fileTlv(f));
    auto r = c.request(kSvcUim, m, 10000);
    if (auto a = parseFileAttributes(r.msg)) {
        *out = *a;
    } else {
        cardResult(r.msg, 0x10, &out->sw1, &out->sw2, &out->hasSw);
        if (r.ok()) {
            r.status = Result::QmiFailure;
            r.qmiError = kErrMalformedMessage;
        }
    }
    return r;
}

std::vector<uint8_t> toGsmGetResponse(const FileAttributes& a) {
    std::vector<uint8_t> g(15, 0);
    g[2] = a.fileSize >> 8;
    g[3] = a.fileSize & 0xff;
    g[4] = a.fileId >> 8;
    g[5] = a.fileId & 0xff;
    switch (a.fileType) {
        case kFileMf: g[6] = 0x01; break;
        case kFileDf: g[6] = 0x02; break;
        default: g[6] = 0x04; break;  // EF
    }
    // access conditions unknown here: report "always" for read (0x00) to not scare the framework
    g[8] = 0x00;
    g[9] = 0x00;
    g[10] = 0x00;
    g[11] = 0x01;  // file status: not invalidated
    g[12] = 0x02;  // length of following data
    switch (a.fileType) {
        case kFileLinearFixed: g[13] = 0x01; break;
        case kFileCyclic: g[13] = 0x03; break;
        default: g[13] = 0x00; break;
    }
    g[14] = static_cast<uint8_t>(a.recordSize);
    return g;
}

static void parseRetries(const Message& m, PinResult* out) {
    if (auto* t = m.get(0x10); t && t->size() >= 2) {
        out->verifyLeft = (*t)[0];
        out->unblockLeft = (*t)[1];
    }
}

Result verifyPin(Client& c, const Session& s, uint8_t pinId, const std::string& pin, PinResult* out) {
    std::vector<uint8_t> info{pinId, static_cast<uint8_t>(pin.size())};
    info.insert(info.end(), pin.begin(), pin.end());
    auto r = c.request(kSvcUim, req(kVerifyPin).raw(0x01, sessionTlv(s)).raw(0x02, info), 10000);
    parseRetries(r.msg, out);
    return r;
}
Result unblockPin(Client& c, const Session& s, uint8_t pinId, const std::string& puk,
                  const std::string& newPin, PinResult* out) {
    std::vector<uint8_t> info{pinId, static_cast<uint8_t>(puk.size())};
    info.insert(info.end(), puk.begin(), puk.end());
    info.push_back(static_cast<uint8_t>(newPin.size()));
    info.insert(info.end(), newPin.begin(), newPin.end());
    auto r = c.request(kSvcUim, req(kUnblockPin).raw(0x01, sessionTlv(s)).raw(0x02, info), 10000);
    parseRetries(r.msg, out);
    return r;
}
Result changePin(Client& c, const Session& s, uint8_t pinId, const std::string& oldPin,
                 const std::string& newPin, PinResult* out) {
    std::vector<uint8_t> info{pinId, static_cast<uint8_t>(oldPin.size())};
    info.insert(info.end(), oldPin.begin(), oldPin.end());
    info.push_back(static_cast<uint8_t>(newPin.size()));
    info.insert(info.end(), newPin.begin(), newPin.end());
    auto r = c.request(kSvcUim, req(kChangePin).raw(0x01, sessionTlv(s)).raw(0x02, info), 10000);
    parseRetries(r.msg, out);
    return r;
}
Result setPinProtection(Client& c, const Session& s, uint8_t pinId, bool enable,
                        const std::string& pin, PinResult* out) {
    std::vector<uint8_t> info{pinId, static_cast<uint8_t>(enable ? 1 : 0),
                              static_cast<uint8_t>(pin.size())};
    info.insert(info.end(), pin.begin(), pin.end());
    auto r = c.request(kSvcUim, req(kSetPinProtection).raw(0x01, sessionTlv(s)).raw(0x02, info),
                       10000);
    parseRetries(r.msg, out);
    return r;
}

std::string decodeIccid(const std::vector<uint8_t>& ef) {
    std::string s;
    for (uint8_t b : ef) {
        uint8_t lo = b & 0x0f, hi = b >> 4;
        if (lo <= 9) s += static_cast<char>('0' + lo); else if (lo != 0xf) s += static_cast<char>('A' + lo - 10);
        if (hi <= 9) s += static_cast<char>('0' + hi); else if (hi != 0xf) s += static_cast<char>('A' + hi - 10);
    }
    return s;
}

std::string decodeImsi(const std::vector<uint8_t>& ef) {
    if (ef.size() < 2) return {};
    size_t len = ef[0];
    if (len == 0 || len + 1 > ef.size()) len = ef.size() - 1;
    std::string s;
    for (size_t i = 1; i <= len; i++) {
        uint8_t lo = ef[i] & 0x0f, hi = ef[i] >> 4;
        if (i != 1 && lo <= 9) s += static_cast<char>('0' + lo);  // first low nibble = parity
        if (hi <= 9) s += static_cast<char>('0' + hi);
    }
    return s;
}

Result readIccid(Client& c, std::string* out) { return readIccid(c, out, kSessionCardSlot1, true); }

Result readIccid(Client& c, std::string* out, uint8_t cardSession, bool dmsFallback) {
    IoResult io;
    Session s{cardSession, {}};
    auto r = readTransparent(c, s, FilePath{0x2FE2, {0x3F00}}, 0, 0, &io);
    if (r.ok() && !io.data.empty()) {
        *out = decodeIccid(io.data);
        return r;
    }
    if (!dmsFallback) return r;
    ALOGW_Q("UIM EF_ICCID read failed (%s), trying DMS", r.describe().c_str());
    return dms::uimGetIccid(c, out);
}

Result readImsi(Client& c, const std::vector<uint8_t>& aid, std::string* out) {
    return readImsi(c, kSessionPrimaryGw, aid, out, true);
}

Result readImsi(Client& c, uint8_t provSession, const std::vector<uint8_t>& aid, std::string* out,
                bool dmsFallback) {
    IoResult io;
    Session s{provSession, aid};
    // EF_IMSI lives under ADF.USIM (7FFF) on a USIM and under DF.GSM (7F20) on a 2G SIM.
    auto r = readTransparent(c, s, FilePath{0x6F07, {0x3F00, 0x7FFF}}, 0, 0, &io);
    if (!(r.ok() && io.data.size() >= 2)) {
        r = readTransparent(c, s, FilePath{0x6F07, {0x3F00, 0x7F20}}, 0, 0, &io);
    }
    if (r.ok() && io.data.size() >= 2) {
        *out = decodeImsi(io.data);
        return r;
    }
    if (!dmsFallback) return r;
    ALOGW_Q("UIM EF_IMSI read failed (%s), trying DMS", r.describe().c_str());
    return dms::uimGetImsi(c, out);
}
}  // namespace uim

// =============================================================== NAS
namespace nas {
static std::string digits(uint16_t v, bool three) {
    char b[8];
    snprintf(b, sizeof b, three ? "%03u" : "%02u", v);
    return b;
}
const char* radioIfName(int8_t r) {
    switch (r) {
        case kRifNone: return "none";
        case kRifCdma1x: return "1x";
        case kRifEvdo: return "evdo";
        case kRifGsm: return "gsm";
        case kRifUmts: return "umts";
        case kRifLte: return "lte";
        case kRifTdscdma: return "tdscdma";
        case kRif5gnr: return "nr";
        default: return "other";
    }
}
std::string radioIfList(const ServingSystem& s) {
    if (s.radioIfs.empty()) return "none";
    std::string o;
    for (auto r : s.radioIfs) o += std::string(o.empty() ? "" : "+") + radioIfName(r);
    return o;
}
std::string ServingSystem::mccStr() const { return digits(mcc, true); }
std::string ServingSystem::mncStr() const { return digits(mnc, mnc3Digits || mnc > 99); }

std::optional<ServingSystem> parseServingSystem(const Message& m, bool ind) {
    ServingSystem s;
    auto* t = m.get(0x01);
    if (!t) return std::nullopt;
    Reader r(*t);
    s.regState = r.u8();
    s.csAttach = r.u8();
    s.psAttach = r.u8();
    s.selectedNetwork = r.u8();
    uint8_t n = r.u8();
    for (unsigned i = 0; i < n; i++) s.radioIfs.push_back(r.i8());
    if (!r.good()) return std::nullopt;
    if (auto* v = m.get(0x10); v && !v->empty()) s.roaming = ((*v)[0] == 0);
    if (auto* v = m.get(0x11)) {
        Reader rd(*v);
        uint8_t k = rd.u8();
        for (unsigned i = 0; i < k && rd.good(); i++) s.dataCaps.push_back(rd.u8());
    }
    if (auto* v = m.get(0x12)) {
        Reader rd(*v);
        s.mcc = rd.u16();
        s.mnc = rd.u16();
        std::string d = rd.str8();
        if (rd.good()) {
            s.hasPlmn = true;
            s.description = decodeNetworkDescription(d);
        }
    }
    const uint8_t tLac = ind ? 0x1D : 0x1C, tCid = ind ? 0x1E : 0x1D, tTac = ind ? 0x25 : 0x24,
                  tPcs = ind ? 0x29 : 0x27;
    if (auto* v = m.get(tLac); v && v->size() >= 2) s.lac = Reader(*v).u16();
    if (auto* v = m.get(tCid); v && v->size() >= 4) s.cid = Reader(*v).u32();
    if (auto* v = m.get(tTac); v && v->size() >= 2) s.tac = Reader(*v).u16();
    if (auto* v = m.get(tPcs); v && v->size() >= 5) {
        Reader rd(*v);
        uint16_t mcc = rd.u16(), mnc = rd.u16();
        uint8_t pcs = rd.u8();
        if (rd.good() && mcc == s.mcc && mnc == s.mnc) s.mnc3Digits = pcs != 0;
    }
    return s;
}

Result getServingSystem(Client& c, ServingSystem* out) {
    auto r = c.request(kSvcNas, req(kGetServingSystem));
    if (r.ok()) {
        auto s = parseServingSystem(r.msg, false);
        if (s) *out = *s;
        else {
            r.status = Result::QmiFailure;
            r.qmiError = kErrMalformedMessage;
        }
    }
    return r;
}

SignalInfo parseSignalInfo(const Message& m) {
    SignalInfo s;
    if (auto* v = m.get(0x12); v && !v->empty()) s.gsmRssi = static_cast<int8_t>((*v)[0]);
    if (auto* v = m.get(0x13); v && v->size() >= 3) {
        Reader r(*v);
        s.wcdmaRssi = r.i8();
        s.wcdmaEcio = r.i16();
    }
    if (auto* v = m.get(0x14); v && v->size() >= 6) {
        Reader r(*v);
        s.lteRssi = r.i8();
        s.lteRsrq = r.i8();
        s.lteRsrp = r.i16();
        s.lteSnr = r.i16();
        s.hasLte = r.good();
    }
    if (auto* v = m.get(0x19); v && v->size() >= 2) s.wcdmaRscp = Reader(*v).i16();
    return s;
}

Result getSignalInfo(Client& c, SignalInfo* out) {
    auto r = c.request(kSvcNas, req(kGetSignalInfo));
    if (r.ok()) *out = parseSignalInfo(r.msg);
    return r;
}

Result configSignalInfo(Client& c) {
    Message m = req(kConfigSignalInfo);
    std::vector<int8_t> rssi{-110, -103, -97, -89, -80, -70};
    std::vector<uint8_t> v{static_cast<uint8_t>(rssi.size())};
    for (auto x : rssi) v.push_back(static_cast<uint8_t>(x));
    m.raw(0x10, v);
    std::vector<int16_t> rsrp{-125, -115, -105, -95, -85};
    std::vector<uint8_t> w{static_cast<uint8_t>(rsrp.size())};
    for (auto x : rsrp) {
        w.push_back(static_cast<uint16_t>(x) & 0xff);
        w.push_back(static_cast<uint16_t>(x) >> 8);
    }
    m.raw(0x16, w);
    return c.request(kSvcNas, m);
}

Result registerIndications(Client& c) {
    Message m = req(kRegisterIndications);
    m.u8(0x13, 1);  // serving system
    m.u8(0x17, 1);  // network time
    m.u8(0x18, 1);  // system info
    m.u8(0x19, 1);  // signal info
    auto r = c.request(kSvcNas, m);
    if (!r.ok()) {  // older NAS may reject some TLVs: retry with the basics
        Message b = req(kRegisterIndications);
        b.u8(0x13, 1);
        b.u8(0x19, 1);
        r = c.request(kSvcNas, b);
    }
    return r;
}

OperatorName parseOperatorName(const Message& m) {
    OperatorName o;
    if (auto* v = m.get(0x10)) {
        Reader r(*v);
        r.u8();
        o.spn = r.str8();
    }
    if (auto* v = m.get(0x13)) o.operatorString = std::string(v->begin(), v->end());
    auto names = [&](Reader& r) {
        uint8_t enc = r.u8();
        r.u8();
        r.u8();
        r.u8();
        auto ln = r.bytes8();
        auto sn = r.bytes8();
        auto conv = [&](const std::vector<uint8_t>& b) -> std::string {
            if (enc == 1) {  // UCS2LE -> ASCII subset / UTF-8
                std::string s;
                for (size_t i = 0; i + 1 < b.size(); i += 2) {
                    unsigned cp = b[i] | (b[i + 1] << 8);
                    if (cp < 0x80) s += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        s += static_cast<char>(0xC0 | (cp >> 6));
                        s += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        s += static_cast<char>(0xE0 | (cp >> 12));
                        s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        s += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                }
                return s;
            }
            return gsm7Unpack(b.data(), b.size());
        };
        return std::make_pair(conv(ln), conv(sn));
    };
    if (auto* v = m.get(0x14)) {  // NITZ
        Reader r(*v);
        auto [l, s] = names(r);
        if (r.good()) {
            o.longName = l;
            o.shortName = s;
        }
    }
    if (o.longName.empty()) {
        if (auto* v = m.get(0x12)) {  // first PLMN name record
            Reader r(*v);
            if (r.u8() > 0) {
                auto [l, s] = names(r);
                if (r.good()) {
                    o.longName = l;
                    o.shortName = s;
                }
            }
        }
    }
    return o;
}

Result getOperatorName(Client& c, OperatorName* out) {
    auto r = c.request(kSvcNas, req(kGetOperatorName));
    if (r.ok()) *out = parseOperatorName(r.msg);
    return r;
}

Result getLteCell(Client& c, LteCell* out) {
    auto r = c.request(kSvcNas, req(kGetCellLocationInfo));
    if (!r.ok()) return r;
    if (auto* v = r.msg.get(0x13)) {
        Reader rd(*v);
        rd.u8();  // ue in idle
        out->plmn = rd.bytes(3);
        out->tac = rd.u16();
        out->globalCellId = rd.u32();
        out->earfcn = rd.u16();
        out->pci = rd.u16();
        rd.u8();
        rd.u8();
        rd.u8();
        rd.u8();
        uint8_t n = rd.u8();
        for (unsigned i = 0; i < n && rd.good(); i++) {
            uint16_t pci = rd.u16();
            int16_t rsrq = rd.i16(), rsrp = rd.i16(), rssi = rd.i16();
            rd.i16();
            if (pci == out->pci) {
                out->rsrq = rsrq;
                out->rsrp = rsrp;
                out->rssi = rssi;
            }
        }
        out->valid = rd.good() || out->earfcn != 0;
    }
    if (auto* v = r.msg.get(0x1E); v && v->size() >= 4) out->timingAdvance = Reader(*v).u32();
    return r;
}

Result setModePreference(Client& c, uint16_t modeMask) {
    return c.request(kSvcNas, req(kSetSystemSelectionPreference).u16(0x11, modeMask).u8(0x17, 1));
}

Result getModePreference(Client& c, uint16_t* modeMask) {
    auto r = c.request(kSvcNas, req(kGetSystemSelectionPreference));
    if (r.ok()) {
        if (auto* v = r.msg.get(0x11); v && v->size() >= 2) *modeMask = Reader(*v).u16();
    }
    return r;
}

Result setNetworkSelection(Client& c, bool manual, uint16_t mcc, uint16_t mnc, int8_t rat, bool mncThreeDigits) {
    Message m = req(kInitiateNetworkRegister);
    m.u8(0x01, manual ? 2 : 1);
    if (manual) {
        m.raw(0x10, {static_cast<uint8_t>(mcc & 0xff), static_cast<uint8_t>(mcc >> 8),
                     static_cast<uint8_t>(mnc & 0xff), static_cast<uint8_t>(mnc >> 8),
                     static_cast<uint8_t>(rat)});
        m.u8(0x11, 1);  // permanent
        m.u8(0x12, mncThreeDigits ? 1 : 0);  // r5 round5 F46: MNC PCS digit include status
    }
    return c.request(kSvcNas, m, 30000);
}

Result networkScan(Client& c, std::vector<ScanEntry>* out, int timeoutMs) {
    auto r = c.request(kSvcNas, req(kNetworkScan), timeoutMs);
    if (!r.ok()) return r;
    if (auto* v = r.msg.get(0x10)) {
        Reader rd(*v);
        uint16_t n = rd.u16();
        for (unsigned i = 0; i < n && rd.good(); i++) {
            ScanEntry e;
            e.mcc = rd.u16();
            e.mnc = rd.u16();
            e.status = rd.u8();
            e.description = decodeNetworkDescription(rd.str8());
            out->push_back(e);
        }
    }
    if (auto* v = r.msg.get(0x11)) {
        Reader rd(*v);
        uint16_t n = rd.u16();
        for (unsigned i = 0; i < n && rd.good(); i++) {
            uint16_t mcc = rd.u16(), mnc = rd.u16();
            int8_t rat = rd.i8();
            for (auto& e : *out)
                if (e.mcc == mcc && e.mnc == mnc && e.rat == 0) {
                    e.rat = rat;
                    break;
                }
        }
    }
    if (auto* v = r.msg.get(0x12)) {
        Reader rd(*v);
        uint16_t n = rd.u16();
        for (unsigned i = 0; i < n && rd.good(); i++) {
            uint16_t mcc = rd.u16(), mnc = rd.u16();
            bool pcs = rd.u8() != 0;
            for (auto& e : *out)
                if (e.mcc == mcc && e.mnc == mnc) e.mnc3Digits = pcs;
        }
    }
    return r;
}

// volte3: subscription info / VSIDs (see services.h)
SubscriptionInfo parseSubscriptionInfo(const Message& m) {
    SubscriptionInfo s;
    auto u8o = [&](uint8_t t, std::optional<uint8_t>* o) {
        if (auto* v = m.get(t); v && !v->empty()) *o = (*v)[0];
    };
    auto u32o = [&](uint8_t t, std::optional<uint32_t>* o) {
        if (auto* v = m.get(t); v && v->size() >= 4) {
            Reader r(*v);
            *o = r.u32();
        }
    };
    u8o(0x10, &s.priority);
    u8o(0x11, &s.active);
    u8o(0x12, &s.defaultData);
    u32o(0x13, &s.voiceVsid);
    u32o(0x14, &s.lteVoiceVsid);
    u32o(0x15, &s.wlanVoiceVsid);
    return s;
}
std::string SubscriptionInfo::summary() const {
    char b[256];
    auto vs = [](const std::optional<uint32_t>& v, char* o, size_t n) {
        if (v) snprintf(o, n, "0x%08X(%s)", *v, vsidName(*v));
        else snprintf(o, n, "absent");
    };
    char a[48], l[48], w[48];
    vs(voiceVsid, a, sizeof a);
    vs(lteVoiceVsid, l, sizeof l);
    vs(wlanVoiceVsid, w, sizeof w);
    snprintf(b, sizeof b, "cs_vsid=%s lte_vsid=%s wlan_vsid=%s active=%d priority=%d dds=%d", a, l, w,
             active ? *active : -1, priority ? *priority : -1, defaultData ? *defaultData : -1);
    return b;
}
Result getSubscriptionInfo(Client& c, SubscriptionInfo* out) {
    auto r = c.request(kSvcNas, req(kGetSubscriptionInfo));
    if (r.ok()) *out = parseSubscriptionInfo(r.msg);
    return r;
}
const char* vsidName(uint32_t v) {
    switch (v) {
        case 0x10C01000: return "CS-Voice";
        case 0x10C02000: return "VoLTE";
        case 0x10DC1000: return "Voice2";
        case 0x10803000: return "QCHAT";
        case 0x10002000: return "VoWLAN";
        case 0x10004000: return "VoIP";
        case 0x11C05000: return "VoiceMMode1";
        case 0x11DC5000: return "VoiceMMode2";
        case 0: return "none";
        default: return "unknown";
    }
}
Result getImsVoiceSupport(Client& c, ImsVoiceSupport* out) {
    auto r = c.request(kSvcNas, req(kGetSystemInfo));
    if (!r.ok()) return r;
    if (auto* v = r.msg.get(0x29); v && !v->empty()) out->lteImsVoice = (*v)[0];
    if (auto* v = r.msg.get(0x2A); v && v->size() >= 4) {
        Reader rd(*v);
        out->lteVoiceDomain = rd.u32();
    }
    return r;
}
}  // namespace nas

// =============================================================== WMS
namespace wms {
Result rawSend(Client& c, const std::vector<uint8_t>& pdu, bool expectMore, SendResult* out) {
    std::vector<uint8_t> v{kFormatGwPp, static_cast<uint8_t>(pdu.size() & 0xff),
                           static_cast<uint8_t>(pdu.size() >> 8)};
    v.insert(v.end(), pdu.begin(), pdu.end());
    Message m = req(kRawSend).raw(0x01, v);
    if (expectMore) m.u8(0x12, 5);  // keep the link 5 s for the next segment
    auto r = c.request(kSvcWms, m, 60000);
    if (auto* t = r.msg.get(0x01); t && t->size() >= 2) out->messageRef = Reader(*t).u16();
    if (auto* t = r.msg.get(0x12); t && t->size() >= 3) {
        Reader rd(*t);
        out->rpCause = rd.u16();
        out->tpCause = rd.u8();
    }
    if (auto* t = r.msg.get(0x13); t && !t->empty()) out->failureType = (*t)[0];
    return r;
}

Result setEventReport(Client& c, bool enable) {
    return c.request(kSvcWms, req(kSetEventReport).u8(0x10, enable ? 1 : 0));
}

Result setRoutesAction(Client& c, uint8_t storage, uint8_t action) {
    std::vector<uint8_t> v{5, 0};
    for (uint8_t cls : {0, 1, 2, 3, 4}) {
        v.push_back(0);  // point to point
        v.push_back(cls);
        v.push_back(storage);
        v.push_back(action);
    }
    return c.request(kSvcWms, req(kSetRoutes).raw(0x01, v).u8(0x10, 1));
}

Result setRoutes(Client& c, bool store) {
    std::vector<uint8_t> v{5, 0};
    for (uint8_t cls : {0, 1, 2, 3, 4}) {
        v.push_back(0);  // point to point
        v.push_back(cls);
        v.push_back(store ? kStorageNv : kStorageNone);
        v.push_back(store ? kReceiptStoreAndNotify : kReceiptTransferOnly);
    }
    return c.request(kSvcWms, req(kSetRoutes).raw(0x01, v).u8(0x10, 1));
}

Result sendAck(Client& c, uint32_t txn, bool success, uint8_t rpCause, uint8_t tpCause,
               const AckOptions& o, int* failureCause) {
    Message m = req(kSendAck);
    m.raw(0x01, {static_cast<uint8_t>(txn), static_cast<uint8_t>(txn >> 8),
                 static_cast<uint8_t>(txn >> 16), static_cast<uint8_t>(txn >> 24),
                 o.protocol, static_cast<uint8_t>(success ? 1 : 0)});
    if (!success) m.raw(0x11, {rpCause, tpCause});
    if (o.smsOnIms) m.u8(0x12, *o.smsOnIms ? 1 : 0);
    auto r = c.request(kSvcWms, m);
    if (failureCause) {
        *failureCause = -1;
        if (auto* t = r.msg.get(0x10); t && !t->empty() && !r.ok()) *failureCause = (*t)[0];
    }
    return r;
}

uint8_t messageProtocolFor(uint8_t format) { return format == kFormatCdma ? 0 : 1; }

const char* ackFailureCauseName(int cause) {
    switch (cause) {
        case 0: return "no-network-response";
        case 1: return "network-released-link";
        case 2: return "not-sent";
        case -1: return "none";
        default: return "unknown";
    }
}

Result bindSubscription(Client& c, uint8_t sub) {
    return c.request(kSvcWms, req(kBindSubscription).u8(0x01, sub));
}

Result rawRead(Client& c, uint8_t storage, uint32_t index, uint8_t* format,
               std::vector<uint8_t>* data) {
    Message m = req(kRawRead);
    m.raw(0x01, {storage, static_cast<uint8_t>(index), static_cast<uint8_t>(index >> 8),
                 static_cast<uint8_t>(index >> 16), static_cast<uint8_t>(index >> 24)});
    m.u8(0x10, 1);  // GW mode
    auto r = c.request(kSvcWms, m);
    if (r.ok()) {
        Reader rd(r.msg.get(0x01));
        rd.u8();  // tag
        *format = rd.u8();
        *data = rd.bytes16();
        if (!rd.good()) {
            r.status = Result::QmiFailure;
            r.qmiError = kErrMalformedMessage;
        }
    }
    return r;
}

Result deleteMessage(Client& c, uint8_t storage, uint32_t index) {
    return c.request(kSvcWms, req(kDelete).u8(0x01, storage).u32(0x10, index).u8(0x12, 1));
}

Result setBroadcastActivation(Client& c, bool activate) {
    return c.request(kSvcWms, req(kSetBroadcastActivation).raw(0x01, {1, static_cast<uint8_t>(activate)}));
}

bool normalizeBroadcastRanges(std::vector<BroadcastRange> in, std::vector<BroadcastRange>* out) {
    std::sort(in.begin(), in.end(), [](const BroadcastRange& a, const BroadcastRange& b) {
        if (a.selected != b.selected) return a.selected > b.selected;
        return a.from != b.from ? a.from < b.from : a.to < b.to;
    });
    out->clear();
    for (auto& r : in) {
        if (!out->empty() && out->back().selected == r.selected &&
            static_cast<uint32_t>(r.from) <= static_cast<uint32_t>(out->back().to) + 1) {
            out->back().to = std::max(out->back().to, r.to);
            continue;
        }
        out->push_back(r);
    }
    return out->size() <= kMaxBroadcastRanges;
}

Message buildSetBroadcastConfig(const std::vector<BroadcastRange>& ranges) {
    std::vector<uint8_t> v{static_cast<uint8_t>(ranges.size())};
    for (auto& r : ranges) {
        v.push_back(static_cast<uint8_t>(r.from & 0xff));
        v.push_back(static_cast<uint8_t>(r.from >> 8));
        v.push_back(static_cast<uint8_t>(r.to & 0xff));
        v.push_back(static_cast<uint8_t>(r.to >> 8));
        v.push_back(r.selected ? 1 : 0);
    }
    return req(kSetBroadcastConfig).u8(0x01, 1).raw(0x10, std::move(v));
}

Result setBroadcastConfig(Client& c, const std::vector<BroadcastRange>& ranges) {
    if (ranges.size() > kMaxBroadcastRanges) {
        Result r;
        r.status = Result::QmiFailure;
        r.qmiError = kErrInvalidArgument;
        return r;
    }
    return c.request(kSvcWms, buildSetBroadcastConfig(ranges));
}

Result getSmscAddress(Client& c, std::string* number, std::string* type) {
    auto r = c.request(kSvcWms, req(kGetSmscAddress));
    if (r.ok()) {
        Reader rd(r.msg.get(0x01));
        *type = rd.str(3);
        *number = rd.str8();
        if (!rd.good()) {
            r.status = Result::QmiFailure;
            r.qmiError = kErrMalformedMessage;
        }
    }
    return r;
}

EventReport parseEventReport(const Message& m) {
    EventReport e;
    if (auto* v = m.get(0x10); v && v->size() >= 5) {
        Reader r(*v);
        uint8_t st = r.u8();
        uint32_t idx = r.u32();
        e.stored = std::make_pair(st, idx);
    }
    if (auto* v = m.get(0x11)) {
        Reader r(*v);
        e.ackIndicator = r.u8();
        e.txn = r.u32();
        e.format = r.u8();
        e.data = r.bytes16();
        e.hasTransfer = r.good();
    }
    if (auto* v = m.get(0x12); v && !v->empty()) e.messageMode = (*v)[0];
    if (auto* v = m.get(0x15)) e.smsc = std::string(v->begin(), v->end());
    if (auto* v = m.get(0x16); v && !v->empty()) e.smsOnIms = (*v)[0] != 0;
    if (auto* v = m.get(0x13)) {
        Reader r(*v);
        uint8_t type = r.u8();
        auto data = r.bytes16();
        if (r.good() && !data.empty()) {
            e.etwsType = type;
            e.etws = std::move(data);
        }
    }
    return e;
}
}  // namespace wms

// =============================================================== VOICE
namespace voice {
std::vector<CallInfo> parseCalls(const Message& m, bool ind) {
    std::vector<CallInfo> calls;
    const uint8_t tInfo = ind ? 0x01 : 0x10, tNum = ind ? 0x10 : 0x11;
    if (auto* v = m.get(tInfo)) {
        Reader r(*v);
        uint8_t n = r.u8();
        for (unsigned i = 0; i < n && r.good(); i++) {
            CallInfo ci;
            ci.id = r.u8();
            ci.state = r.u8();
            ci.type = r.u8();
            ci.direction = r.u8();
            ci.mode = r.u8();
            ci.multiparty = r.u8() != 0;
            ci.als = r.u8();
            if (r.good()) calls.push_back(ci);
        }
    }
    if (auto* v = m.get(tNum)) {
        Reader r(*v);
        uint8_t n = r.u8();
        for (unsigned i = 0; i < n && r.good(); i++) {
            uint8_t id = r.u8();
            uint8_t pi = r.u8();
            std::string num = r.str8();
            if (!r.good()) break;
            for (auto& ci : calls)
                if (ci.id == id) {
                    ci.number = num;
                    ci.presentation = pi;
                    ci.hasNumber = true;
                }
        }
    }
    return calls;
}

Result indicationRegister(Client& c) {
    Message m = req(kIndicationRegister);
    m.u8(0x13, 1);  // call notification events
    m.u8(0x12, 1);  // supplementary service notifications
    m.u8(0x16, 1);  // telephony-flows (29 Sep 2026): USSD notification events (USSD / release indications)
    return c.request(kSvcVoice, m);
}

Result dial(Client& c, const std::string& number, bool emergency, uint8_t* callId) {
    Message m = req(kDialCall).strNoLen(0x01, number);
    if (emergency) m.u8(0x10, kTypeEmergency);
    auto r = c.request(kSvcVoice, m, 30000);
    if (auto* v = r.msg.get(0x10); v && !v->empty()) *callId = (*v)[0];
    return r;
}
Result endCall(Client& c, uint8_t callId) {
    return c.request(kSvcVoice, req(kEndCall).u8(0x01, callId), 10000);
}
Result answer(Client& c, uint8_t callId) {
    return c.request(kSvcVoice, req(kAnswerCall).u8(0x01, callId), 10000);
}
Result getAllCalls(Client& c, std::vector<CallInfo>* out) {
    auto r = c.request(kSvcVoice, req(kGetAllCallInfo));
    if (r.ok()) *out = parseCalls(r.msg, false);
    return r;
}
Result manageCalls(Client& c, uint8_t sups, std::optional<uint8_t> callId) {
    Message m = req(kManageCalls).u8(0x01, sups);
    if (callId) m.u8(0x10, *callId);
    return c.request(kSvcVoice, m, 20000);
}
Result startDtmf(Client& c, uint8_t callId, char digit) {
    return c.request(kSvcVoice, req(kStartContDtmf).raw(0x01, {callId, static_cast<uint8_t>(digit)}));
}
Result stopDtmf(Client& c, uint8_t callId) {
    return c.request(kSvcVoice, req(kStopContDtmf).raw(0x01, {callId}));
}
Result burstDtmf(Client& c, uint8_t callId, const std::string& digits) {
    std::vector<uint8_t> v{callId, static_cast<uint8_t>(digits.size())};
    v.insert(v.end(), digits.begin(), digits.end());
    return c.request(kSvcVoice, req(kBurstDtmf).raw(0x01, v), 20000);
}

// volte3: call domain / VoLTE helpers (see services.h)
const char* callTypeName(uint8_t t) {
    switch (t) {
        case 0x00: return "voice";
        case 0x01: return "voice-forced";
        case 0x02: return "voice-ip";
        case 0x03: return "vt";
        case 0x04: return "videoshare";
        case 0x05: return "test";
        case 0x06: return "otapa";
        case 0x07: return "std-otasp";
        case 0x08: return "non-std-otasp";
        case 0x09: return "emergency";
        case 0x0A: return "sups";
        case 0x0B: return "emergency-ip";
        case 0x0C: return "ecall";
        case 0x0D: return "emergency-vt";
        default: return "?";
    }
}
const char* callModeName(uint8_t m) {
    switch (m) {
        case kModeNoSrv: return "no-srv";
        case kModeCdma: return "cdma";
        case kModeGsm: return "gsm";
        case kModeUmts: return "umts";
        case kModeLte: return "lte";
        case kModeTdscdma: return "tdscdma";
        case kModeUnknown: return "unknown";
        case kModeWlan: return "wlan";
        case kModeNr5g: return "nr5g";
        default: return "?";
    }
}
bool isImsCall(const CallInfo& c) {
    if (c.type == 0x02 || c.type == 0x03 || c.type == 0x04 || c.type == 0x0B || c.type == 0x0D) return true;
    return c.mode == kModeLte || c.mode == kModeWlan || c.mode == kModeNr5g;
}
const char* callDomain(const CallInfo& c) {
    if (isImsCall(c)) return c.mode == kModeWlan ? "ims-wlan" : "ims";
    if (c.mode == kModeCdma || c.mode == kModeGsm || c.mode == kModeUmts || c.mode == kModeTdscdma) return "cs";
    return "unknown";
}
Result dialTyped(Client& c, const std::string& number, uint8_t callType, uint8_t* callId) {
    Message m = req(kDialCall).strNoLen(0x01, number);
    m.u8(0x10, callType);
    auto r = c.request(kSvcVoice, m, 30000);
    if (auto* v = r.msg.get(0x10); v && !v->empty()) *callId = (*v)[0];
    return r;
}
// r5 deep review F23/F24 (28 Sep 2026)
Message buildDial(const DialRequest& d) {
    Message m = req(kDialCall).strNoLen(0x01, d.number);
    if (d.emergency) m.u8(0x10, kTypeEmergency);
    if (d.clir) m.u8(0x11, *d.clir);
    if (d.emergency && d.emergencyCategory) m.u8(0x14, *d.emergencyCategory);
    return m;
}
Result dial(Client& c, const DialRequest& d, uint8_t* callId) {
    auto r = c.request(kSvcVoice, buildDial(d), 30000);
    if (auto* v = r.msg.get(0x10); v && !v->empty()) *callId = (*v)[0];
    return r;
}
bool clirFromAndroid(int32_t androidClir, std::optional<uint8_t>* out) {
    switch (androidClir) {
        case 0: *out = std::nullopt; return true;       // CLIR_DEFAULT: subscription default
        case 1: *out = kClirInvocation; return true;    // CLIR_INVOCATION: restrict presentation
        case 2: *out = kClirSuppression; return true;   // CLIR_SUPPRESSION: allow presentation
        default: return false;
    }
}
bool isWellKnownEmergencyNumber(const std::string& number) {
    std::string d;
    for (char ch : number)
        if (ch >= '0' && ch <= '9') d += ch;
        else if (ch != '+' && ch != '-' && ch != ' ' && ch != '(' && ch != ')') return false;  // *#, pause: not a plain number
    static const char* const kList[] = {"112", "911", "999", "000", "08",  "110", "118", "119", "100", "101",
                                        "102", "103", "104", "108", "113", "115", "120", "122", "15",  "17",
                                        "18",  "190", "192", "193", "197", "995", "997", "998", "061", "062",
                                        "114", "191", "196"};  // telephony-flows: FR 114 (deaf), 191 air, 196 sea
    for (auto* e : kList)
        if (d == e) return true;
    return false;
}
bool isEncodingRejection(const Result& r) {
    return r.status == Result::QmiFailure &&
           (r.qmiError == kErrMalformedMessage || r.qmiError == kErrInvalidArgument ||
            r.qmiError == kErrMissingArgument || r.qmiError == kErrNotSupported);
}
EmergencyDialOutcome emergencyDial(Client& c, const EmergencyDialRequest& q, uint8_t* callId) {
    EmergencyDialOutcome o;
    std::optional<uint8_t> clir;
    if (!clirFromAndroid(q.androidClir, &clir)) clir.reset();  // never block an emergency call on a bad CLIR value
    auto attempt = [&](DialRequest d) {
        o.attempts.push_back(d);
        o.result = dial(c, d, callId);
        ALOGI_Q("emergency dial attempt %zu (emergency=%d clir=%d cat=%d): %s", o.attempts.size(), d.emergency,
                d.clir ? *d.clir : -1, d.emergencyCategory ? *d.emergencyCategory : -1, o.result.describe().c_str());
        return o.result;
    };
    if (q.isTesting) {
        // "must not be sent to a real emergency service": no emergency call type, no emergency fallback, and no
        // request at all for a number the modem/network would route to an emergency centre anyway.
        if (isWellKnownEmergencyNumber(q.number)) {
            o.refusedTestToEmergencyNumber = true;
            o.result.status = Result::QmiFailure;
            o.result.qmiError = kErrInvalidArgument;
            ALOGW_Q("test emergency dial to a real emergency number refused (nothing sent)");
            return o;
        }
        attempt({q.number, false, clir, std::nullopt});
        return o;
    }
    bool normalAttempted = false;
    if (q.routing == EmergencyRouting::Normal && !q.hasKnownUserIntentEmergency) {
        normalAttempted = true;
        auto r = attempt({q.number, false, clir, std::nullopt});
        // success, or an ambiguous outcome (timeout / transport: a call may have been created) -> no second dial
        if (r.ok() || r.status != Result::QmiFailure) return o;
        ALOGW_Q("normal routing refused (0x%x): using emergency routing", r.qmiError);
    }
    std::optional<uint8_t> cat;
    if (q.categories & 0x1F) cat = static_cast<uint8_t>(q.categories & 0x1F);
    auto r = attempt({q.number, true, std::nullopt, cat});
    if (r.ok() || !isEncodingRejection(r)) return o;
    if (cat) {
        r = attempt({q.number, true, std::nullopt, std::nullopt});
        if (r.ok() || !isEncodingRejection(r)) return o;
    }
    if (!normalAttempted) attempt({q.number, false, std::nullopt, std::nullopt});  // legacy VOICE: no call type TLV
    return o;
}
Result indicationRegisterVolte(Client& c) {
    Message m = req(kIndicationRegister);
    m.u8(0x12, 1);  // supplementary service notifications
    m.u8(0x13, 1);  // call notification events
    m.u8(0x14, 1);  // handover events (SRVCC)
    m.u8(0x15, 1);  // speech codec events (AMR / AMR-WB = HD)
    m.u8(0x23, 1);  // audio RAT change events (stock qcrild: byte 38 of the request struct = TLV 0x23)
    return c.request(kSvcVoice, m);
}
AudioRatInfo parseAudioRatChange(const Message& m) {
    AudioRatInfo a;
    if (auto* v = m.get(0x10); v && v->size() >= 4) {
        Reader r(*v);
        a.sessionInfo = r.u32();
    }
    if (auto* v = m.get(0x11); v && !v->empty()) a.rat = (*v)[0];
    return a;
}

// ---- r5 review round7 (28 Sep 2026): F56 finite DTMF, F57 targeted reject, F58 call end reasons
std::vector<std::pair<uint8_t, uint16_t>> parseCallEndReasons(const Message& m) {
    std::vector<std::pair<uint8_t, uint16_t>> out;
    auto* v = m.get(0x14);
    if (!v || v->empty()) return out;
    Reader r(*v);
    uint8_t n = r.u8();
    if (v->size() != 1u + 3u * n) return out;  // layout mismatch: unknown, never misread
    for (unsigned i = 0; i < n; i++) {
        uint8_t id = r.u8();
        uint16_t reason = r.u16();
        out.push_back({id, reason});
    }
    return out;
}

int32_t lastCallFailCauseFromQmi(uint16_t q) {
    // 3GPP TS 24.008 CC causes (QMI 141..188) -> the same Android value
    static const std::pair<uint16_t, int32_t> kCc[] = {
            {141, 1},   {142, 3},   {143, 6},   {144, 8},   {145, 16},  {146, 17},  {147, 18},  {148, 19},
            {149, 21},  {150, 22},  {151, 25},  {152, 27},  {153, 28},  {154, 29},  {155, 30},  {156, 31},
            {157, 34},  {158, 38},  {159, 41},  {160, 42},  {161, 43},  {162, 44},  {163, 47},  {164, 49},
            {165, 50},  {166, 55},  {167, 57},  {168, 58},  {169, 63},  {170, 68},  {171, 65},  {172, 69},
            {173, 70},  {174, 79},  {175, 81},  {176, 87},  {177, 88},  {178, 91},  {179, 95},  {180, 96},
            {181, 97},  {182, 98},  {183, 99},  {184, 100}, {185, 101}, {186, 102}, {187, 111}, {188, 127},
    };
    for (auto& [k, v] : kCc)
        if (k == q) return v;
    switch (q) {
        case 29:   // CLIENT_END: this phone's user ended the call
        case 25:   // RELEASE_NORMAL: normal release from the network
            return kLcfNormal;
        case 104:  // NETWORK_END: network ended the call, no further cause
            return kLcfNormalUnspecified;
        case 102:  // INCOMING_REJECTED (client rejected the incoming call)
        case 103:  // SETUP_REJECTED
        case 134:  // REJECTED_BY_USER
            return kLcfCallRejected;
        case 105: return 68;   // NO_FUNDS -> ACM_LIMIT_EXCEEDED
        case 115:              // CALL_BARRED
        case 189:              // OUTGOING_CALLS_BARRED_WITHIN_CUG
            return 240;        // CALL_BARRED
        case 199: return 242;  // IMSI_UNKNOWN_IN_VLR
        case 200: return 243;  // IMEI_NOT_ACCEPTED
        case 0: return kLcfRadioOff;  // OFFLINE
        case 21: case 106: case 107: case 108: case 223:  // NO_SERVICE / NO_GW / NO_CDMA / NO_FULL / NO_CELL
            return kLcfOutOfService;
        case 34: case 217: return kLcfNoValidSim;  // UIM_NOT_PRESENT / INVALID_SIM
        case 22: case 225: case 307: return kLcfRadioLinkLost;  // FADE / RADIO_LINK_LOST / DATA_CONNECTION_LOST
        case 216: case 228: case 229: case 230: case 231: case 232: case 233: case 234: case 235: case 236:
            return kLcfRadioAccessFailure;  // ACCESS_STRATUM_FAILURE / ACCESS_STRATUM_REJECT_*
        case 219: case 313: return kLcfAccessClassBlocked;  // ACCESS_CLASS_BLOCKED / SSAC_REJECT
        case 222: case 226: case 305: case 310: return kLcfNetworkRespTimeout;  // T3230 / T303 / no response
        case 135: case 198: case 201: case 202: case 203: case 204: case 205: case 206: case 210: case 211:
        case 212: case 224: case 309:
            return kLcfNetworkReject;  // REJECTED_BY_NETWORK / MM rejects / ABORT / SIP 403
        case 209: return 34;   // NETWORK_CONGESTION -> CONGESTION
        case 220: return 47;   // NO_RESOURCES -> RESOURCES_UNAVAILABLE_OR_UNSPECIFIED
        default: return kLcfErrorUnspecified;
    }
}

RejectOutcome rejectRingingOrWaiting(Client& c) {
    RejectOutcome o;
    std::vector<CallInfo> calls;
    o.result = getAllCalls(c, &calls);
    if (!o.result.ok()) {
        o.kind = RejectOutcome::QueryFailed;  // no untargeted fallback on a failed query
        return o;
    }
    auto find = [](const std::vector<CallInfo>& v, uint8_t state) -> std::optional<uint8_t> {
        for (auto& ci : v)
            if (ci.state == state) return ci.id;
        return std::nullopt;
    };
    if (auto id = find(calls, kStateIncoming)) {
        o.kind = RejectOutcome::EndedIncoming;
        o.callId = *id;
        o.result = endCall(c, *id);
        return o;
    }
    auto w = find(calls, kStateWaiting);
    if (!w) {
        o.kind = RejectOutcome::NoTarget;  // only held/active/nothing: never release an unrelated call
        o.result = Result{};
        return o;
    }
    o.kind = RejectOutcome::ReleasedWaiting;
    o.callId = *w;
    o.result = manageCalls(c, kSupsReleaseHeldOrWaiting, *w);
    if (o.result.ok() || !isEncodingRejection(o.result)) return o;
    // the call-id TLV itself was refused: plain CHLD=0 only while that same call is still the waiting one
    std::vector<CallInfo> again;
    if (!getAllCalls(c, &again).ok()) return o;
    bool stillWaiting = false;
    for (auto& ci : again)
        if (ci.id == *w && ci.state == kStateWaiting) stillWaiting = true;
    if (stillWaiting) o.result = manageCalls(c, kSupsReleaseHeldOrWaiting);
    return o;
}

bool isDtmfDigit(char ch) { return (ch >= '0' && ch <= '9') || ch == '*' || ch == '#'; }

FiniteDtmfOutcome finiteDtmf(Client& c, uint8_t callId, char digit, int toneMs, int stopTries, int retryMs) {
    FiniteDtmfOutcome o;
    o.start = startDtmf(c, callId, digit);
    // a refused START started nothing; a timed-out START may have been applied: it is stopped too (still a failure)
    if (!o.start.ok() && o.start.status != Result::Timeout) return o;
    std::this_thread::sleep_for(std::chrono::milliseconds(toneMs));
    for (int i = 0; i < std::max(1, stopTries); i++) {
        if (i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(retryMs));
            // retry only for the same live call: a vanished id means the tone ended with the call
            std::vector<CallInfo> calls;
            if (getAllCalls(c, &calls).ok()) {
                bool live = false;
                for (auto& ci : calls)
                    if (ci.id == callId && ci.state != kStateEnd && ci.state != kStateDisconnecting) live = true;
                if (!live) {
                    o.callGone = true;
                    return o;
                }
            }
        }
        o.stopAttempts++;
        o.stop = stopDtmf(c, callId);
        if (o.stop.ok()) return o;
    }
    return o;
}
}  // namespace voice

// =============================================================== WDS / WDA
namespace wds {
Result bindMuxDataPort(Client& c, uint32_t epType, uint32_t iface, uint8_t muxId) {
    Message m = req(kBindMuxDataPort);
    std::vector<uint8_t> ep;
    for (uint32_t x : {epType, iface})
        for (int i = 0; i < 4; i++) ep.push_back((x >> (8 * i)) & 0xff);
    m.raw(0x10, ep);
    m.u8(0x11, muxId);
    return c.request(kSvcWds, m);
}
Result setIpFamily(Client& c, uint8_t fam) {
    return c.request(kSvcWds, req(kSetIpFamily).u8(0x01, fam));
}
Result startNetwork(Client& c, const StartParams& p, StartResult* out, int timeoutMs) {
    Message m = req(kStartNetwork);
    if (!p.apn.empty()) m.strNoLen(0x14, p.apn);
    if (p.auth) m.u8(0x16, p.auth);
    if (!p.user.empty()) m.strNoLen(0x17, p.user);
    if (!p.password.empty()) m.strNoLen(0x18, p.password);
    m.u8(0x19, p.ipFamily);
    if (p.profileIndex3gpp) m.u8(0x31, *p.profileIndex3gpp);
    auto r = c.request(kSvcWds, m, timeoutMs);
    if (auto* v = r.msg.get(0x01); v && v->size() >= 4) out->handle = Reader(*v).u32();
    if (auto* v = r.msg.get(0x10); v && v->size() >= 2) out->callEndReason = Reader(*v).u16();
    if (auto* v = r.msg.get(0x11); v && v->size() >= 4) {
        Reader rd(*v);
        uint16_t t = rd.u16();
        int16_t rs = rd.i16();
        out->verboseReason = std::make_pair(t, rs);
    }
    return r;
}
Result stopNetwork(Client& c, uint32_t handle) {
    return c.request(kSvcWds, req(kStopNetwork).u32(0x01, handle), 20000);
}

static std::optional<std::array<uint8_t, 16>> v6(const std::vector<uint8_t>* v, uint8_t* prefix) {
    if (!v || v->size() < 16) return std::nullopt;
    std::array<uint8_t, 16> a;
    for (int i = 0; i < 16; i++) a[i] = (*v)[i];
    if (prefix && v->size() >= 17) *prefix = (*v)[16];
    return a;
}

Settings parseSettings(const Message& m) {
    Settings s;
    auto u32 = [&](uint8_t t) -> std::optional<uint32_t> {
        auto* v = m.get(t);
        if (!v || v->size() < 4) return std::nullopt;
        return Reader(*v).u32();
    };
    s.dns4a = u32(0x15);
    s.dns4b = u32(0x16);
    s.ipv4 = u32(0x1E);
    s.gw4 = u32(0x20);
    s.mask4 = u32(0x21);
    s.mtu = u32(0x29);
    if (auto* v = m.get(0x14)) s.apn = std::string(v->begin(), v->end());
    s.ipv6 = v6(m.get(0x25), &s.ipv6Prefix);
    s.gw6 = v6(m.get(0x26), &s.gw6Prefix);
    s.dns6a = v6(m.get(0x27), nullptr);
    s.dns6b = v6(m.get(0x28), nullptr);
    if (auto* v = m.get(0x2B); v && !v->empty()) s.family = (*v)[0];
    if (auto* v = m.get(0x23)) {
        Reader r(*v);
        uint8_t n = r.u8();
        for (unsigned i = 0; i < n && r.good(); i++) s.pcscf4.push_back(r.u32());
        if (!r.good()) s.pcscf4.clear();
    }
    if (auto* v = m.get(0x2A)) {
        Reader r(*v);
        uint8_t n = r.u8();
        for (unsigned i = 0; i < n && r.good(); i++) {
            std::string d = r.str16();
            if (r.good()) s.domains.push_back(d);
        }
    }
    return s;
}

Result getCurrentSettings(Client& c, Settings* out, uint32_t mask) {
    auto r = c.request(kSvcWds, req(kGetCurrentSettings).u32(0x10, mask));
    if (r.ok()) *out = parseSettings(r.msg);
    return r;
}

Result getPacketServiceStatus(Client& c, uint8_t* status) {
    auto r = c.request(kSvcWds, req(kPacketServiceStatus));
    if (r.ok()) {
        auto* v = r.msg.get(0x01);
        *status = (v && !v->empty()) ? (*v)[0] : 0;
    }
    return r;
}

PacketStatus parsePacketStatus(const Message& m) {
    PacketStatus p;
    if (auto* v = m.get(0x01); v && v->size() >= 2) {
        p.status = (*v)[0];
        p.reconfig = (*v)[1] != 0;
    }
    if (auto* v = m.get(0x10); v && v->size() >= 2) p.callEndReason = Reader(*v).u16();
    if (auto* v = m.get(0x11); v && v->size() >= 4) {
        Reader rd(*v);
        uint16_t t = rd.u16();
        int16_t rs = rd.i16();
        p.verboseReason = std::make_pair(t, rs);
    }
    if (auto* v = m.get(0x12); v && !v->empty()) p.family = (*v)[0];
    return p;
}
}  // namespace wds

namespace wda {
static void putEp(std::vector<uint8_t>* v, uint32_t epType, uint32_t iface) {
    for (uint32_t x : {epType, iface})
        for (int i = 0; i < 4; i++) v->push_back((x >> (8 * i)) & 0xff);
}
static void readFormat(const Message& m, DataFormat* d) {
    auto u32 = [&](uint8_t t, uint32_t* dst) {
        if (auto* v = m.get(t); v && v->size() >= 4) *dst = Reader(*v).u32();
    };
    u32(0x11, &d->llp);
    u32(0x12, &d->ulAgg);
    u32(0x13, &d->dlAgg);
    u32(0x15, &d->dlMaxDatagrams);
    u32(0x16, &d->dlMaxSize);
}
Message buildSetDataFormat(const DataFormat& f) {
    Message m = req(kSetDataFormat);
    m.u32(0x11, f.llp);
    m.u32(0x12, f.ulAgg);
    m.u32(0x13, f.dlAgg);
    if (f.dlAgg != kAggDisabled) {
        m.u32(0x15, f.dlMaxDatagrams);
        m.u32(0x16, f.dlMaxSize);
    }
    std::vector<uint8_t> ep;
    putEp(&ep, f.epType, f.iface);
    m.raw(0x17, ep);
    return m;
}
Result setDataFormat(Client& c, const DataFormat& f, DataFormat* granted) {
    auto r = c.request(kSvcWda, buildSetDataFormat(f));
    if (r.ok() && granted) {
        *granted = f;
        readFormat(r.msg, granted);
    }
    return r;
}
Result getDataFormat(Client& c, uint32_t epType, uint32_t iface, DataFormat* out) {
    Message m = req(kGetDataFormat);
    std::vector<uint8_t> ep;
    putEp(&ep, epType, iface);
    m.raw(0x10, ep);
    auto r = c.request(kSvcWda, m);
    if (r.ok() && out) {
        out->epType = epType;
        out->iface = iface;
        readFormat(r.msg, out);
    }
    return r;
}
}  // namespace wda

namespace dpm {
Message buildOpenPort(const std::vector<HwDataPort>& hw, const std::vector<CtlPort>& ctl) {
    Message m = req(kOpenPort);
    auto le32 = [](std::vector<uint8_t>* v, uint32_t x) {
        for (int i = 0; i < 4; i++) v->push_back((x >> (8 * i)) & 0xff);
    };
    if (!ctl.empty()) {
        std::vector<uint8_t> v{static_cast<uint8_t>(ctl.size())};
        for (auto& p : ctl) {
            v.push_back(static_cast<uint8_t>(p.name.size()));
            v.insert(v.end(), p.name.begin(), p.name.end());
            le32(&v, p.epType);
            le32(&v, p.iface);
        }
        m.raw(0x10, v);
    }
    if (!hw.empty()) {
        std::vector<uint8_t> v{static_cast<uint8_t>(hw.size())};
        for (auto& p : hw) {
            le32(&v, p.epType);
            le32(&v, p.iface);
            le32(&v, p.rxEp);
            le32(&v, p.txEp);
        }
        m.raw(0x11, v);
    }
    return m;
}
Result openPort(Client& c, const std::vector<HwDataPort>& hw, const std::vector<CtlPort>& ctl) {
    return c.request(kSvcDpm, buildOpenPort(hw, ctl), 10000);
}
Result closePort(Client& c) { return c.request(kSvcDpm, req(kClosePort)); }
}  // namespace dpm

}  // namespace a6l::qmi
