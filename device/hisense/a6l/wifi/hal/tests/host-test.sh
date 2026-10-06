#!/usr/bin/env bash
# usage: host-test.sh <lineage tree> <work dir>   (g++ on the build host; libnl/cutils/log from shims or tree headers)
set -eu
T=$1; W=$2; S=$(cd "$(dirname "$0")/.." && pwd); mkdir -p $W; rm -rf $W/net; mkdir -p $W/net
# nl80211 functions are only linked, not called, by the lifecycle test
cat > $W/nlstub.cpp <<'C'
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
extern "C" {
nl_sock* nl_socket_alloc() { return nullptr; }
void nl_socket_free(nl_sock*) {}
int genl_connect(nl_sock*) { return -1; }
int genl_ctrl_resolve(nl_sock*, const char*) { return -1; }
int nl_socket_get_fd(const nl_sock*) { return -1; }
nl_msg* nlmsg_alloc() { return nullptr; }
void nlmsg_free(nl_msg*) {}
void* genlmsg_put(nl_msg*, uint32_t, uint32_t, int, int, int, uint8_t, uint8_t) { return nullptr; }
int nla_put_u32(nl_msg*, int, uint32_t) { return -1; }
int nla_put_flag(nl_msg*, int) { return -1; }
int nla_put_string(nl_msg*, int, const char*) { return -1; }
nl_cb* nl_cb_alloc(enum nl_cb_kind) { return nullptr; }
int nl_cb_err(nl_cb*, enum nl_cb_kind, nl_recvmsg_err_cb_t, void*) { return 0; }
int nl_cb_set(nl_cb*, enum nl_cb_type, enum nl_cb_kind, nl_recvmsg_msg_cb_t, void*) { return 0; }
void nl_cb_put(nl_cb*) {}
int nl_send_auto(nl_sock*, nl_msg*) { return -1; }
int nl_recvmsgs(nl_sock*, nl_cb*) { return -1; }
nlmsghdr* nlmsg_hdr(nl_msg*) { return nullptr; }
void* nlmsg_data(const nlmsghdr*) { return nullptr; }
nlattr* genlmsg_attrdata(const genlmsghdr*, int) { return nullptr; }
int genlmsg_attrlen(const genlmsghdr*, int) { return 0; }
int nla_parse(nlattr**, int, nlattr*, int, const nla_policy*) { return -1; }
uint32_t nla_get_u32(const nlattr*) { return 0; }
void* nla_data(const nlattr*) { return nullptr; }
int nla_len(const nlattr*) { return 0; }
int nla_ok(const nlattr*, int) { return 0; }
nlattr* nla_next(const nlattr*, int*) { return nullptr; }
}
C
INC="-isystem $T/hardware/interfaces/wifi/legacy_headers/include -isystem $T/external/libnl/include -I$T/system/core/libcutils/include -I$T/system/logging/liblog/include"
g++ -std=gnu++20 -O1 -g -Wall -Wextra -Werror -Wno-missing-field-initializers -fsanitize=address,undefined \
    -DA6L_SYSFS_NET="\"$W/net\"" -include stdarg.h $INC \
    $S/a6l_wifi_hal.cpp $S/tests/host-test.cpp $W/nlstub.cpp -o $W/host-test -lpthread
$W/host-test
