// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): QMI PDC (service 36, Persistent Device Configuration) client for MBN
// selection / activation / loading. TLV layouts from the stock IDL (libqmiservices.so, PDC IDL 1.15, dumped with
// tools/volte/idldump.py); every request is answered by a response (result only) and then by an indication that
// carries the real result (enum16 TLV 0x01) and our token (TLV 0x10).
//   0x20 Register        : 0x10 u8 enable_reporting, 0x11 u8 enable_refresh
//   0x22 Get Selected    : 0x01 u32 type, 0x10 token           -> ind 0x11 active id (len8), 0x12 pending id (len8)
//   0x23 Set Selected    : 0x01 {u32 type, u8 len, id}, 0x10 token
//   0x24 List Configs    : 0x10 token, 0x11 u32 type             -> ind 0x11 {u8 n, n x {u32 type, u8 len, id}}
//   0x26 Load Config     : 0x01 {u32 type, u8 len, id, u32 total_size, u16 len, chunk (<= 32768)}, 0x10 token
//                          -> resp 0x10 u8 frame_reset; ind 0x11 u32 received, 0x12 u32 remaining, 0x13 u8 frame_reset
//   0x27 Activate        : 0x01 u32 type, 0x10 token             (the modem resets itself afterwards)
//   0x28 Get Config Info : 0x01 {u32 type, u8 len, id}, 0x10 token -> ind 0x11 u32 size, 0x12 desc (len8), 0x13 u32 ver
//   0x2b Deactivate      : 0x01 u32 type, 0x10 token
// Config ids of loaded MBNs: SHA-1 of the file (as libqmi qmicli --pdc-load-config does). Chunk: 1024 B (libqmi).
#pragma once

#include <a6lqmi/client.h>

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace a6l::volte4 {

namespace pdc {
constexpr uint32_t kSvcPdc = 36;
enum Msg : uint16_t {
    kRegister = 0x20, kConfigChangeInd = 0x21, kGetSelected = 0x22, kSetSelected = 0x23, kListConfigs = 0x24,
    kLoadConfig = 0x26, kActivate = 0x27, kGetInfo = 0x28, kDeactivate = 0x2b,
};
constexpr uint32_t kTypeHw = 0, kTypeSw = 1;
std::vector<uint8_t> typeWithId(uint32_t type, const std::vector<uint8_t>& id);
std::vector<uint8_t> loadChunk(uint32_t type, const std::vector<uint8_t>& id, uint32_t total,
                               const uint8_t* chunk, size_t len);
}  // namespace pdc

struct PdcInfo {
    std::vector<uint8_t> id;
    uint32_t size = 0, version = 0;
    std::string desc;
    bool ok = false;
};
struct PdcSelected {
    std::optional<std::vector<uint8_t>> active, pending;
    int indResult = -1;
};

class Pdc {
  public:
    explicit Pdc(qmi::Client& c, int timeoutMs = 8000);
    bool registerReporting();
    // Each returns the indication result (0 = success), or -1 (no response/indication), or the QMI error of the
    // response + 0x10000 (so "response refused" and "indication failed" are told apart).
    int getSelected(uint32_t type, PdcSelected* out);
    int list(uint32_t type, std::vector<std::vector<uint8_t>>* ids);
    int info(uint32_t type, const std::vector<uint8_t>& id, PdcInfo* out);
    int setSelected(uint32_t type, const std::vector<uint8_t>& id);
    int activate(uint32_t type);
    int deactivate(uint32_t type);
    // Chunked load. progress(received, remaining) after each indication. chunk <= 32768.
    int load(uint32_t type, const std::vector<uint8_t>& id, const std::vector<uint8_t>& data, size_t chunk,
             const std::function<void(uint32_t, uint32_t)>& progress);
    std::string lastError() const { return mErr; }

  private:
    int call(uint16_t msgId, qmi::Message req, qmi::Message* ind);
    std::optional<qmi::Message> waitInd(uint16_t msgId, uint32_t token, int timeoutMs);
    qmi::Client& mC;
    int mTimeout;
    uint32_t mToken = 0xA6400000;
    std::string mErr;
    struct Q {
        std::mutex m;
        std::condition_variable cv;
        std::deque<qmi::Message> q;
    };
    std::shared_ptr<Q> mQ;
};

std::string idHex(const std::vector<uint8_t>& id);
// SHA-1 (FIPS 180-1), 20 bytes; used for the config id of a loaded MBN.
std::vector<uint8_t> sha1(const uint8_t* p, size_t n);

// MCFG MBN sanity info (ELF32 with an 'MCFG' segment): config type (0 HW, 1 SW), item count, trailer name/version.
struct MbnInfo {
    bool ok = false;
    uint16_t format = 0, type = 0;
    uint32_t items = 0, version = 0;
    std::string name, err;
};
MbnInfo parseMbn(const std::vector<uint8_t>& f);

}  // namespace a6l::volte4
