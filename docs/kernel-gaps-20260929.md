# Kernel gaps: xt_quota2, dm-default-key, per-UID stats, release config (29 Sep 2026)

This closes completeness-audit items **19** (xt_quota2), **18** (dm-default-key: the kernel part), **23** (release config
fragment), and the per-UID stats gap from `docs/kernel-android-config-20260930.md`.

All of this work was offline: no phone, no adb, no flash and no `m`. Everything was built and tested in WSL. The functional tests
boot the **real V67 and r5 Images** in QEMU. **Nothing here has run on the phone.**

## What is ready

### 1. xt_quota2: netd data limits and alerts (staged)

- **Source:** `device/hisense/a6l/kernel/android-gaps/xt_quota2/`. This is the ACK `net/netfilter/xt_quota2.c` from
  aosp-mirror/kernel_common android16-6.12 (identical to android-mainline), with QUOTA2_LOG always on as in GKI.
- **Kept identical to ACK:** the netd contract.
  - The rule `-m quota2 ! --quota N --name X`, match revision 3.
  - Read/write through `/proc/net/xt_quota/<name>`.
  - The alert goes to NETLINK_NFLOG group 1 as nlmsg_type 112, using the 192-byte `ulog_packet_msg_t` that libsysutils
    `NetlinkEvent::parseUlogPacketMessage` expects. The size is pinned by a `static_assert`.
- **Fixes vs ACK:**
  - The counter list is managed under a mutex, and the proc entry is created and removed under that same lock. This closes the
    ACK use-after-free race on a failed `proc_create`.
  - A proc write parses the value with `kstrtoull`, so garbage returns EINVAL instead of storing 0.
  - The netlink socket is released on exit and on init failure.
  - The module no longer writes `skb->tstamp` inside the match.
- **Built for both kernels:** `firmware/extracted/kernel-gaps-20260929/{v67,r5}/xt_quota2.ko`.
- **Loaded by `rom/modules/base.txt`** in early-init, before netd and system_server. A module is required: Android has no
  modprobe, and r5 has `STATIC_USERMODEHELPER`.
- **SELinux:** no change needed. `/proc/net/xt_quota` is `proc_net`, which netd may already read and write. netd already has
  `netlink_nflog_socket`.

### 2. uid_sys_stats: per-UID CPU and IO (staged)

- **Source:** `android-gaps/uid_sys_stats/`, from the ACK `drivers/misc/uid_sys_stats.c`. It keeps the same procfs ABI:
  - `/proc/uid_cputime/show_uid_stat` and `remove_uid_range`: BatteryStats `KernelCpuUidUserSysTimeReader` and statsd
  - `/proc/uid_io/stats`: storaged and `StoragedUidIoStatsReader`
  - `/proc/uid_procstat/set`: system_server
- **Labels:** these paths already have labels in `system/sepolicy` genfs_contexts, so no sepolicy change is needed.
- **Mainline changes:**
  - **Dead-task accounting.** The ACK uses `profile_event_register(PROFILE_TASK_EXIT)`, which mainline removed. The port attaches
    a probe to the `sched_process_exit` tracepoint instead (`for_each_kernel_tracepoint` + `tracepoint_probe_register`, both
    exported). The probe prototype must match `TP_PROTO` for kCFI, and the static test checks this against both trees.
  - **Probe context.** The probe runs with preemption disabled, so it uses GFP_ATOMIC and the ACK trylock + llist deferral.
  - **fsync column.** Always 0, because `ioac.syscfs` is ACK-only.
  - **UIDs** are resolved in init_user_ns.
  - **remove_uid_range** is bounded (rejects a reversed range and caps the span at 1<<20 UIDs) and calls `cond_resched()`.
- **Loaded by:** `base.txt`, built for both kernels.

### 3. dm-default-key: metadata encryption kernel side (built, NOT staged)

- **Source:** `android-gaps/dm-default-key/`, from the ACK `drivers/md/dm-default-key.c`. It provides target `default-key` v2.1.0,
  which is what vold `MetadataCrypt` / libdm `DmTargetDefaultKey` create. Upstream only has `inlinecrypt`, which vold does not use.
