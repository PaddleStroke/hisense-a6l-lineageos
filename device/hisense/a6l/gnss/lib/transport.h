// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): packet transport to the modem. Real implementation = AF_QIPCRTR datagram
// socket (kernel qrtr + in-kernel name service, as used by rmtfs/tqftpserv/diag-router on this phone); tests use an
// in-process fake modem with the same interface.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace a6l {

enum LogLevel { kLogDebug = 0, kLogInfo = 1, kLogWarn = 2, kLogError = 3 };
using LogFn = std::function<void(int level, const std::string& msg)>;

struct QrtrAddr {
    uint32_t node = 0;
    uint32_t port = 0;
};

constexpr uint32_t kQrtrPortCtrl = 0xfffffffeu;

enum QrtrCtrlCmd : uint32_t {
    kQrtrData = 1, kQrtrHello = 2, kQrtrBye = 3, kQrtrNewServer = 4, kQrtrDelServer = 5, kQrtrDelClient = 6,
    kQrtrResumeTx = 7, kQrtrExit = 8, kQrtrPing = 9, kQrtrNewLookup = 10, kQrtrDelLookup = 11,
};

struct QrtrCtrl {
    uint32_t cmd = 0;
    uint32_t service = 0, instance = 0, node = 0, port = 0;   // server fields (NEW/DEL_SERVER, NEW_LOOKUP)
};

bool parseCtrl(const std::vector<uint8_t>& b, QrtrCtrl* c);
std::vector<uint8_t> makeCtrl(const QrtrCtrl& c);   // 20 bytes: cmd + 4 x u32
std::vector<uint8_t> makeLookup(uint32_t service);  // service 0 = every service

class Transport {
  public:
    virtual ~Transport() = default;
    virtual bool open(std::string* err) = 0;
    virtual void close() = 0;
    // Control packet to the local name service (NEW_LOOKUP etc.).
    virtual bool sendCtrl(const std::vector<uint8_t>& pkt) = 0;
    virtual bool send(const QrtrAddr& to, const std::vector<uint8_t>& pkt) = 0;
    // 1 = packet received, 0 = timeout, -1 = error. Control packets arrive from port kQrtrPortCtrl.
    virtual int recv(QrtrAddr* from, std::vector<uint8_t>* pkt, int timeoutMs) = 0;
};

std::unique_ptr<Transport> makeQrtrTransport(LogFn log);

}  // namespace a6l
