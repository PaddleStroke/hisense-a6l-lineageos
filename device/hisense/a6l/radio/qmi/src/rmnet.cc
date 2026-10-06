// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): rmnet link management over rtnetlink.
#include <a6lqmi/log.h>
#include <a6lqmi/rmnet.h>

#include <arpa/inet.h>
#include <errno.h>
#include <linux/if_addr.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>

namespace a6l::rmnet {

namespace {
// Attribute ids (stable kernel ABI; spelled out so the builder does not depend on header age)
constexpr uint16_t kIflaIfname = 3, kIflaLink = 5, kIflaLinkinfo = 18;
constexpr uint16_t kIflaInfoKind = 1, kIflaInfoData = 2;
constexpr uint16_t kIflaRmnetMuxId = 1, kIflaRmnetFlags = 2;
constexpr uint16_t kNlaFNested = 0;  // iproute2 does not set NLA_F_NESTED for these

struct Buf {
    std::vector<uint8_t> b;
    void align() {
        while (b.size() % 4) b.push_back(0);
    }
    void put16(uint16_t v) {
        b.push_back(v & 0xff);
        b.push_back(v >> 8);
    }
    void put32(uint32_t v) {
        for (int i = 0; i < 4; i++) b.push_back((v >> (8 * i)) & 0xff);
    }
    size_t attr(uint16_t type, const void* data, size_t len) {
        align();
        size_t at = b.size();
        put16(static_cast<uint16_t>(4 + len));
        put16(type);
        const uint8_t* p = static_cast<const uint8_t*>(data);
        b.insert(b.end(), p, p + len);
        align();
        return at;
    }
    size_t nestStart(uint16_t type) {
        align();
        size_t at = b.size();
        put16(0);
        put16(type | kNlaFNested);
        return at;
    }
    void nestEnd(size_t at) {
        align();
        uint16_t len = static_cast<uint16_t>(b.size() - at);
        b[at] = len & 0xff;
        b[at + 1] = len >> 8;
    }
    void finish() {
        uint32_t len = static_cast<uint32_t>(b.size());
        for (int i = 0; i < 4; i++) b[i] = (len >> (8 * i)) & 0xff;
    }
};

void header(Buf& m, uint16_t type, uint16_t flags, uint32_t seq) {
    m.put32(0);  // length, patched in finish()
    m.put16(type);
    m.put16(flags);
    m.put32(seq);
    m.put32(0);  // pid
    // struct ifinfomsg (16 bytes)
    for (int i = 0; i < 16; i++) m.b.push_back(0);
}

int talk(const std::vector<uint8_t>& msg) {
    int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0) return -errno;
    sockaddr_nl sa{};
    sa.nl_family = AF_NETLINK;
    if (sendto(fd, msg.data(), msg.size(), 0, reinterpret_cast<sockaddr*>(&sa), sizeof sa) < 0) {
        int e = -errno;
        close(fd);
        return e;
    }
    uint8_t buf[4096];
    ssize_t n = recv(fd, buf, sizeof buf, 0);
    close(fd);
    if (n < static_cast<ssize_t>(sizeof(nlmsghdr))) return -EIO;
    auto* h = reinterpret_cast<nlmsghdr*>(buf);
    if (h->nlmsg_type == NLMSG_ERROR) {
        auto* e = reinterpret_cast<nlmsgerr*>(NLMSG_DATA(h));
        return e->error;  // 0 = ACK
    }
    return 0;
}
}  // namespace

std::vector<uint8_t> buildNewLink(uint32_t seq, int parentIfindex, const std::string& name,
                                  uint16_t muxId, uint32_t flags, uint32_t mask) {
    Buf m;
    header(m, RTM_NEWLINK, NLM_F_REQUEST | NLM_F_ACK | NLM_F_EXCL | NLM_F_CREATE, seq);
    uint32_t link = static_cast<uint32_t>(parentIfindex);
    m.attr(kIflaLink, &link, 4);
    m.attr(kIflaIfname, name.c_str(), name.size() + 1);
    size_t li = m.nestStart(kIflaLinkinfo);
    m.attr(kIflaInfoKind, "rmnet", 5);
    size_t id = m.nestStart(kIflaInfoData);
    m.attr(kIflaRmnetMuxId, &muxId, 2);
    uint32_t fl[2] = {flags, mask};
    m.attr(kIflaRmnetFlags, fl, 8);
    m.nestEnd(id);
    m.nestEnd(li);
    m.finish();
    return m.b;
}

