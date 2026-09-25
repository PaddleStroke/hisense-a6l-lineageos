// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): typed QMI requests/parsers.
#include <a6lqmi/log.h>
#include <a6lqmi/services.h>

#include <cstdio>

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

Result readIccid(Client& c, std::string* out) {
    IoResult io;
    Session s{kSessionCardSlot1, {}};
    auto r = readTransparent(c, s, FilePath{0x2FE2, {0x3F00}}, 0, 0, &io);
    if (r.ok() && !io.data.empty()) {
        *out = decodeIccid(io.data);
        return r;
    }
    ALOGW_Q("UIM EF_ICCID read failed (%s), trying DMS", r.describe().c_str());
    return dms::uimGetIccid(c, out);
}

Result readImsi(Client& c, const std::vector<uint8_t>& aid, std::string* out) {
    IoResult io;
    Session s{kSessionPrimaryGw, aid};
    // EF_IMSI lives under ADF.USIM (7FFF) on a USIM and under DF.GSM (7F20) on a 2G SIM.
    auto r = readTransparent(c, s, FilePath{0x6F07, {0x3F00, 0x7FFF}}, 0, 0, &io);
    if (!(r.ok() && io.data.size() >= 2)) {
        r = readTransparent(c, s, FilePath{0x6F07, {0x3F00, 0x7F20}}, 0, 0, &io);
    }
    if (r.ok() && io.data.size() >= 2) {
        *out = decodeImsi(io.data);
        return r;
    }
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

Result setNetworkSelection(Client& c, bool manual, uint16_t mcc, uint16_t mnc, int8_t rat) {
    Message m = req(kInitiateNetworkRegister);
    m.u8(0x01, manual ? 2 : 1);
    if (manual) {
        m.raw(0x10, {static_cast<uint8_t>(mcc & 0xff), static_cast<uint8_t>(mcc >> 8),
                     static_cast<uint8_t>(mnc & 0xff), static_cast<uint8_t>(mnc >> 8),
                     static_cast<uint8_t>(rat)});
        m.u8(0x11, 1);  // permanent
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

Result sendAck(Client& c, uint32_t txn, bool success, uint8_t rpCause, uint8_t tpCause) {
    Message m = req(kSendAck);
    m.raw(0x01, {static_cast<uint8_t>(txn), static_cast<uint8_t>(txn >> 8),
                 static_cast<uint8_t>(txn >> 16), static_cast<uint8_t>(txn >> 24),
                 1 /*WCDMA/GW*/, static_cast<uint8_t>(success ? 1 : 0)});
    if (!success) m.raw(0x11, {rpCause, tpCause});
    return c.request(kSvcWms, m);
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
Result setDataFormat(Client& c, const DataFormat& f, DataFormat* granted) {
    Message m = req(kSetDataFormat);
    m.u32(0x11, f.llp);
    m.u32(0x12, f.ulAgg);
    m.u32(0x13, f.dlAgg);
    if (f.dlAgg != kAggDisabled) {
        m.u32(0x15, f.dlMaxDatagrams);
        m.u32(0x16, f.dlMaxSize);
    }
    std::vector<uint8_t> ep;
    for (uint32_t x : {f.epType, f.iface})
        for (int i = 0; i < 4; i++) ep.push_back((x >> (8 * i)) & 0xff);
    m.raw(0x17, ep);
    auto r = c.request(kSvcWda, m);
    if (r.ok() && granted) {
        *granted = f;
        auto u32 = [&](uint8_t t, uint32_t* dst) {
            if (auto* v = r.msg.get(t); v && v->size() >= 4) *dst = Reader(*v).u32();
        };
        u32(0x11, &granted->llp);
        u32(0x12, &granted->ulAgg);
        u32(0x13, &granted->dlAgg);
        u32(0x15, &granted->dlMaxDatagrams);
        u32(0x16, &granted->dlMaxSize);
    }
    return r;
}
}  // namespace wda

}  // namespace a6l::qmi
