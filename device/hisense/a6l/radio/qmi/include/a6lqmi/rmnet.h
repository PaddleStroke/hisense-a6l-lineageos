// SPDX-License-Identifier: Apache-2.0
// Hisense A6L radio (agent ril): rmnet (MAP mux) link management over rtnetlink.
// Equivalent of `ip link add link rmnet_ipa0 name rmnet_data0 type rmnet mux_id 1 [flags]`.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace a6l::rmnet {

// linux/if_link.h RMNET_FLAGS_*
enum : uint32_t {
    kIngressDeaggregation = 1u << 0,
    kIngressMapCommands = 1u << 1,
    kIngressMapCksumV4 = 1u << 2,
    kEgressMapCksumV4 = 1u << 3,
    kIngressMapCksumV5 = 1u << 4,
    kEgressMapCksumV5 = 1u << 5,
};
constexpr uint32_t kDefaultFlags = kIngressDeaggregation | kIngressMapCksumV4 | kEgressMapCksumV4;

// Pure builder (unit-tested against an iproute2 capture).
std::vector<uint8_t> buildNewLink(uint32_t seq, int parentIfindex, const std::string& name,
                                  uint16_t muxId, uint32_t flags, uint32_t flagsMask = 0xffffffffu);
std::vector<uint8_t> buildDelLink(uint32_t seq, const std::string& name);

int ifindex(const std::string& name);  // 0 if missing
bool exists(const std::string& name);
// Returns 0 on success or -errno (EEXIST is reported as success if the link already exists).
int createLink(const std::string& parent, const std::string& name, uint16_t muxId, uint32_t flags);
int deleteLink(const std::string& name);
int setUp(const std::string& name, bool up);
int setMtu(const std::string& name, int mtu);

// r5 review F14 (28 Sep 2026): the Android framework does NOT put the modem's addresses on the interface
// (ConnectivityService only programs routes/DNS/MTU from the LinkProperties; the vendor RIL/netmgr owns
// address configuration), so the HAL adds each address it reports in SetupDataCallResult.
// `cidr` = "a.b.c.d/nn" or "x::y/nn". Same RTM_NEWADDR as `ip addr add <cidr> dev <name> [nodad]`
// (IFA_LOCAL + IFA_ADDRESS; IPv6 with IFA_F_NODAD: the PDN prefix is unique to this UE, 3GPP TS 23.401
// 5.3.1.2.2), but NLM_F_REPLACE instead of NLM_F_EXCL so a repeated add is not an error.
// parseCidr: false on a malformed string (family AF_INET/AF_INET6, addr 4/16 bytes).
bool parseCidr(const std::string& cidr, int* family, std::vector<uint8_t>* addr, int* prefix);
std::vector<uint8_t> buildNewAddr(uint32_t seq, int ifindex, int family, const std::vector<uint8_t>& addr,
                                  int prefix, uint16_t nlFlags);
int addAddress(const std::string& name, const std::string& cidr);  // 0 or -errno

}  // namespace a6l::rmnet
