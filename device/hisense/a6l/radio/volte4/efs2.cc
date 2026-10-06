// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): EFS2-over-DIAG client (see efs2.h for the wire layouts).
#include "efs2.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace a6l::volte4 {

namespace {
void le16(std::vector<uint8_t>* v, uint16_t x) { v->push_back(x & 0xff); v->push_back(x >> 8); }
void le32(std::vector<uint8_t>* v, uint32_t x) { for (int i = 0; i < 4; i++) v->push_back((x >> (8 * i)) & 0xff); }
int32_t gi32(const std::vector<uint8_t>& v, size_t o) {
    return static_cast<int32_t>(v[o] | (v[o + 1] << 8) | (v[o + 2] << 16) | (static_cast<uint32_t>(v[o + 3]) << 24));
}
int16_t gi16(const std::vector<uint8_t>& v, size_t o) { return static_cast<int16_t>(v[o] | (v[o + 1] << 8)); }
std::vector<uint8_t> hdr(uint8_t subsys, uint16_t op) {
    std::vector<uint8_t> v{efs::kCmdSubsys, subsys};
    le16(&v, op);
    return v;
}
void pathz(std::vector<uint8_t>* v, const std::string& p) {
    v->insert(v->end(), p.begin(), p.end());
    v->push_back(0);
}

class UnixDiag : public DiagTransport {
  public:
    ~UnixDiag() override { if (mFd >= 0) close(mFd); }
    bool open(const std::string& name) {
        mFd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        if (mFd < 0) return false;
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (name.size() + 1 > sizeof a.sun_path) return false;
        memcpy(a.sun_path + 1, name.data(), name.size());  // abstract: leading NUL
        // diag-router binds sizeof(sockaddr_un) with "\0diag" + zero padding: the abstract name is the whole
        // sun_path, so connect with the same length
        socklen_t l = sizeof a;
        return connect(mFd, reinterpret_cast<sockaddr*>(&a), l) == 0;
    }
    bool send(const std::vector<uint8_t>& p) override {
        return ::send(mFd, p.data(), p.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(p.size());
    }
    int recv(std::vector<uint8_t>* p, int timeoutMs) override {
        pollfd pf{mFd, POLLIN, 0};
        int r = poll(&pf, 1, timeoutMs);
        if (r == 0) return 0;
        if (r < 0) return -1;
        p->resize(65536);
        ssize_t n = ::recv(mFd, p->data(), p->size(), 0);
        if (n <= 0) return -1;
        p->resize(n);
        return 1;
    }

  private:
    int mFd = -1;
};
}  // namespace

std::unique_ptr<DiagTransport> makeDiagUnixTransport(const std::string& name) {
    auto t = std::make_unique<UnixDiag>();
    if (!t->open(name)) return nullptr;
    return t;
}

namespace efs {
const char* errnoName(int e) {
    switch (e) {
        case 0: return "ok";
        case 1: return "EPERM";
        case 2: return "ENOENT";
        case 5: return "EIO";
        case 9: return "EBADF";
        case 12: return "ENOMEM";
        case 13: return "EACCES";
        case 17: return "EEXIST";
        case 20: return "ENOTDIR";
        case 21: return "EISDIR";
        case 22: return "EINVAL";
        case 24: return "EMFILE";
        case 27: return "EFBIG";
        case 28: return "ENOSPC";
        case 36: return "ENAMETOOLONG";
        case 39: return "ENOTEMPTY";
        default: return e < 0 ? "no-answer" : "efs-errno";
    }
}
std::vector<uint8_t> encodePath(uint8_t subsys, uint16_t op, const std::string& path) {
    auto v = hdr(subsys, op);
    pathz(&v, path);
    return v;
}
std::vector<uint8_t> encodeGet(uint8_t subsys, const std::string& path) { return encodePath(subsys, kGet, path); }
std::vector<uint8_t> encodePut(uint8_t subsys, const std::string& path, const std::vector<uint8_t>& data,
                               int32_t flags, int16_t mode) {
    auto v = hdr(subsys, kPut);
    le16(&v, static_cast<uint16_t>(data.size()));
    le16(&v, 0);
    le32(&v, static_cast<uint32_t>(flags));
    le16(&v, static_cast<uint16_t>(mode));
    v.insert(v.end(), data.begin(), data.end());
    pathz(&v, path);
    return v;
}
}  // namespace efs

int Efs2::xfer(const std::vector<uint8_t>& req, uint16_t op, std::vector<uint8_t>* rsp) {
    mErr.clear();
    if (!mT || !mT->send(req)) {
        mErr = "diag send failed";
        return efs::kTransportError;
    }
    auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(mTimeout);
    for (;;) {
        int left = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end - std::chrono::steady_clock::now()).count());
        if (left <= 0) {
            mErr = "timeout waiting for EFS2 op " + std::to_string(op);
            return efs::kTransportError;
        }
        std::vector<uint8_t> p;
        int r = mT->recv(&p, left);
        if (r < 0) {
            mErr = "diag socket closed";
            return efs::kTransportError;
        }
        if (r == 0) continue;
        // router refusal: error code + our request echoed
        if (p.size() >= 1 + 4 && (p[0] == 0x13 || p[0] == 0x14 || p[0] == 0x15) && p[1] == efs::kCmdSubsys &&
            p[2] == mSubsys && (p[3] | (p[4] << 8)) == op) {
            mErr = p[0] == 0x13 ? "diag: bad command (EFS2 subsystem not registered by the modem?)"
                   : p[0] == 0x14 ? "diag: bad parameters" : "diag: bad length";
            return efs::kTransportError;
        }
        if (p.size() >= 4 && p[0] == efs::kCmdSubsys && p[1] == mSubsys && (p[2] | (p[3] << 8)) == op) {
            *rsp = std::move(p);
            return 0;
        }
        mIgnored++;  // log/event/other client's response
    }
}

