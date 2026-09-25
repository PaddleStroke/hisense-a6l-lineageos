// SPDX-License-Identifier: Apache-2.0
// A6L GNSS (agent gnss, 24 Sep 2026): AF_QIPCRTR transport (Linux uapi <linux/qrtr.h>, redefined locally so the
// file builds with any libc/NDK).
#include "transport.h"

#include <errno.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>

#include "qmi.h"

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

namespace a6l {

namespace {

struct SockaddrQrtr {
    unsigned short sq_family;
    uint32_t sq_node;
    uint32_t sq_port;
};

class QrtrTransport : public Transport {
  public:
    explicit QrtrTransport(LogFn log) : log_(std::move(log)) {}
    ~QrtrTransport() override { close(); }

    bool open(std::string* err) override {
        close();
        fd_ = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) {
            if (err) *err = std::string("socket(AF_QIPCRTR): ") + strerror(errno);
            return false;
        }
        SockaddrQrtr sq{};
        socklen_t sl = sizeof(sq);
        if (getsockname(fd_, reinterpret_cast<sockaddr*>(&sq), &sl) < 0 || sq.sq_family != AF_QIPCRTR) {
            if (err) *err = std::string("getsockname: ") + strerror(errno);
            close();
            return false;
        }
        localNode_ = sq.sq_node;
        if (log_) log_(kLogDebug, "qrtr socket open, local node " + std::to_string(localNode_) + " port " +
                                       std::to_string(sq.sq_port));
        return true;
    }

    void close() override {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
    }

    bool sendCtrl(const std::vector<uint8_t>& pkt) override {
        return send(QrtrAddr{localNode_, kQrtrPortCtrl}, pkt);
    }

    bool send(const QrtrAddr& to, const std::vector<uint8_t>& pkt) override {
        if (fd_ < 0) return false;
        SockaddrQrtr sq{};
        sq.sq_family = AF_QIPCRTR;
        sq.sq_node = to.node;
        sq.sq_port = to.port;
        ssize_t n = sendto(fd_, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&sq), sizeof(sq));
        if (n != ssize_t(pkt.size())) {
            if (log_) log_(kLogWarn, std::string("qrtr sendto: ") + strerror(errno));
            return false;
        }
        return true;
    }

    int recv(QrtrAddr* from, std::vector<uint8_t>* pkt, int timeoutMs) override {
        if (fd_ < 0) return -1;
        pollfd p{fd_, POLLIN, 0};
        int r = poll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
        pkt->resize(65536);
        SockaddrQrtr sq{};
        socklen_t sl = sizeof(sq);
        ssize_t n = recvfrom(fd_, pkt->data(), pkt->size(), 0, reinterpret_cast<sockaddr*>(&sq), &sl);
        if (n < 0) {
            // ENETRESET etc. after a remote restart: report as error so the caller reopens
            if (errno == EINTR || errno == EAGAIN) return 0;
            if (log_) log_(kLogWarn, std::string("qrtr recvfrom: ") + strerror(errno));
            return -1;
        }
        pkt->resize(size_t(n));
        from->node = sq.sq_node;
        from->port = sq.sq_port;
        return 1;
    }

  private:
    LogFn log_;
    int fd_ = -1;
    uint32_t localNode_ = 1;
};

}  // namespace

bool parseCtrl(const std::vector<uint8_t>& b, QrtrCtrl* c) {
    qmi::Reader r(b);
    if (!r.u32(&c->cmd)) return false;
    c->service = c->instance = c->node = c->port = 0;
    if (c->cmd == kQrtrNewServer || c->cmd == kQrtrDelServer || c->cmd == kQrtrNewLookup) {
        return r.u32(&c->service) && r.u32(&c->instance) && r.u32(&c->node) && r.u32(&c->port);
    }
    if (c->cmd == kQrtrBye || c->cmd == kQrtrDelClient || c->cmd == kQrtrHello) {
        // client {node, port} (BYE carries only the node on some kernels)
        r.u32(&c->node);
        r.u32(&c->port);
    }
    return true;
}

std::vector<uint8_t> makeCtrl(const QrtrCtrl& c) {
    qmi::Writer w;
    w.u32(c.cmd).u32(c.service).u32(c.instance).u32(c.node).u32(c.port);
    return w.b;
}

std::vector<uint8_t> makeLookup(uint32_t service) {
    QrtrCtrl c;
    c.cmd = kQrtrNewLookup;
    c.service = service;
    return makeCtrl(c);
}

std::unique_ptr<Transport> makeQrtrTransport(LogFn log) { return std::make_unique<QrtrTransport>(std::move(log)); }

}  // namespace a6l
