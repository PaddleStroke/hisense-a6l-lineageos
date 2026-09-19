#!/usr/bin/env bash
# V67 PHONE kernel CANDIDATE (build only): validated V38 android-init configuration + the V59 Android
# networking set built in + QCOM_RMTFS_MEM, with ALL modules rebuilt against it so the prepared peripheral
# bundles can be regenerated consistently. Produces an archive; never packages, flashes or boots anything.
set -euo pipefail
workspace=/mnt/c/Users/Pierre/Desktop/A6L
kernel_dir=/home/a6l/kernel/a6l-baseline-7.2
out=/home/a6l/kernel/out-a6l-phone-v67
v50=$workspace/firmware/extracted/android-init-kernel-20260917
archive=$workspace/firmware/extracted/phone-kernel-v67-candidate-$(date +%Y%m%d)
module_dir=/home/a6l/kernel/a6l-simplefb-phone-v67
clang_dir=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
test "$(git -C "$kernel_dir" rev-parse HEAD)" = e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
test ! -e "$out"; test ! -e "$archive"; test ! -e "$module_dir"
mkdir "$out" "$archive" "$module_dir"
exec > "$out/build.log" 2>&1
git -C "$kernel_dir" diff --binary > "$archive/source.patch"
cmp "$v50/source.patch" "$archive/source.patch"
cp "$v50/config" "$out/.config"; cp "$v50/config" "$archive/config-input"
export PATH="$clang_dir:$PATH"
export KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build
export KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
cd "$kernel_dir"
wanted="NETFILTER NETFILTER_ADVANCED NETFILTER_XTABLES NETFILTER_XTABLES_LEGACY NF_CONNTRACK NF_NAT
 IP_NF_IPTABLES IP_NF_IPTABLES_LEGACY IP_NF_FILTER IP_NF_MANGLE IP_NF_NAT IP_NF_RAW IP_NF_SECURITY IP_NF_TARGET_REJECT IP_NF_TARGET_MASQUERADE
 IP6_NF_IPTABLES IP6_NF_IPTABLES_LEGACY IP6_NF_FILTER IP6_NF_MANGLE IP6_NF_RAW IP6_NF_TARGET_REJECT IP6_NF_MATCH_RPFILTER IP_NF_MATCH_RPFILTER
 NETFILTER_XT_MARK NETFILTER_XT_CONNMARK NETFILTER_XT_MATCH_BPF NETFILTER_XT_MATCH_OWNER NETFILTER_XT_MATCH_CONNTRACK
 NETFILTER_XT_MATCH_STATE NETFILTER_XT_MATCH_LIMIT NETFILTER_XT_MATCH_MAC NETFILTER_XT_MATCH_MULTIPORT NETFILTER_XT_MATCH_QUOTA
 NETFILTER_XT_MATCH_SOCKET NETFILTER_XT_MATCH_U32 NETFILTER_XT_MATCH_STRING NETFILTER_XT_MATCH_COMMENT NETFILTER_XT_MATCH_PKTTYPE
 NETFILTER_XT_MATCH_IPRANGE NETFILTER_XT_MATCH_LENGTH NETFILTER_XT_MATCH_HELPER NETFILTER_XT_MATCH_POLICY
 NETFILTER_XT_TARGET_IDLETIMER NETFILTER_XT_TARGET_TCPMSS NETFILTER_XT_TARGET_NFLOG NETFILTER_XT_TARGET_CT NETFILTER_XT_TARGET_CLASSIFY
 NETFILTER_XT_TARGET_MASQUERADE NETFILTER_XT_TARGET_TPROXY NETFILTER_XT_TARGET_SECMARK NETFILTER_XT_TARGET_CONNSECMARK NETFILTER_XT_NAT
 NETFILTER_NETLINK_LOG NETFILTER_NETLINK_QUEUE
 NET_SCHED NET_SCH_INGRESS NET_SCH_HTB NET_CLS_BPF NET_CLS_U32 NET_CLS_ACT NET_ACT_BPF NET_ACT_POLICE NET_ACT_GACT NET_ACT_MIRRED
 INET_DIAG INET_UDP_DIAG INET_DIAG_DESTROY XFRM_USER NET_KEY INET_ESP INET6_ESP TUN VETH
 IPV6_ROUTER_PREF IPV6_ROUTE_INFO IPV6_OPTIMISTIC_DAD IPV6_MIP6 IPV6_VTI NET_IPVTI IPV6_MULTIPLE_TABLES IP_MULTIPLE_TABLES IP_ADVANCED_ROUTER
 BPF_SYSCALL BPF_JIT CGROUP_BPF
 QCOM_RMTFS_MEM USERFAULTFD CPUSETS_V1 DM_VERITY EROFS_FS"
args=(); for s in $wanted; do args+=(-e "$s"); done
scripts/config --file "$out/.config" "${args[@]}"
make O="$out" ARCH=arm64 LLVM=1 olddefconfig
cp "$out/.config" "$archive/config"
scripts/diffconfig "$archive/config-input" "$archive/config" > "$archive/config-differences.txt" || true
: > "$archive/config-not-builtin.txt"
for s in $wanted; do grep -qx "CONFIG_$s=y" "$out/.config" || echo "$s" >> "$archive/config-not-builtin.txt"; done
for s in USERFAULTFD NETFILTER_XTABLES IP_NF_FILTER IP6_NF_FILTER IP_NF_MANGLE NETFILTER_XT_MATCH_BPF NETFILTER_XT_MATCH_OWNER NET_CLS_BPF NET_SCH_INGRESS; do
    grep -qx "CONFIG_$s=y" "$out/.config"
done
make O="$out" ARCH=arm64 LLVM=1 -j14 Image.gz
make O="$out" ARCH=arm64 LLVM=1 -j14 modules dtbs > "$out/modules-build.log" 2>&1
# modules_install wants zstd (absent in WSL); collect the built modules directly, stripped of debug info.
rm -rf "$out/modinst"; mkdir -p "$out/modinst"
( cd "$out" && find . -name "*.ko" -not -path "./modinst/*" | while read -r m; do mkdir -p "modinst/$(dirname "$m")"; llvm-strip --strip-debug -o "modinst/$m" "$m"; done )
cp "$out/modules.order" "$out/modules.builtin" "$out/modinst/" 2>/dev/null || true
( cd "$out/modinst" && find . -name "*.ko" | sort | xargs sha256sum ) > "$archive/modules-SHA256SUMS"
tar -C "$out/modinst" -czf "$archive/modules.tar.gz" .
cp "$out/arch/arm64/boot/dts/qcom/sdm660-hisense-a6l"*.dtb "$archive/" 2>/dev/null || true
cp "$workspace/firmware/extracted/framework-kernel-v50-20260918/a6l_simplefb.c" "$module_dir/"; printf 'obj-m += a6l_simplefb.o\n' > "$module_dir/Makefile"
make O="$out" ARCH=arm64 LLVM=1 -j14 M="$module_dir" modules > "$module_dir/build.log" 2>&1
cp "$out/arch/arm64/boot/Image" "$out/arch/arm64/boot/Image.gz" "$module_dir/a6l_simplefb.c" "$module_dir/a6l_simplefb.ko" "$archive/"
git diff --binary > "$out/source-after.patch"; cmp "$archive/source.patch" "$out/source-after.patch"
( cd "$archive" && sha256sum Image Image.gz a6l_simplefb.ko > SHA256SUMS )
cp "$0" "$archive/"
printf '\nA6L_PHONE_KERNEL_V67_CANDIDATE_BUILD_PASS\n'
cp "$out/build.log" "$archive/build.log"