std::vector<uint8_t> buildDelLink(uint32_t seq, const std::string& name) {
    Buf m;
    header(m, RTM_DELLINK, NLM_F_REQUEST | NLM_F_ACK, seq);
    m.attr(kIflaIfname, name.c_str(), name.size() + 1);
    m.finish();
    return m.b;
}

bool parseCidr(const std::string& cidr, int* family, std::vector<uint8_t>* addr, int* prefix) {
    auto slash = cidr.find('/');
    std::string a = cidr.substr(0, slash);
    uint8_t b[16];
    int fam = a.find(':') != std::string::npos ? AF_INET6 : AF_INET;
    if (inet_pton(fam, a.c_str(), b) != 1) return false;
    const int max = fam == AF_INET ? 32 : 128;
    int p = max;
    if (slash != std::string::npos) {
        const std::string ps = cidr.substr(slash + 1);
        if (ps.empty() || ps.size() > 3 || ps.find_first_not_of("0123456789") != std::string::npos) return false;
        p = atoi(ps.c_str());
        if (p < 1 || p > max) return false;
    }
    *family = fam;
    addr->assign(b, b + (fam == AF_INET ? 4 : 16));
    *prefix = p;
    return true;
}

std::vector<uint8_t> buildNewAddr(uint32_t seq, int ifindex, int family, const std::vector<uint8_t>& addr,
                                  int prefix, uint16_t nlFlags) {
    Buf m;
    m.put32(0);
    m.put16(RTM_NEWADDR);
    m.put16(nlFlags);
    m.put32(seq);
    m.put32(0);
    // struct ifaddrmsg
    m.b.push_back(static_cast<uint8_t>(family));
    m.b.push_back(static_cast<uint8_t>(prefix));
    m.b.push_back(family == AF_INET6 ? IFA_F_NODAD : 0);
    m.b.push_back(0);  // RT_SCOPE_UNIVERSE
    m.put32(static_cast<uint32_t>(ifindex));
    m.attr(IFA_LOCAL, addr.data(), addr.size());
    m.attr(IFA_ADDRESS, addr.data(), addr.size());
    m.finish();
    return m.b;
}

int addAddress(const std::string& name, const std::string& cidr) {
    int fam = 0, prefix = 0;
    std::vector<uint8_t> a;
    if (!parseCidr(cidr, &fam, &a, &prefix)) return -EINVAL;
    int idx = ifindex(name);
    if (idx <= 0) return -ENODEV;
    int r = talk(buildNewAddr(3, idx, fam, a, prefix, NLM_F_REQUEST | NLM_F_ACK | NLM_F_CREATE | NLM_F_REPLACE));
    if (r) ALOGE_Q("rmnet: add address %s to %s: %s", cidr.c_str(), name.c_str(), strerror(-r));
    return r;
}

int ifindex(const std::string& name) { return static_cast<int>(if_nametoindex(name.c_str())); }
bool exists(const std::string& name) { return ifindex(name) > 0; }

int createLink(const std::string& parent, const std::string& name, uint16_t muxId, uint32_t flags) {
    int p = ifindex(parent);
    if (p <= 0) {
        ALOGE_Q("rmnet: parent %s missing (no IPA netdev?)", parent.c_str());
        return -ENODEV;
    }
    if (exists(name)) return 0;
    int r = talk(buildNewLink(1, p, name, muxId, flags));
    if (r == -EEXIST) r = 0;
    if (r) ALOGE_Q("rmnet: create %s on %s mux %u: %s", name.c_str(), parent.c_str(), muxId, strerror(-r));
    return r;
}

int deleteLink(const std::string& name) {
    if (!exists(name)) return 0;
    return talk(buildDelLink(2, name));
}

int setUp(const std::string& name, bool up) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    ifreq ifr{};
    strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    int r = 0;
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        r = -errno;
    } else {
        if (up) ifr.ifr_flags |= IFF_UP;
        else ifr.ifr_flags &= ~IFF_UP;
        if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) r = -errno;
    }
    close(fd);
    return r;
}

int setMtu(const std::string& name, int mtu) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -errno;
    ifreq ifr{};
    strncpy(ifr.ifr_name, name.c_str(), IFNAMSIZ - 1);
    ifr.ifr_mtu = mtu;
    int r = ioctl(fd, SIOCSIFMTU, &ifr) < 0 ? -errno : 0;
    close(fd);
    return r;
}

}  // namespace a6l::rmnet