- **Kernel:** r5 only. V67 has no BLK_INLINE_ENCRYPTION.
- **Changes needed on 7.2:**
  - **Skip shim.** Mainline has no `bi_skip_dm_default_key`, so the port skips a bio that already carries an inline-crypt
    context. On the A6L eMMC (no ICE) that never happens: the file system applies blk-crypto-fallback before the dm device, so
    FBE file contents are **encrypted twice** (FBE key, then the metadata key). This is always consistent, and the only cost is
    CPU (ARMv8 CE AES-XTS).
  - **Explicit fallback submission.** In 7.2, blk-crypto-fallback is no longer implicit in `submit_bio()`. `->map` must submit
    through `__blk_crypto_submit_bio()`, as upstream dm-inlinecrypt does. **The QEMU test caught this:** the straight ACK port
    wrote nothing and failed reads with EOPNOTSUPP.
  - The `prepare_ioctl` signature was updated, `STANDARD` became `RAW`, and `<linux/hex.h>` was added.
- **Why it is not staged:** metadata encryption needs the FBE fstab trial (`A6L_FSTAB=fbe`, attended), a `/metadata`
  partition with a keydirectory, `metadata_encryption=aes-256-xts` in fstab, and a **userdata wipe**.
  - To enable it, add `dm-default-key.ko` to an r5-only list that loads before `vold` / `fs_mgr` mounts userdata (first stage or
    early-init). vold v2 passes `allow_discards sector_size:4096 iv_large_sectors`, which is tested.

### 4. Per-UID CPU frequency times (CPU_FREQ_TIMES): not ported, by design

- `cpufreq_times` needs hooks inside scheduler cputime accounting, so it cannot be built out of tree. Android has also replaced
  `/proc/uid_time_in_state` with the BPF `time_in_state` program (bpfloader + libtimeinstate). That program needs cpufreq policies.
- The A6L has **no cpufreq driver yet** (the cpufreq/CPR session). Until then:
  - per-UID frequency times are unavailable;
  - per-UID user/sys CPU time comes from uid_sys_stats (above);
  - network per-UID accounting is BPF (netd), as before.
- Once cpufreq exists: check `dumpsys batterystats --checkin` for time-in-state, and that bpfloader loaded `timeInState.o`.

### 5. Release kernel config fragment

- **Fragment:** `device/hisense/a6l/kernel/configs/a6l-release.config`, applied on top of the r5 config **for a release/user
  kernel only**. Debug and RAM-boot kernels keep the r5 config.
- **Settings:**
  - Off: DEVMEM, DEVPORT, KEXEC, CRASH_DUMP, HIBERNATION.
  - On: SLAB_FREELIST_RANDOM, SLAB_FREELIST_HARDENED, SHUFFLE_PAGE_ALLOCATOR, RANDOMIZE_KSTACK_OFFSET_DEFAULT, FORTIFY_SOURCE.
- **Why DEVMEM can go:** `a6l_mmio` (the only /dev/mem user) is a diagnostic tool pushed to recovery/RAM boots
  (`tools/build-display-diag.sh`). It is not in the ROM.
- **Merge and check:** `configs/build-release-config.sh` merges, runs olddefconfig and checks every value.
  **A6L_RELEASE_CONFIG_PASS**. The diff is in `firmware/extracted/kernel-gaps-20260929/release-config/`.
- **Build (`--build`):** Image `5ef8030d…`, 0 new warnings (the 2 are camss `a6l_*` -Wmissing-prototypes from the camera series).
- **It is NOT module-ABI neutral.** 47 export CRCs change (the kmalloc/kmem_cache family, dma_iova_*, cpu_device_create, …)
  and 13 kexec/hibernation/vmcore exports disappear. None of the removed exports is imported by a staged module. However, 64 of
  the 131 r5 modules import a changed CRC.
  - **Consequence:** a release kernel needs the full module rebuild against its own tree, like r5 did (`build-r5-oot.sh`
    pattern + in-tree modules), plus a new stage variant (`A6L_KERNEL=release`). That is release packaging work, not done yet.
- **Optional, separate: `a6l-release-abi.config`** (`ARM64_SW_TTBR0_PAN=y`: PAN emulation on the ARMv8.0 cores, as in GKI).
  Pierre decides (see below).
- **Deliberately left unchanged:**
  - MODULE_SIG_FORCE: the staged OOT modules are unsigned (taint E). Forcing it needs `sign-file` in every module build.
  - PANIC_ON_OOPS: there are known driver oopses, e.g. `adreno_remove`.
  - SECURITY_SELINUX_DEVELOP: as in GKI; enforcing is decided by init/cmdline.
  - DEBUG_FS: as in GKI; not mounted in user builds.
  - MAGIC_SYSRQ and KPROBES: as in GKI.

## Tests

- **Build:** `android-gaps/build-android-gaps.sh`, 5 modules, W=1, 0 warnings. SHA256SUMS:

  | Module | sha256 (prefix) |
  |---|---|
  | v67 xt_quota2 | `7683ce8e` |
  | v67 uid_sys_stats | `22497248` |
  | r5 xt_quota2 | `b0be045d` |
  | r5 uid_sys_stats | `098e3e81` |
  | r5 dm-default-key | `e9bd1c49` |

