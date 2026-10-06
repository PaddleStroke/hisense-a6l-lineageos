# A6L kernel gaps: out-of-tree ports of Android Common Kernel features

The A6L kernels (V67 and r5) are mainline 7.2. They lack three features that exist only in the ACK. This directory ports them as
out-of-tree modules. Full notes: `docs/kernel-gaps-20260929.md`.

| Module | ACK option | Built for | Staged | Android user |
|---|---|---|---|---|
| `xt_quota2/` | NETFILTER_XT_MATCH_QUOTA2 + _LOG | v67, r5 | yes, `rom/modules/base.txt` | netd BandwidthController (data warning/limit, alerts) |
| `uid_sys_stats/` | UID_SYS_STATS | v67, r5 | yes, `base.txt` | BatteryStats (`/proc/uid_cputime`), storaged (`/proc/uid_io`), AMS (`/proc/uid_procstat`) |
| `dm-default-key/` | DM_DEFAULT_KEY | r5 only | **no** (FBE + metadata trial) | vold MetadataCrypt |

- **Build:** `build-android-gaps.sh`. It uses W=1 and clang r584948 against `out-a6l-phone-v67` and `out-a6l-rom-r5`, and writes
  to `firmware/extracted/kernel-gaps-20260929/{v67,r5}` + SHA256SUMS.
- **Static tests:** `tests/test-android-gaps.sh`. With `--qemu` it also runs `tests/qemu/run-qemu-gaps.sh v67|r5`, which boots
  the real Images in QEMU and runs the functional tests.
