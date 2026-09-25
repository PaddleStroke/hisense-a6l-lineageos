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

}  // namespace a6l::rmnet