int Efs2::hello(uint32_t* version) {
    auto v = hdr(mSubsys, efs::kHello);
    for (int i = 0; i < 6; i++) le32(&v, 0x100000);
    le32(&v, 1);
    le32(&v, 1);
    le32(&v, 1);
    le32(&v, 0xFFFFFFFFu);
    std::vector<uint8_t> r;
    if (int e = xfer(v, efs::kHello, &r); e) return e;
    if (r.size() >= 4 + 7 * 4 && version) *version = static_cast<uint32_t>(gi32(r, 4 + 6 * 4));
    return 0;
}

int Efs2::get(const std::string& path, std::vector<uint8_t>* data) {
    std::vector<uint8_t> r;
    if (int e = xfer(efs::encodeGet(mSubsys, path), efs::kGet, &r); e) return e;
    if (r.size() < 12) { mErr = "GET short answer"; return efs::kTransportError; }
    int32_t len = gi32(r, 4), err = gi32(r, 8);
    if (err) { mErr = std::string("GET ") + path + ": " + efs::errnoName(err); return err; }
    if (len < 0 || static_cast<size_t>(len) > r.size() - 12) { mErr = "GET length mismatch"; return efs::kTransportError; }
    data->assign(r.begin() + 12, r.begin() + 12 + len);
    return 0;
}

int Efs2::put(const std::string& path, const std::vector<uint8_t>& data) {
    if (data.size() > 6144) { mErr = "item too large for PUT"; return efs::kTransportError; }
    std::vector<uint8_t> r;
    if (int e = xfer(efs::encodePut(mSubsys, path, data), efs::kPut, &r); e) return e;
    if (r.size() < 8) { mErr = "PUT short answer"; return efs::kTransportError; }
    int err = gi16(r, 6);
    if (err) mErr = std::string("PUT ") + path + ": " + efs::errnoName(err);
    return err;
}

int Efs2::stat(const std::string& path, EfsStat* st) {
    std::vector<uint8_t> r;
    if (int e = xfer(efs::encodePath(mSubsys, efs::kStat, path), efs::kStat, &r); e) return e;
    if (r.size() < 8) { mErr = "STAT short answer"; return efs::kTransportError; }
    int err = gi32(r, 4);
    if (err) { mErr = std::string("STAT ") + path + ": " + efs::errnoName(err); return err; }
    if (r.size() < 32) { mErr = "STAT short answer"; return efs::kTransportError; }
    st->mode = gi32(r, 8); st->size = gi32(r, 12); st->nlink = gi32(r, 16);
    st->atime = gi32(r, 20); st->mtime = gi32(r, 24); st->ctime = gi32(r, 28);
    return 0;
}

int Efs2::unlink(const std::string& path) {
    std::vector<uint8_t> r;
    if (int e = xfer(efs::encodePath(mSubsys, efs::kUnlink, path), efs::kUnlink, &r); e) return e;
    if (r.size() < 8) { mErr = "UNLINK short answer"; return efs::kTransportError; }
    int err = gi32(r, 4);
    if (err) mErr = std::string("UNLINK ") + path + ": " + efs::errnoName(err);
    return err;
}

