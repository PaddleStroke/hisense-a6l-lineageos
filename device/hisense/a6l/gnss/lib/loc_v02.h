// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): QMI LOC (service 16, "Location service ~PDS v2") message ids, request
// builders and indication parsers. Ids/TLVs follow Qualcomm's public loc_api_v02 IDL as also used by libqmi
// (qmi-service-loc.json) and the CAF/LineageOS loc_api_v02 client. Only the subset needed for standalone fixes,
// NMEA, satellite status and time/position injection is implemented.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "qmi.h"

namespace a6l {
namespace loc {

constexpr uint32_t kServiceId = 16;

enum MsgId : uint16_t {
    kInformClientRevision = 0x0020,
    kRegEvents = 0x0021,
    kStart = 0x0022,
    kStop = 0x0023,
    kIndPosition = 0x0024,
    kIndSvInfo = 0x0025,
    kIndNmea = 0x0026,
    kIndNiNotify = 0x0027,
    kIndInjectTimeReq = 0x0028,
    kIndInjectOrbitsReq = 0x0029,
    kIndInjectPositionReq = 0x002A,
    kIndEngineState = 0x002B,
    kIndFixSessionState = 0x002C,
    kIndWifiReq = 0x002D,
    kIndSensorStreamingReady = 0x002E,
    kIndTimeSyncReq = 0x002F,
    kIndSpiStreamingReport = 0x0030,
    kIndLocationServerConnReq = 0x0031,
    kGetServiceRevision = 0x0032,
    kInjectPredictedOrbits = 0x0035,        // request + indication (one per part)
    kGetPredictedOrbitsSource = 0x0036,     // request + indication (servers, max file/part size)
    kGetPredictedOrbitsValidity = 0x0037,   // request + indication (start GPS time, duration hours)
    kInjectUtcTime = 0x0038,
    kInjectPosition = 0x0039,
    kSetEngineLock = 0x003A,
    kGetEngineLock = 0x003B,
    kSetNmeaTypes = 0x003E,
    kGetNmeaTypes = 0x003F,
    kDeleteAssistData = 0x0044,
    kSetOperationMode = 0x004A,
    kGetOperationMode = 0x004B,
};

// QMI_LOC_REG_EVENTS mask bits (u64, TLV 0x01)
enum EventMask : uint64_t {
    kEvPositionReport = 0x00000001,
    kEvGnssSvInfo = 0x00000002,
    kEvNmea = 0x00000004,
    kEvNiNotifyVerify = 0x00000008,
    kEvInjectTimeReq = 0x00000010,
    kEvInjectOrbitsReq = 0x00000020,
    kEvInjectPositionReq = 0x00000040,
    kEvEngineState = 0x00000080,
    kEvFixSessionState = 0x00000100,
};

enum OperationMode : uint32_t { kModeDefault = 1, kModeMsb = 2, kModeMsa = 3, kModeStandalone = 4, kModeCellId = 5 };
enum EngineLock : uint32_t { kLockNone = 1, kLockMi = 2, kLockMt = 3, kLockAll = 4 };

// QMI_LOC_SET_NMEA_TYPES mask (u32)
enum NmeaMask : uint32_t {
    kNmeaGga = 0x1, kNmeaRmc = 0x2, kNmeaGsv = 0x4, kNmeaGsa = 0x8, kNmeaVtg = 0x10,
    kNmeaPqxfi = 0x20, kNmeaPstis = 0x40, kNmeaGlgsv = 0x80, kNmeaGngsa = 0x100, kNmeaGngns = 0x200,
};
constexpr uint32_t kDefaultNmeaMask = kNmeaGga | kNmeaRmc | kNmeaGsv | kNmeaGsa | kNmeaVtg | kNmeaGlgsv | kNmeaGngsa;

// Position report session status (TLV 0x01 of 0x0024)
enum SessionStatus : uint32_t {
    kStatusSuccess = 0, kStatusInProgress = 1, kStatusGeneralFailure = 2, kStatusTimeout = 3,
    kStatusUserEnd = 4, kStatusBadParameter = 5, kStatusPhoneOffline = 6, kStatusEngineLocked = 7,
};
const char* sessionStatusName(uint32_t s);

// SV system (qmiLocSvSystemEnumT_v02)
enum SvSystem : uint32_t {
    kSysGps = 1, kSysGalileo = 2, kSysSbas = 3, kSysCompass = 4, kSysGlonass = 5, kSysBds = 6, kSysQzss = 7,
};
enum SvStatus : uint32_t { kSvIdle = 1, kSvSearch = 2, kSvTrack = 3 };
enum SvValid : uint32_t {
    kSvValidSystem = 0x01, kSvValidId = 0x02, kSvValidHealth = 0x04, kSvValidStatus = 0x08,
    kSvValidInfoMask = 0x10, kSvValidElevation = 0x20, kSvValidAzimuth = 0x40, kSvValidSnr = 0x80,
};
enum SvInfoMask : uint8_t { kSvHasEphemeris = 0x01, kSvHasAlmanac = 0x02 };
constexpr size_t kSvInfoWireSize = 28;   // u32 valid, u32 system, u16 id, u8 health, u32 status, u8 info, 3 x float

struct Fix {
    uint32_t status = kStatusGeneralFailure;
    uint8_t sessionId = 0;
    bool hasLatLon = false;
    double latitude = 0, longitude = 0;
    bool hasHorUnc = false;
    float horUncCircular = 0;
    bool hasAltEllipsoid = false;
    float altEllipsoid = 0;
    bool hasAltMsl = false;
    float altMsl = 0;
    bool hasVertUnc = false;
    float vertUnc = 0;
    bool hasSpeed = false;
    float speedHorizontal = 0;
    bool hasSpeedUnc = false;
    float speedUnc = 0;
    bool hasSpeedVertical = false;
    float speedVertical = 0;
    bool hasHeading = false;
    float heading = 0;
    bool hasHeadingUnc = false;
    float headingUnc = 0;
    bool hasDop = false;
    float pdop = 0, hdop = 0, vdop = 0;
    bool hasUtc = false;
    uint64_t utcMs = 0;
    bool hasLeap = false;
    uint8_t leapSeconds = 0;
    bool hasGpsTime = false;
    uint16_t gpsWeek = 0;
    uint32_t gpsTowMs = 0;
    bool hasTimeUnc = false;
    float timeUncMs = 0;
    bool hasTech = false;
    uint32_t techMask = 0;
    bool altitudeAssumed = false;
    std::vector<uint16_t> svUsed;   // QMI numbering (GPS 1-32, SBAS 33-64, GLO 65-96, BDS 201-237, GAL 301-336, QZSS 193-197)
};

struct Sv {
    uint32_t valid = 0;
    uint32_t system = 0;
    uint16_t svId = 0;
    uint8_t health = 0;
    uint32_t status = 0;
    uint8_t infoMask = 0;
    float elevation = 0, azimuth = 0, snr = 0;
};

// Request builders (return a REQUEST message without txn; the client fills txn).
qmi::Message makeInformClientRevision(uint32_t revision);
qmi::Message makeRegEvents(uint64_t mask);
qmi::Message makeSetOperationMode(uint32_t mode);
qmi::Message makeSetNmeaTypes(uint32_t mask);
qmi::Message makeSetEngineLock(uint32_t lock);
// fixRecurrence 1 = periodic; accuracy 1 low / 2 medium / 3 high; intermediate 1 on / 2 off
qmi::Message makeStart(uint8_t sessionId, uint32_t minIntervalMs, bool intermediate, uint32_t accuracyLevel = 3);
qmi::Message makeStop(uint8_t sessionId);
qmi::Message makeInjectUtcTime(uint64_t utcMs, uint32_t uncMs);
qmi::Message makeInjectPosition(double lat, double lon, float horUncMeters);
// Coarse (cell/network or user supplied) position: adds horReliability (0x14) and positionSource (0x1D: 1 CELLID,
// 6 OTHER). Older modems ignore unknown optional TLVs.
qmi::Message makeInjectCoarsePosition(double lat, double lon, float horUncMeters, uint32_t positionSource);
qmi::Message makeDeleteAllAssistData();

// ---- XTRA (Qualcomm predicted orbits) ------------------------------------------------------------------------
// QMI_LOC_INJECT_PREDICTED_ORBITS_DATA (0x0035): TLV 0x01 u32 totalSize, 0x02 u16 totalParts, 0x03 u16 partNum
// (1-based), 0x04 partData (u16 length + bytes, <= 1024 per the loc_api_v02 IDL), optional 0x10 u32 formatType
// (0 = XTRA). The modem answers each part with a response AND an indication 0x0035 (TLV 0x01 u32 status,
// 0x10 u16 partNum); the next part is sent only after that indication (same as the Qualcomm loc client).
constexpr size_t kMaxOrbitsPart = 1024;
qmi::Message makeInjectPredictedOrbitsPart(uint32_t totalSize, uint16_t totalParts, uint16_t partNum,
                                           const uint8_t* data, size_t len, bool withFormatType = true);
qmi::Message makeGetPredictedOrbitsSource();
qmi::Message makeGetPredictedOrbitsValidity();
struct OrbitsSource {
    uint32_t status = 0xffffffff;
    bool hasSizes = false;
    uint32_t maxFileSize = 0, maxPartSize = 0;
    std::vector<std::string> servers;   // best effort: printable http(s) URLs found in TLV 0x11
};
bool parseOrbitsSourceInd(const qmi::Message& m, OrbitsSource* o);
struct OrbitsValidity {
    uint32_t status = 0xffffffff;
    bool valid = false;
    uint64_t startGpsSec = 0;   // start of validity (GPS time, seconds)
    uint16_t durationHours = 0;
};
bool parseOrbitsValidityInd(const qmi::Message& m, OrbitsValidity* v);
// Injection indication: status (0 = success) and the part it acknowledges (0 if absent).
bool parseInjectOrbitsInd(const qmi::Message& m, uint32_t* status, uint16_t* partNum);
// Splits `file` into parts of partSize (<= kMaxOrbitsPart) and returns the request messages.
std::vector<qmi::Message> buildXtraParts(const std::vector<uint8_t>& file, size_t partSize = kMaxOrbitsPart,
                                         bool withFormatType = true);
// Sanity check of an XTRA file (size limits; not a signature check - the modem validates the content).
bool looksLikeXtra(const std::vector<uint8_t>& file, std::string* why);

// Indication parsers.
bool parsePosition(const qmi::Message& m, Fix* f);
bool parseSvInfo(const qmi::Message& m, std::vector<Sv>* out, bool* altitudeAssumed);
bool parseNmea(const qmi::Message& m, std::vector<std::string>* sentences);   // split on CR/LF, trimmed
bool parseU32Status(const qmi::Message& m, uint32_t* v);                       // TLV 0x01 u32 (engine/session/ind status)

// Android-style mapping helpers (constellation numbers follow android.hardware.gnss.GnssConstellationType:
// GPS 1, SBAS 2, GLONASS 3, QZSS 4, BEIDOU 5, GALILEO 6, UNKNOWN 0).
struct AndroidSvId {
    int constellation;
    int svid;
};
AndroidSvId toAndroid(uint32_t system, uint16_t qmiId);
AndroidSvId toAndroidFromUsedId(uint16_t qmiId);   // used-SV list ids carry the system in their numeric range
double carrierHz(int androidConstellation);

// Encoders for tests / synthetic replay (inverse of the parsers).
qmi::Message makePositionInd(const Fix& f);
qmi::Message makeSvInfoInd(const std::vector<Sv>& svs);
qmi::Message makeNmeaInd(const std::string& nmea);

}  // namespace loc
}  // namespace a6l
