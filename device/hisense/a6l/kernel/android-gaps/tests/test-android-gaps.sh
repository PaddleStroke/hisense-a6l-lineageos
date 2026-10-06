#!/usr/bin/env bash
# Static checks of the kernel-gaps modules (WSL, offline). docs/kernel-gaps-20260929.md
#  - bundle SHA256SUMS, vermagic per kernel (V67 / r5 modversions)
#  - r5: every __versions CRC == the export CRC in the r5 Module.symvers (what the phone kernel will check)
#  - V67 (no MODVERSIONS): every undefined symbol is exported by the V67 vmlinux (out-a6l-phone-v67/Module.symvers)
#  - kCFI: the uid_sys_stats probe prototype == TP_PROTO(sched_process_exit) of both kernel trees
#  - xt_quota2.h == external/iptables xt_quota2.h (netd's iptables userspace ABI); match revision 3 in libxt_quota2
#  - base.txt lists xt_quota2.ko and uid_sys_stats.ko; dm-default-key is NOT staged (FBE+metadata trial only)
#  - the QEMU test images are the ROM's kernels (r5 Image de4970b3 = thermal rebuild of 5f92e057, same modules, V67 candidate Image)
# usage: test-android-gaps.sh [--qemu]   (--qemu also runs tests/qemu/run-qemu-gaps.sh for v67 and r5)
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd); G=$(cd "$HERE/.." && pwd); REPO=$(cd "$G/../../../../.." && pwd)
B=$REPO/firmware/extracted/kernel-gaps-20260929; L=/home/a6l/android/a6l-lineage24
export PATH=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH
pass=0; fail=0; exp() { if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL $1: got '$2' want '$3'"; fi; }
( cd "$B" && sha256sum -c --quiet SHA256SUMS ); exp "bundle SHA256SUMS" $? 0
V67M="vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload aarch64"; R5M="vermagic=7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64"
for m in xt_quota2 uid_sys_stats; do exp "v67 $m vermagic" "$(strings $B/v67/$m.ko | grep -m1 ^vermagic=)" "$V67M"; done
for m in xt_quota2 uid_sys_stats dm-default-key; do exp "r5 $m vermagic" "$(strings $B/r5/$m.ko | grep -m1 ^vermagic=)" "$R5M"; done
exp "v67 has no dm-default-key" "$(ls $B/v67 | grep -c dm-default-key)" 0
python3 - "$B" "$REPO/firmware/extracted/kernel-r5-20260930/Module.symvers" /home/a6l/kernel/out-a6l-phone-v67/Module.symvers <<'PY'
import sys, struct, subprocess
B, S5, S67 = sys.argv[1:4]
def symvers(p):
    return {l.split('\t')[1]: int(l.split('\t')[0], 16) for l in open(p) if l.count('\t') >= 3}
def versions(ko):
    # __versions: struct modversion_info { unsigned long crc; char name[64-8]; } (64-bit)
    t = '/tmp/a6l-gaps-versions.bin'
    subprocess.run(['llvm-objcopy', '-O', 'binary', '--only-section=__versions', ko, t], check=True); out = open(t, 'rb').read()
    r = {}
    for i in range(0, len(out), 64):
        crc = struct.unpack_from('<Q', out, i)[0] & 0xffffffff; name = out[i+8:i+64].split(b'\0')[0].decode()
        if name: r[name] = crc
    return r
def undef(ko):
    o = subprocess.run(['llvm-nm', '-u', ko], capture_output=True, text=True).stdout.split()
    return {x for x in o if x not in ('U', 'w')}
bad = 0; s5 = symvers(S5); s67 = symvers(S67)
for m in ('xt_quota2', 'uid_sys_stats', 'dm-default-key'):
    v = versions(f'{B}/r5/{m}.ko'); u = undef(f'{B}/r5/{m}.ko')
    miss = [n for n in v if n not in s5]; wrong = [n for n in v if n in s5 and s5[n] != v[n]]; unv = sorted(u - set(v))
    ok = not miss and not wrong and not unv and len(v) > 5
    print(f"r5 {m}: {len(v)} versioned imports, missing={miss} crc_mismatch={wrong} unversioned={unv}", 'OK' if ok else 'BAD'); bad += not ok
for m in ('xt_quota2', 'uid_sys_stats'):
    u = undef(f'{B}/v67/{m}.ko'); miss = sorted(x for x in u if x not in s67)
    print(f"v67 {m}: {len(u)} imports, not exported by V67 vmlinux: {miss}", 'OK' if not miss else 'BAD'); bad += bool(miss)
sys.exit(1 if bad else 0)
PY
exp "symbol/CRC check" $? 0
for k in /home/a6l/kernel/a6l-baseline-7.2 /home/a6l/kernel/a6l-rom-r5-src; do
  exp "sched_process_exit TP_PROTO in $(basename $k)" "$(awk '/TRACE_EVENT\(sched_process_exit,/{f=1} f&&/TP_PROTO/{print; exit}' $k/include/trace/events/sched.h | tr -d ' \t')" "TP_PROTO(structtask_struct*p,boolgroup_dead),"
done
exp "probe prototype" "$(grep -c '^static void a6l_uid_exit_probe(void \*data, struct task_struct \*task, bool group_dead)$' $G/uid_sys_stats/uid_sys_stats.c)" 1
strip_h() { sed -n '/^enum xt_quota_flags/,$p' "$1"; }
exp "xt_quota2.h == external/iptables" "$(diff <(strip_h $G/xt_quota2/include/linux/netfilter/xt_quota2.h) <(strip_h $L/external/iptables/include/linux/netfilter/xt_quota2.h) >/dev/null && echo same)" same
exp "libxt_quota2 revision 3" "$(grep -c '\.revision *= *3' $L/external/iptables/extensions/libxt_quota2.c)" 1
exp "libsysutils ulog size 192" "$(grep -c 'static_assert(sizeof(ulog_packet_msg_t) == 192)' $L/system/core/libsysutils/src/NetlinkEvent.cpp)" 1
exp "libsysutils QLOG event 112" "$(grep -c 'LOCAL_QLOG_NL_EVENT = 112' $L/system/core/libsysutils/src/NetlinkEvent.cpp)" 1
BT=$REPO/device/hisense/a6l/rom/modules/base.txt
exp "base.txt xt_quota2" "$(grep -v '^#' $BT | grep -c '^xt_quota2.ko$')" 1
exp "base.txt uid_sys_stats" "$(grep -v '^#' $BT | grep -c '^uid_sys_stats.ko$')" 1
exp "dm-default-key not in any list" "$(cat $REPO/device/hisense/a6l/rom/modules/*.txt | grep -v '^#' | grep -c dm-default-key)" 0
exp "stage script stages kernel-gaps" "$(grep -c '^KG=$X/kernel-gaps-20260929;' $REPO/tools/stage-rom-v2-prebuilts.sh)" 1
exp "genfs labels for uid_sys_stats procfs" "$(grep -c -E 'genfscon proc /(uid_cputime/show_uid_stat|uid_cputime/remove_uid_range|uid_io/stats|uid_procstat/set) ' $L/system/sepolicy/private/genfs_contexts)" 4
exp "r5 Image is the ROM r5 Image" "$(sha256sum < $REPO/firmware/extracted/kernel-r5-20260930/Image | cut -c1-8)" de4970b3
if [ "${1:-}" = --qemu ]; then
  for k in v67 r5; do bash "$HERE/qemu/run-qemu-gaps.sh" $k > /tmp/a6l-gaps-qemu-$k.txt 2>&1; exp "QEMU functional $k" $? 0; tail -1 /tmp/a6l-gaps-qemu-$k.txt; done
fi
echo "pass=$pass fail=$fail"; [ $fail = 0 ] && echo A6L_GAPS_TEST_PASS || { echo A6L_GAPS_TEST_FAIL; exit 1; }
