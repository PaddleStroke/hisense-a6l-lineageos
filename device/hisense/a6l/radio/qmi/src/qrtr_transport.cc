// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): AF_QIPCRTR datagram transport (kernel QRTR, in-kernel name service).
#include <a6lqmi/client.h>
#include <a6lqmi/log.h>

#include <errno.h>
#include <stdint.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#if __has_include(<linux/qrtr.h>)
#include <linux/qrtr.h>
#else
// Old host sysroots (Soong's glibc 2.17 prebuilt) lack the uapi header: same layout as linux/qrtr.h.
struct sockaddr_qrtr {
    unsigned short sq_family;
    uint32_t sq_node;
    uint32_t sq_port;
};
#endif

#ifndef AF_QIPCRTR
#define AF_QIPCRTR 42
#endif

namespace a6l::qmi {

namespace {

class QrtrTransport : public Transport {
  public:
    ~QrtrTransport() override { close(); }

    bool open() override {
        mFd = socket(AF_QIPCRTR, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        if (mFd < 0) {
            ALOGE_Q("socket(AF_QIPCRTR): %s (qrtr module loaded?)", strerror(errno));
            return false;
        }
        sockaddr_qrtr sq{};
        socklen_t sl = sizeof sq;
        if (getsockname(mFd, reinterpret_cast<sockaddr*>(&sq), &sl) < 0 ||
            sq.sq_family != AF_QIPCRTR) {
            ALOGE_Q("getsockname(qrtr): %s", strerror(errno));
            ::close(mFd);
            mFd = -1;
            return false;
        }
        mNode = sq.sq_node;
        mPort = sq.sq_port;
        ALOGI_Q("qrtr socket bound to %u:%u", mNode, mPort);
        return true;
    }

    void close() override {
        if (mFd >= 0) ::close(mFd);
        mFd = -1;
    }

    uint32_t localNode() const override { return mNode; }

    bool send(const Addr& to, const std::vector<uint8_t>& data) override {
        sockaddr_qrtr sq{};
        sq.sq_family = AF_QIPCRTR;
        sq.sq_node = to.node;
        sq.sq_port = to.port;
        ssize_t n = sendto(mFd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&sq),
                           sizeof sq);
        if (n < 0) {
            ALOGE_Q("qrtr sendto %u:%u: %s", to.node, to.port, strerror(errno));
            return false;
        }
        return true;
    }

    int recv(Addr* from, std::vector<uint8_t>* data, int timeoutMs) override {
        if (mFd < 0) return -1;
        pollfd p{mFd, POLLIN, 0};
        int r = poll(&p, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return errno == EINTR ? 0 : -1;
        data->resize(65536);
        sockaddr_qrtr sq{};
        socklen_t sl = sizeof sq;
        ssize_t n = recvfrom(mFd, data->data(), data->size(), 0, reinterpret_cast<sockaddr*>(&sq),
                             &sl);
        if (n < 0) {
            // ENETRESET is reported once when the remote node restarts; keep the socket.
            if (errno == EINTR || errno == EAGAIN || errno == ENETRESET) return 0;
            ALOGE_Q("qrtr recvfrom: %s", strerror(errno));
            return -1;
        }
        data->resize(n);
        from->node = sq.sq_node;
        from->port = sq.sq_port;
        return 1;
    }

  private:
    int mFd = -1;
    uint32_t mNode = 0, mPort = 0;
};

}  // namespace

std::unique_ptr<Transport> makeQrtrTransport() { return std::make_unique<QrtrTransport>(); }

}  // namespace a6l::qmi