- **Static:** `android-gaps/tests/test-android-gaps.sh` → **A6L_GAPS_TEST_PASS** (21 checks, 23 with `--qemu`). It checks:
  - vermagic;
  - every r5 `__versions` CRC against the r5 Module.symvers (41/42/24 imports, none missing or unversioned);
  - that every V67 import is exported by the V67 vmlinux;
  - the kCFI probe prototype against both trees;
  - `xt_quota2.h` against external/iptables, libxt_quota2 revision 3, and the libsysutils 192/112 values;
  - the base.txt entries and the stage-script hook, and that dm-default-key is in no list;
  - the genfs labels.
- **QEMU functional** (`tests/qemu/run-qemu-gaps.sh v67|r5`: a static test `/init`, the real Images, the ROM's virt.dtb):
  **A6L_GAPS_QEMU PASS on both** (logs: `firmware/extracted/kernel-gaps-20260929/qemu-{v67,r5}.txt`). Checked:
  - **xt_quota2:**
    - the rule is installed with raw `IPT_SO_SET_REPLACE`;
    - 7 × 128-byte packets pass, the counter reads 104, and the 8th gets EPERM (DROP);
    - the NFLOG alert arrives once (type 112, prefix `a6ltest`, out `lo`);
    - a netd-style proc write re-opens traffic, and a garbage write gets EINVAL;
    - removing the rule frees the counter, and rmmod works.
  - **uid_sys_stats:**
    - a dead task's cputime (265 ms) and io (256 KiB wchar) are accounted;
    - fg/bg bucket switching works;
    - 400 exits across 8 UIDs with 2 concurrent readers work, with no CFI failure or BUG;
    - `remove_uid_range` works, bad input is rejected, and rmmod plus later task exits work.
  - **dm-default-key (r5):**
    - a vold v2 table on a loop device works, and the status line matches with the key omitted;
    - 8 MiB written through it is encrypted on the backing file;
    - **a dm-crypt `aes-xts-plain64 … sector_size:4096 iv_large_sectors` table with the same key reads back the plaintext**, so
      the on-disk format is the ACK/dm-crypt one;
    - O_DIRECT and buffered read-back work;
    - a bad option set is rejected, and teardown and rmmod work.
- **Stage dry runs:**
  - default: `STAGE_ROM_V2_PREBUILTS_PASS`, MODULE_ORDER PASS, 126 modules (+2);
  - `A6L_KERNEL=r5`: PASS, MODULE_ORDER PASS, 126 modules, r5 vermagic on both new modules.
- **`device/hisense/a6l/rom/tests/test-rom-static.sh`:** PASS. No sepolicy files were touched.

## Needs an attended test (phone)

1. **Boot.** dmesg shows `xt_quota2: A6L port loaded (nflog on, event 112)` and `uid_sys_stats: A6L port loaded`, with no
   `Unknown symbol` or CFI failure. The logcat A6L_ROM line for base shows `fails=0`.
2. **Data limit.** In Settings > Network > Data warning & limit, set a warning of 1 MB, then download. Check that the warning
   notification appears. `adb shell cat /proc/net/xt_quota/*` should show the counters, and `iptables -S bw_costly_shared`
   should show quota2 rules. netd logs should show no `quota2` errors.
3. **Per-UID stats.**
   - `adb shell cat /proc/uid_cputime/show_uid_stat | head` and `/proc/uid_io/stats` should be non-empty.
   - `dumpsys batterystats` should show per-app CPU user/sys.
   - `dumpsys storaged --uid` should show per-app io.
   - There should be no avc denials.
4. **Later, with the FBE trial (r5, wipe).** Stage `dm-default-key.ko` in an early list, set `metadata_encryption` in fstab,
   and check that vold creates `default-key` and `/data` mounts. Measure the cost of double encryption (e.g. with fio).

## Needs Pierre

- **Release kernel timing.** When to cut a release kernel with `a6l-release.config`. It needs its own full module rebuild and
  stage variant.
- **ARM64_SW_TTBR0_PAN** (`a6l-release-abi.config`): security vs. per-uaccess cost. It also changes the module ABI.
- **MODULE_SIG_FORCE.** Signed modules only, for release: add `sign-file` to every module build.
- **Metadata encryption.** Whether it is worth a userdata wipe and the double-encryption CPU cost on the eMMC. FBE alone already
  protects user files.
