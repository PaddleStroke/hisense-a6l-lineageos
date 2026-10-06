// SPDX-License-Identifier: Apache-2.0
// Hisense A6L (agent volte4, 27 Sep 2026): EFS2-over-DIAG client for the modem EFS (item files such as
// /nv/item_files/ims/IMS_enable), used by a6l-volte-cfg. Transport = the linux-msm diag-router UNIX socket
// (abstract "\0diag", SOCK_SEQPACKET, raw packets, no HDLC): the router forwards subsystem 0x4B/0x13 commands to the
// modem over QRTR 4097 and broadcasts the modem's responses to every UNIX client.
//
// Wire layouts (little-endian, packed; opcode numbering of libopenpst/EfsTools, PUT/GET item layouts as in qfenix
// (verified against QPST) and qcom-efs-browser (verified on SM8350 hardware); both agree):
//   header           : u8 0x4B | u8 subsys (0x13) | le16 opcode
//   HELLO  (0)       : 6 x u32 windows, u32 version, u32 min, u32 max, u32 features  -> echo header + same fields
//   OPEN   (2)       : i32 oflag, i32 mode, path\0               -> i32 fd, i32 errno
//   CLOSE  (3)       : i32 fd                                    -> i32 errno
//   READ   (4)       : i32 fd, u32 nbytes, u32 offset            -> i32 fd, u32 offset, i32 nread, i32 errno, data
//   UNLINK (8)       : path\0                                    -> i32 errno
//   OPENDIR(11)      : path\0                                    -> i32 dirp, i32 errno
//   READDIR(12)      : i32 dirp, i32 seqno                       -> i32 dirp, i32 seqno, i32 errno, i32 type, i32 mode,
//                                                                   i32 size, i32 atime, mtime, ctime, name\0 (empty = end)
//   CLOSEDIR(13)     : i32 dirp                                  -> i32 errno
//   STAT   (15)      : path\0                                    -> i32 errno, i32 mode, size, nlink, atime, mtime, ctime
//   PUT    (38)      : le16 len, u16 pad, i32 flags, i16 mode, data[len], path\0 -> i16 perm?, i16 errno, i16 written
//   GET    (39)      : path\0                                    -> i32 len, i32 errno, data[len]
//   SYNC_NO_WAIT(48) : u16 seq, path\0                           -> u16 seq, u32 token, i32 errno
//   SYNC_GET_STATUS(49): u16 seq, u32 token, path\0              -> u16 seq, u8 done, i32 errno
//   router errors    : u8 0x13/0x14/0x15 (bad command/params/length) + the request echoed
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace a6l::volte4 {

class DiagTransport {
  public:
    virtual ~DiagTransport() = default;
    virtual bool send(const std::vector<uint8_t>& pkt) = 0;
    // 1 = packet, 0 = timeout, -1 = error/closed
    virtual int recv(std::vector<uint8_t>* pkt, int timeoutMs) = 0;
};
// Connects to the diag-router abstract socket ("\0" + name). nullptr on failure (errno kept).
std::unique_ptr<DiagTransport> makeDiagUnixTransport(const std::string& name = "diag");

namespace efs {
constexpr uint8_t kCmdSubsys = 0x4B;
constexpr uint8_t kSubsysFs = 0x13;
enum Op : uint16_t {
    kHello = 0, kQuery = 1, kOpen = 2, kClose = 3, kRead = 4, kWrite = 5, kUnlink = 8, kMkdir = 9,
    kOpendir = 11, kReaddir = 12, kClosedir = 13, kStat = 15, kPut = 38, kGet = 39, kSyncNoWait = 48,
    kSyncGetStatus = 49,
};
// EFS2 open flags (POSIX numbering, as the modem expects)
constexpr int32_t kORdonly = 0x0, kOWronly = 0x1, kORdwr = 0x2, kOCreat = 0x40, kOTrunc = 0x200,
                  kOItemfile = 0x40000, kOAutodir = 0x80000;
constexpr int32_t kPutFlags = kOCreat | kOWronly | kOTrunc | kOItemfile | kOAutodir;  // 0xC0241
constexpr int16_t kPutMode = 0x1FF;
constexpr int kEnoent = 2;
constexpr int kTransportError = -1;  // any negative = no valid answer
const char* errnoName(int e);

std::vector<uint8_t> encodeGet(uint8_t subsys, const std::string& path);
std::vector<uint8_t> encodePut(uint8_t subsys, const std::string& path, const std::vector<uint8_t>& data,
                               int32_t flags = kPutFlags, int16_t mode = kPutMode);
std::vector<uint8_t> encodePath(uint8_t subsys, uint16_t op, const std::string& path);
}  // namespace efs

struct EfsStat {
    int32_t mode = 0, size = 0, nlink = 0, atime = 0, mtime = 0, ctime = 0;
};
struct EfsDirent {
    int32_t type = 0, mode = 0, size = 0;
    std::string name;
};

class Efs2 {
  public:
    explicit Efs2(DiagTransport* t, uint8_t subsys = efs::kSubsysFs, int timeoutMs = 4000)
        : mT(t), mSubsys(subsys), mTimeout(timeoutMs) {}
    // All calls return the modem's EFS errno (0 = ok) or a negative value (no/invalid answer, see lastError()).
    int hello(uint32_t* version = nullptr);
    int get(const std::string& path, std::vector<uint8_t>* data);
    int put(const std::string& path, const std::vector<uint8_t>& data);
    int stat(const std::string& path, EfsStat* st);
    int unlink(const std::string& path);
    int readFile(const std::string& path, std::vector<uint8_t>* data);  // OPEN/READ/CLOSE (regular files)
    int list(const std::string& dir, std::vector<EfsDirent>* out);
    int sync();  // commit the EFS journal (so the change reaches rmtfs and survives a modem restart)
    // item read: GET first, then the file interface (some items are regular files)
    int read(const std::string& path, std::vector<uint8_t>* data);
    const std::string& lastError() const { return mErr; }
    uint8_t subsys() const { return mSubsys; }
    int ignoredPackets() const { return mIgnored; }

  private:
    int xfer(const std::vector<uint8_t>& req, uint16_t op, std::vector<uint8_t>* rsp);
    DiagTransport* mT;
    uint8_t mSubsys;
    int mTimeout;
    std::string mErr;
    int mIgnored = 0;
};

}  // namespace a6l::volte4