int Efs2::readFile(const std::string& path, std::vector<uint8_t>* data) {
    auto v = hdr(mSubsys, efs::kOpen);
    le32(&v, efs::kORdonly);
    le32(&v, 0);
    pathz(&v, path);
    std::vector<uint8_t> r;
    if (int e = xfer(v, efs::kOpen, &r); e) return e;
    if (r.size() < 12) { mErr = "OPEN short answer"; return efs::kTransportError; }
    int32_t fd = gi32(r, 4), err = gi32(r, 8);
    if (err || fd < 0) { mErr = std::string("OPEN ") + path + ": " + efs::errnoName(err); return err ? err : efs::kEnoent; }
    data->clear();
    int rc = 0;
    for (uint32_t off = 0; off < (1u << 20);) {
        auto q = hdr(mSubsys, efs::kRead);
        le32(&q, static_cast<uint32_t>(fd));
        le32(&q, 1024);
        le32(&q, off);
        if (int e = xfer(q, efs::kRead, &r); e) { rc = e; break; }
        if (r.size() < 20) { mErr = "READ short answer"; rc = efs::kTransportError; break; }
        int32_t n = gi32(r, 12), e2 = gi32(r, 16);
        if (e2) { rc = e2; mErr = std::string("READ: ") + efs::errnoName(e2); break; }
        if (n <= 0) break;
        if (static_cast<size_t>(n) > r.size() - 20) n = static_cast<int32_t>(r.size() - 20);
        data->insert(data->end(), r.begin() + 20, r.begin() + 20 + n);
        off += n;
        if (n < 1024) break;
    }
    auto c = hdr(mSubsys, efs::kClose);
    le32(&c, static_cast<uint32_t>(fd));
    std::string keep = mErr;
    xfer(c, efs::kClose, &r);
    mErr = keep;
    return rc;
}

int Efs2::read(const std::string& path, std::vector<uint8_t>* data) {
    int e = get(path, data);
    if (e == 0 || e < 0) return e;
    std::vector<uint8_t> f;
    int e2 = readFile(path, &f);
    if (e2 == 0) { *data = std::move(f); return 0; }
    return e;  // report the GET errno (ENOENT for a missing item)
}

int Efs2::list(const std::string& dir, std::vector<EfsDirent>* out) {
    std::vector<uint8_t> r;
    if (int e = xfer(efs::encodePath(mSubsys, efs::kOpendir, dir), efs::kOpendir, &r); e) return e;
    if (r.size() < 12) { mErr = "OPENDIR short answer"; return efs::kTransportError; }
    int32_t dirp = gi32(r, 4), err = gi32(r, 8);
    if (err) { mErr = std::string("OPENDIR ") + dir + ": " + efs::errnoName(err); return err; }
    int rc = 0;
    for (uint32_t seq = 1; seq < 4096; seq++) {
        auto q = hdr(mSubsys, efs::kReaddir);
        le32(&q, static_cast<uint32_t>(dirp));
        le32(&q, seq);
        if (int e = xfer(q, efs::kReaddir, &r); e) { rc = e; break; }
        if (r.size() < 16) { rc = efs::kTransportError; mErr = "READDIR short"; break; }
        if (int e2 = gi32(r, 12); e2) { rc = e2; break; }
        if (r.size() <= 40 || r[40] == 0) break;
        EfsDirent d;
        d.type = gi32(r, 16); d.mode = gi32(r, 20); d.size = gi32(r, 24);
        d.name.assign(reinterpret_cast<const char*>(&r[40]), strnlen(reinterpret_cast<const char*>(&r[40]), r.size() - 40));
        out->push_back(d);
    }
    auto c = hdr(mSubsys, efs::kClosedir);
    le32(&c, static_cast<uint32_t>(dirp));
    std::string keep = mErr;
    xfer(c, efs::kClosedir, &r);
    mErr = keep;
    return rc;
}

int Efs2::sync() {
    uint32_t token = 0;
    int err = -1;
    std::vector<uint8_t> r;
    for (int attempt = 0; attempt < 6; attempt++) {
        auto v = hdr(mSubsys, efs::kSyncNoWait);
        le16(&v, 1);
        pathz(&v, "/");
        if (int e = xfer(v, efs::kSyncNoWait, &r); e) return e;
        if (r.size() < 14) { mErr = "SYNC short answer"; return efs::kTransportError; }
        token = static_cast<uint32_t>(gi32(r, 6));
        err = gi32(r, 10);
        if (!err) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));  // 306 = a commit still settling
    }
    if (err) { mErr = "SYNC refused: efs errno " + std::to_string(err); return err; }
    for (int i = 0; i < 20; i++) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto v = hdr(mSubsys, efs::kSyncGetStatus);
        le16(&v, 1);
        le32(&v, token);
        pathz(&v, "/");
        if (xfer(v, efs::kSyncGetStatus, &r)) continue;
        if (r.size() >= 7 && r[6] == 0) return 0;  // status 0 = commit done
    }
    return 0;  // started; some modems never report "done" although the commit happened (qcom-efs-browser note)
}

}  // namespace a6l::volte4
