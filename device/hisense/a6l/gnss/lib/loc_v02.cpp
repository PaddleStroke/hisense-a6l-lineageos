// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC v02 builders/parsers.
#include "loc_v02.h"

#include <algorithm>

namespace a6l {
namespace loc {

using qmi::Message;
using qmi::Reader;
using qmi::Writer;

const char* sessionStatusName(uint32_t s) {
    switch (s) {
        case kStatusSuccess: return "SUCCESS";
        case kStatusInProgress: return "IN_PROGRESS";
        case kStatusGeneralFailure: return "GENERAL_FAILURE";
        case kStatusTimeout: return "TIMEOUT";
        case kStatusUserEnd: return "USER_END";
        case kStatusBadParameter: return "BAD_PARAMETER";
        case kStatusPhoneOffline: return "PHONE_OFFLINE";
        case kStatusEngineLocked: return "ENGINE_LOCKED";
        default: return "UNKNOWN";
    }
}

static Message req(uint16_t id) {
    Message m;
    m.type = qmi::kRequest;
    m.msgId = id;
    return m;
}

Message makeInformClientRevision(uint32_t revision) {
    return req(kInformClientRevision).add(0x01, Writer().u32(revision));
}

Message makeRegEvents(uint64_t mask) { return req(kRegEvents).add(0x01, Writer().u64(mask)); }

Message makeSetOperationMode(uint32_t mode) { return req(kSetOperationMode).add(0x01, Writer().u32(mode)); }

Message makeSetNmeaTypes(uint32_t mask) { return req(kSetNmeaTypes).add(0x01, Writer().u32(mask)); }

Message makeSetEngineLock(uint32_t lock) { return req(kSetEngineLock).add(0x01, Writer().u32(lock)); }

Message makeStart(uint8_t sessionId, uint32_t minIntervalMs, bool intermediate, uint32_t accuracyLevel) {
    Message m = req(kStart);
    m.add(0x01, Writer().u8(sessionId));
    m.add(0x10, Writer().u32(1));                          // fixRecurrence: periodic
    m.add(0x11, Writer().u32(accuracyLevel));              // horizontalAccuracyLevel
    m.add(0x12, Writer().u32(intermediate ? 1 : 2));       // intermediateReportState
    m.add(0x13, Writer().u32(minIntervalMs));              // minInterval (ms)
    return m;
}

Message makeStop(uint8_t sessionId) { return req(kStop).add(0x01, Writer().u8(sessionId)); }

Message makeInjectUtcTime(uint64_t utcMs, uint32_t uncMs) {
    Message m = req(kInjectUtcTime);
    m.add(0x01, Writer().u64(utcMs));
    m.add(0x02, Writer().u32(uncMs));
    return m;
}

Message makeInjectPosition(double lat, double lon, float horUncMeters) {
    Message m = req(kInjectPosition);
    m.add(0x10, Writer().f64(lat));
    m.add(0x11, Writer().f64(lon));
    m.add(0x12, Writer().f32(horUncMeters));
    m.add(0x13, Writer().u8(68));   // horConfidence (%)
    return m;
}

Message makeDeleteAllAssistData() { return req(kDeleteAssistData).add(0x01, Writer().u8(1)); }

Message makeInjectCoarsePosition(double lat, double lon, float horUncMeters, uint32_t positionSource) {
    Message m = makeInjectPosition(lat, lon, horUncMeters);
    m.add(0x14, Writer().u32(2));                 // horReliability: LOW (coarse)
    m.add(0x1D, Writer().u32(positionSource));    // positionSrc
    return m;
}

Message makeInjectPredictedOrbitsPart(uint32_t totalSize, uint16_t totalParts, uint16_t partNum, const uint8_t* data,
                                      size_t len, bool withFormatType) {
    Message m = req(kInjectPredictedOrbits);
    m.add(0x01, Writer().u32(totalSize));
    m.add(0x02, Writer().u16(totalParts));
    m.add(0x03, Writer().u16(partNum));
    m.add(0x04, Writer().u16(uint16_t(len)).bytes(data, len));
    if (withFormatType) m.add(0x10, Writer().u32(0));   // QMI_LOC_PREDICTED_ORBITS_XTRA
    return m;
}

Message makeGetPredictedOrbitsSource() { return req(kGetPredictedOrbitsSource); }
Message makeGetPredictedOrbitsValidity() { return req(kGetPredictedOrbitsValidity); }

bool parseOrbitsSourceInd(const Message& m, OrbitsSource* o) {
    *o = OrbitsSource();
    if (!m.getU32(0x01, &o->status)) return false;
    if (auto* v = m.find(0x10)) {
        Reader r(*v);
        o->hasSizes = r.u32(&o->maxFileSize) && r.u32(&o->maxPartSize);
    }
    if (auto* v = m.find(0x11)) {   // server list: scan for URLs (string length encoding varies across IDL revs)
        const std::vector<uint8_t>& b = *v;
        for (size_t i = 0; i + 4 < b.size(); i++) {
            if (b[i] != 'h' || b[i + 1] != 't' || b[i + 2] != 't' || b[i + 3] != 'p') continue;
            size_t j = i;
            while (j < b.size() && b[j] > 0x20 && b[j] < 0x7f) j++;
            o->servers.emplace_back(b.begin() + i, b.begin() + j);
            i = j;
        }
    }
    return true;
}

bool parseOrbitsValidityInd(const Message& m, OrbitsValidity* v) {
    *v = OrbitsValidity();
    if (!m.getU32(0x01, &v->status)) return false;
    if (auto* t = m.find(0x10)) {
        Reader r(*t);
        v->valid = r.u64(&v->startGpsSec) && r.u16(&v->durationHours);
    }
    return true;
}

bool parseInjectOrbitsInd(const Message& m, uint32_t* status, uint16_t* partNum) {
    *partNum = 0;
    if (!m.getU32(0x01, status)) return false;
    m.getU16(0x10, partNum);
    return true;
}

std::vector<Message> buildXtraParts(const std::vector<uint8_t>& file, size_t partSize, bool withFormatType) {
    std::vector<Message> out;
    if (file.empty()) return out;
    if (partSize == 0 || partSize > kMaxOrbitsPart) partSize = kMaxOrbitsPart;
    size_t parts = (file.size() + partSize - 1) / partSize;
    for (size_t i = 0; i < parts; i++) {
        size_t off = i * partSize, len = std::min(partSize, file.size() - off);
        out.push_back(makeInjectPredictedOrbitsPart(uint32_t(file.size()), uint16_t(parts), uint16_t(i + 1),
                                                    file.data() + off, len, withFormatType));
    }
    return out;
}

bool looksLikeXtra(const std::vector<uint8_t>& f, std::string* why) {
    if (f.size() < 1000) {
        if (why) *why = "file too small (" + std::to_string(f.size()) + " bytes; an HTTP error page?)";
        return false;
    }
    if (f.size() > 400 * 1024) {
        if (why) *why = "file too large (" + std::to_string(f.size()) + " bytes)";
        return false;
    }
    if (f[0] == '<' || (f.size() > 4 && f[0] == 'H' && f[1] == 'T' && f[2] == 'T' && f[3] == 'P')) {
        if (why) *why = "looks like HTML/HTTP text, not XTRA";
        return false;
    }
    return true;
}

bool parseU32Status(const Message& m, uint32_t* v) { return m.getU32(0x01, v); }

bool parsePosition(const Message& m, Fix* f) {
    *f = Fix();
    if (!m.getU32(0x01, &f->status)) return false;
    m.getU8(0x02, &f->sessionId);
    double lat, lon;
    if (m.getF64(0x10, &lat) && m.getF64(0x11, &lon)) {
        f->hasLatLon = true;
        f->latitude = lat;
        f->longitude = lon;
    }
    f->hasHorUnc = m.getF32(0x12, &f->horUncCircular);
    f->hasSpeed = m.getF32(0x18, &f->speedHorizontal);
    f->hasSpeedUnc = m.getF32(0x19, &f->speedUnc);
    f->hasAltEllipsoid = m.getF32(0x1A, &f->altEllipsoid);
    f->hasAltMsl = m.getF32(0x1B, &f->altMsl);
    f->hasVertUnc = m.getF32(0x1C, &f->vertUnc);
    f->hasSpeedVertical = m.getF32(0x1F, &f->speedVertical);
    f->hasHeading = m.getF32(0x20, &f->heading);
    f->hasHeadingUnc = m.getF32(0x21, &f->headingUnc);
    f->hasTech = m.getU32(0x23, &f->techMask);
    if (const auto* b = m.find(0x24)) {
        Reader r(*b);
        f->hasDop = r.f32(&f->pdop) && r.f32(&f->hdop) && r.f32(&f->vdop);
    }
    f->hasUtc = m.getU64(0x25, &f->utcMs);
    f->hasLeap = m.getU8(0x26, &f->leapSeconds);
    if (const auto* b = m.find(0x27)) {
        Reader r(*b);
        f->hasGpsTime = r.u16(&f->gpsWeek) && r.u32(&f->gpsTowMs);
    }
    f->hasTimeUnc = m.getF32(0x28, &f->timeUncMs);
    if (const auto* b = m.find(0x2C)) {
        Reader r(*b);
        uint8_t n = 0;
        if (r.u8(&n)) {
            for (uint8_t i = 0; i < n; i++) {
                uint16_t id;
                if (!r.u16(&id)) break;
                f->svUsed.push_back(id);
            }
        }
    }
    uint8_t assumed = 0;
    if (m.getU8(0x2D, &assumed)) f->altitudeAssumed = assumed != 0;
    return true;
}

bool parseSvInfo(const Message& m, std::vector<Sv>* out, bool* altitudeAssumed) {
    out->clear();
    uint8_t assumed = 0;
    if (!m.getU8(0x01, &assumed)) return false;
    if (altitudeAssumed) *altitudeAssumed = assumed != 0;
    const auto* b = m.find(0x10);
    if (!b) return true;   // no list = no satellites
    Reader r(*b);
    uint8_t n = 0;
    if (!r.u8(&n)) return false;
    if (r.left() < size_t(n) * kSvInfoWireSize) return false;
    for (uint8_t i = 0; i < n; i++) {
        Sv s;
        r.u32(&s.valid);
        r.u32(&s.system);
        r.u16(&s.svId);
        r.u8(&s.health);
        r.u32(&s.status);
        r.u8(&s.infoMask);
        r.f32(&s.elevation);
        r.f32(&s.azimuth);
        r.f32(&s.snr);
        out->push_back(s);
    }
    return true;
}

bool parseNmea(const Message& m, std::vector<std::string>* sentences) {
    sentences->clear();
    std::string s;
    // 0x10 = expandedNmea (newer modems, up to 4 KiB) when present, else 0x01 (<= 200 chars)
    if (!m.getString(0x10, &s) || s.empty()) {
        if (!m.getString(0x01, &s)) return false;
    }
    std::string cur;
    for (char c : s) {
        if (c == '\r' || c == '\n') {
            if (!cur.empty()) sentences->push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) sentences->push_back(cur);
    return true;
}

AndroidSvId toAndroid(uint32_t system, uint16_t id) {
    switch (system) {
        case kSysGps: return {1, id};
        case kSysSbas: return {2, id >= 33 && id <= 64 ? id + 87 : id};   // 33..64 -> PRN 120..151
        case kSysGlonass: return {3, id >= 65 && id <= 96 ? id - 64 : id};
        case kSysQzss: return {4, id};
        case kSysCompass:
        case kSysBds: return {5, id >= 201 && id <= 263 ? id - 200 : id};
        case kSysGalileo: return {6, id >= 301 && id <= 336 ? id - 300 : id};
        default: return {0, id};
    }
}

AndroidSvId toAndroidFromUsedId(uint16_t id) {
    if (id >= 1 && id <= 32) return {1, id};
    if (id >= 33 && id <= 64) return {2, id + 87};
    if (id >= 65 && id <= 96) return {3, id - 64};
    if (id >= 193 && id <= 200) return {4, id};
    if (id >= 201 && id <= 263) return {5, id - 200};
    if (id >= 301 && id <= 336) return {6, id - 300};
    return {0, id};
}

double carrierHz(int c) {
    switch (c) {
        case 3: return 1602.0e6;      // GLONASS L1 (FDMA centre; channel unknown)
        case 5: return 1561.098e6;    // BeiDou B1I
        default: return 1575.42e6;    // GPS/SBAS/QZSS L1 C/A, Galileo E1
    }
}

Message makePositionInd(const Fix& f) {
    Message m;
    m.type = qmi::kIndication;
    m.msgId = kIndPosition;
    m.add(0x01, Writer().u32(f.status));
    m.add(0x02, Writer().u8(f.sessionId));
    if (f.hasLatLon) {
        m.add(0x10, Writer().f64(f.latitude));
        m.add(0x11, Writer().f64(f.longitude));
    }
    if (f.hasHorUnc) m.add(0x12, Writer().f32(f.horUncCircular));
    if (f.hasSpeed) m.add(0x18, Writer().f32(f.speedHorizontal));
    if (f.hasSpeedUnc) m.add(0x19, Writer().f32(f.speedUnc));
    if (f.hasAltEllipsoid) m.add(0x1A, Writer().f32(f.altEllipsoid));
    if (f.hasAltMsl) m.add(0x1B, Writer().f32(f.altMsl));
    if (f.hasVertUnc) m.add(0x1C, Writer().f32(f.vertUnc));
    if (f.hasHeading) m.add(0x20, Writer().f32(f.heading));
    if (f.hasTech) m.add(0x23, Writer().u32(f.techMask));
    if (f.hasDop) m.add(0x24, Writer().f32(f.pdop).f32(f.hdop).f32(f.vdop));
    if (f.hasUtc) m.add(0x25, Writer().u64(f.utcMs));
    if (f.hasLeap) m.add(0x26, Writer().u8(f.leapSeconds));
    if (f.hasGpsTime) m.add(0x27, Writer().u16(f.gpsWeek).u32(f.gpsTowMs));
    if (!f.svUsed.empty()) {
        Writer w;
        w.u8(uint8_t(f.svUsed.size()));
        for (auto id : f.svUsed) w.u16(id);
        m.add(0x2C, w);
    }
    return m;
}

Message makeSvInfoInd(const std::vector<Sv>& svs) {
    Message m;
    m.type = qmi::kIndication;
    m.msgId = kIndSvInfo;
    m.add(0x01, Writer().u8(0));
    Writer w;
    w.u8(uint8_t(svs.size()));
    for (const auto& s : svs)
        w.u32(s.valid).u32(s.system).u16(s.svId).u8(s.health).u32(s.status).u8(s.infoMask)
                .f32(s.elevation).f32(s.azimuth).f32(s.snr);
    m.add(0x10, w);
    return m;
}

Message makeNmeaInd(const std::string& nmea) {
    Message m;
    m.type = qmi::kIndication;
    m.msgId = kIndNmea;
    std::vector<uint8_t> v(nmea.begin(), nmea.end());
    v.push_back(0);
    m.add(0x01, std::move(v));
    return m;
}

}  // namespace loc
}  // namespace a6l
