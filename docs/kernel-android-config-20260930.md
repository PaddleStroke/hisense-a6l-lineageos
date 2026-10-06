# Kernel r5: Android config pass (30 Sep 2026)

Fixes the ledger's bug-hunt finding **B3** ("r5 bug hunt boot-init"): the V67 kernel config was missing options that
Android needs. This was done offline: no phone, no adb, no flash and no `m`. **The r5 kernel has not been booted.** The ROM
default is still the proven V67 kernel. r5 is opt-in with `A6L_KERNEL=r5`.

## Artifacts

| What | Where | sha256 |
|---|---|---|
| Fragment | `device/hisense/a6l/kernel/configs/a6l-android.config` | |
| Image (7.2.3-a6l-probe+) | `firmware/extracted/kernel-r5-20260930/Image` | `de4970b302c1dbb2efde2718f6abb79ff0811ca726d1b41e92a3669e2c465fd6` (thermal rebuild, 29 Sep; was `5f92e057…`, see below) |
| Image.gz | same dir | `857232fd0282c09706b3687d58bf26d9780c364286487329636861e5d51f5f4d` (was `a13f5d39…`) |
| .config | same dir `config` (+ `config-diff-v67-r5.txt`, fragments, `config-input-v67`) | `7871e1bd…` (thermal; was `12fd5d0f…`) |
| 122 ROM modules | same dir `modules/` + `modules-provenance.txt` | `SHA256SUMS` |
| first-stage sdhci-msm.ko | `ramdisk-modules/` | `5c6d904d…` |
| QEMU virtio_mmio/virtio_blk | `qemu-modules/` | |
| series q6voice stack (volte3+F6+F37) | `extra/series-q6voice/` (not staged, see below) | |
| Module.symvers, System.map, build scripts/logs | same dir, `scripts/` | |

The DT is unchanged: `tools/build-rom-v2-dt.sh`, rom-v2.dtb `5d186e79…`.

## Source and build

- **Source tree:** `/home/a6l/kernel/a6l-rom-r5-src`. It is baseline commit `e47d622cb` plus the archived V67
  `source.patch` (`66ee3648`), plus the untracked dts/`a6l_earlycon.c` (17 Sep), plus `kernel/rom-v2/series` (all 12
  patches, camfix5 line; the camfix7 candidate stays commented).
  - The baseline working tree now also carries the 22 Sep msm DSI `a6l_dsi1_slave` WIP, which is **not** in V67. Those
    3 files were restored to their V67 state. Checked with `diff -r` against a fresh HEAD + source.patch tree: the only
    differences are the series files.
- **Output dir:** `/home/a6l/kernel/out-a6l-rom-r5`. `out-a6l-phone-v67` was not touched (Image still `0d7d2eb6`).
- **Config:** `merge_config.sh -m` of v67 `.config` + `rom-v2/config.fragment` + `a6l-android.config`, then `olddefconfig`.
  Every requested value landed, with one exception: `V4L2_CCI_I2C` is select-only, so v4l2-cci stays out-of-tree as before.
- **Build:** `LOCALVERSION=+` (the copy has no .git; this keeps `7.2.3-a6l-probe+`), clang r584948. 0 warnings on Image or modules.

## Options added (reference: Lineage 24 = cp2a / Android 17)

`kernel/configs/c/android-6.18/android-base.config` is **empty** in this tree, so the check used
`b/android-6.12/android-base.config` + `android-base-conditional.xml` (arm64 group).

Options added as `=y` unless stated:

- **Module ABI:** MODVERSIONS
- **Storage and encryption:**
  - FBE: FS_ENCRYPTION, FS_ENCRYPTION_INLINE_CRYPT, BLK_INLINE_ENCRYPTION and _FALLBACK (no eMMC ICE; MMC_CRYPTO stays off)
  - FS_VERITY, CRYPTO_ADIANTUM, CRYPTO_HCTR2, DM_SNAPSHOT, QFMT_V2
- **Crypto:** CHACHA20POLY1305, CMAC (both were m, now y), XCBC
- **netd and connectivity:**
  - xt matches: CONNLIMIT, CONNMARK, HASHLIMIT, MARK (was m), STATISTIC (was m), TIME
  - xt targets: CONNMARK, CONNSECMARK, MARK, NFQUEUE, TRACE
  - arptables: ARPTABLES, ARPFILTER, ARP_MANGLE; IP_NF_MATCH_ECN/TTL, TARGET_NETMAP, TARGET_REDIRECT (was m)
  - conntrack: SECMARK, NF_CT_NETLINK, helpers AMANDA/FTP/H323/IRC/NETBIOS_NS/PPTP/SANE/TFTP
  - tc: NET_CLS_MATCHALL, NET_EMATCH(+U32), NET_SCH_TBF
  - devices and tunnels: NET_IPGRE_DEMUX, IFB, DUMMY (was m), INET6_IPCOMP, XFRM_INTERFACE/MIGRATE/STATISTICS
- **Power:** PM_WAKELOCKS, NO_HZ
- **Input:** HID_SONY, JOYSTICK_XPAD (both were m, now y), PLAYSTATION_FF. HID_PLAYSTATION stays **m** because it
  depends on LEDS_CLASS_MULTICOLOR=m.
- **Hardening and misc:** BPF_JIT_ALWAYS_ON, BUG_ON_DATA_CORRUPTION, HARDENED_USERCOPY, STATIC_USERMODEHELPER (path ""),
  KFENCE, ARMV8_DEPRECATED + SWP/CP15/SETEND emulation, EXPERT, STAGING, DEFAULT_SECURITY_SELINUX
- **Removed (android-base "must not be set"):** FHANDLE, SYSVIPC, FW_CACHE

## Not available in mainline 7.2, and their replacements

- **NETFILTER_XT_MATCH_QUOTA2(_LOG):** this option exists only in the Android Common Kernel (ACK). Lineage 24 netd
  still emits `-m quota2` (BandwidthController.cpp:59/341/448) for data alerts and global/per-interface quotas. These
  rules will fail, so the data warning/limit alerts will not work. Per-UID accounting and data saver use BPF, which is
  present.
  - **TODO:** port ACK `xt_quota2.c` as an out-of-tree xt module.
- **DM_DEFAULT_KEY:** ACK only. FBE works without it, but there is no metadata encryption.
  - **TODO:** port it before using `metadata_encryption=`.
- **ASHMEM:** removed upstream. libcutils falls back to memfd.
- **CPU_FREQ_TIMES / UID_SYS_STATS:** ACK only. Per-UID CPU and IO stats are degraded (BPF time_in_state needs bpfloader).
- **NF_CT_PROTO_DCCP/UDPLITE, SCHED_DEBUG, ARM64_PAN:** removed or renamed upstream. `CFI_CLANG` is now `CFI=y`.
  ARM64_SW_TTBR0_PAN is left off (ARMv8.0 cores) and needs review for release.

## Deliberate deviations

- **DEVMEM stays y** because `a6l_mmio` (attended camera/display register dumps) uses /dev/mem. Unset it for a `user` release.
- **FUSE_FS, UHID, SND, SOUND stay m.** The ROM loads them itself (base.txt / audio.txt), so the proven module lists are unchanged.

## Removed or downgraded vs V67 (boot safety)

`scripts/diffconfig` shows 238 lines. The only removals and downgrades are:

- FHANDLE, SYSVIPC (+SYSVIPC_COMPAT/SYSCTL), FW_CACHE: the Android requirement.
- DEFAULT_SECURITY_DAC → SELINUX: `CONFIG_LSM` is unchanged, so SELinux was already active.
- MEDIA_HIDE_ANCILLARY_SUBDRV: hidden by EXPERT. All newly visible media drivers are n.

No module the ROM stages disappeared. All 122 names are still built as modules, and the m→y options were not staged.

Things to watch at the first boot:

- HARDENED_USERCOPY / BUG_ON_DATA_CORRUPTION turn latent driver bugs into BUGs.
- STATIC_USERMODEHELPER="" disables all kernel usermode helpers. The ROM uses none, and there is no modprobe on Android.

## Module compatibility

With V67 there was no real ABI check: MODVERSIONS was off and every CRC in Module.symvers was 0. Meanwhile
FS_ENCRYPTION/FS_VERITY/BLK_INLINE_ENCRYPTION change `struct inode`/`bio`, so **every staged module must be replaced**,
and all 122 were rebuilt.

With MODVERSIONS=y, the vermagic is `7.2.3-a6l-probe+ SMP preempt mod_unload modversions aarch64`. The two kernels'
modules are mutually refused, so they can never be mixed silently.

**Provenance of the rebuilt modules.** Each staged .ko was matched (by the sha of its stripped build) to the directory
that built it and rebuilt from that same source (`scripts/build-r5-oot.sh`, `modules-provenance.txt`):

- **81 unmodified in-tree modules:** rebuilt from the r5 tree.
- **Series-patched modules, from the r5 tree:** camss camfix5, q6adm, q6routing per-direction, q6asm-dai, lpi, btqca,
  qcom_smbx, msm8916-analog MBHC v2, rmnet, v4l2/vb2, leds. Their sources were checked to be identical to the dirs
  that built the staged files.
- **Out-of-tree modules, rebuilt from their original dirs:**

  | Module | Source dir |
  |---|---|
  | camera (v4l2-cci, imx576_a6l, s5k3t1, gt9769, hi846, i2c-qcom-cci) | camfix2/mod |
  | ipa2_lite | data3 |
  | panel-a6l-epd-dsi | epd-v73 |
  | panel-ft8719 | display-v67 |
  | tps65185 | extra-v67 |
  | snd-soc-tfa98xx | audio3 |
  | tmd3702 | audio2-v74 |
  | a6l_simplefb | simplefb-v67 |
  | stk3338_a6l | stk-f3 (= repo) |
  | a6l_gpio_vib | vib49 (= repo) |
  | q6voice stack | kvoice-build (= the proven audio4 44ca61c6 source) |
  | q6routing-upstream | unpatched baseline |

**Results:**

- `scripts/collect-verify-r5.py`: **R5_MODULES_VERIFY PASS**. All 122/122 have the r5 vermagic, and all 7297
  `__versions` imports match the export CRC of vmlinux or of the staged r5 provider module. There are no unresolved or
  unversioned imports.
- The extras (sdhci-msm, virtio, series q6voice) also pass.
- modinfo (depends/parm/alias/name/license) is identical old vs new for all 122. The only symbol differences are local
  inlining (list helpers under LIST_HARDENED).
- The kvoice note in the ledger is confirmed: the **staged q6voice stack is audio4, not the series**. The series also
  carries volte3 `mmode1_session`, F6 `TX Mute` and F37 `RX Volume Step`. Those builds are in `extra/series-q6voice/`
  and are not staged, so r5 keeps the proven behaviour. Shipping them is the separate ledger TODO.

## How the ROM picks it up

Run `A6L_KERNEL=r5` for **both** the stage step and the boot step (`tools/rom-v2-pipeline.sh` passes it on):

- **`tools/stage-rom-v2-prebuilts.sh`:** replaces every staged .ko with `kernel-r5-20260930/modules/<name>`. It
  checks SHA256SUMS and that the set has the same size, and requires the exact r5 vermagic. Without the variable it
  requires the exact V67 vermagic.
  - Dry run with the default: `STAGE_ROM_V2_PREBUILTS_PASS`, manifest byte-identical to before the change.
  - Dry run with r5 (`/tmp/stage-r5`): `STAGE_ROM_V2_PREBUILTS_PASS`, MODULE_ORDER PASS, same 489 files, only .ko differ.
- **`tools/Prepare-RomV2Boot.py`:** uses the r5 Image (sha asserted) and the r5 first-stage `sdhci-msm.ko`.
  - Offline packaging passes for V67, r5 and r5 `--qemu`. The V67 ramdisk is unchanged (`5d6b077f` = r3).
  - r5 boot.img: `ae05419b…` (body 14.4 MB < 64 MB). After the thermal rebuild: `458201f9…` (see below).
- **`tools/rom-v2-pipeline.sh`:** under r5 it uses the r5 QEMU modules.
- `tools/check-rom-v2-kernel-series.sh` (`A6L_KSERIES_DIR=/tmp/kseries-kcfg`): **A6L_KSERIES_PASS**.

## Thermal rebuild (29 Sep 2026): critical trips power off again

Finding (docs/android-thermal-cooling-20260929.md), verified in the r5 source: a critical trip calls
`thermal_zone_device_halt` -> `__hw_protection_trigger(msg, CONFIG_THERMAL_EMERGENCY_POWEROFF_DELAY_MS, …)`
(`drivers/thermal/thermal_core.c:321`). That queues the backup `hw_failure_emergency_action_work` only when the delay is
`> 0` (`kernel/reboot.c` `hw_failure_emergency_schedule`), then runs `orderly_poweroff(true)`. With
`STATIC_USERMODEHELPER_PATH=""`, `call_usermodehelper_exec` returns 0 without running anything (`kernel/umh.c:422`), so
`__orderly_poweroff` never takes its forced `kernel_power_off()` branch. Old r5 (`DELAY_MS=0`): a critical trip only logged
"HARDWARE PROTECTION shutdown".

- **Fix:** `config` = old r5 config + `device/hisense/a6l/kernel/configs/a6l-thermal.config` (`merge_config.sh -m` +
  `olddefconfig`). diffconfig: only `THERMAL_EMERGENCY_POWEROFF_DELAY_MS 0 -> 100` (GKI value). The critical trip now
  force-powers off 100 ms after the (no-op) orderly attempt.
- **Build:** same tree `/home/a6l/kernel/a6l-rom-r5-src`, new out dir `/home/a6l/kernel/out-a6l-rom-r5-thermal` (the r5
  out dir was not touched), `scripts/build-r5-thermal.sh`, clang r584948, `W=1`. No warning in thermal_core/reboot; the
  1098 W=1 warnings are the tree's existing `-Winitializer-overrides` (1096) and 2 `-Wunused-but-set-variable`.
- **Image `de4970b3…`**, Image.gz `857232fd…`, config `7871e1bd…`, System.map `d35094c4…` (same symbols and
  addresses; only `kernel_config_data_end` moves by 1 byte). Same `Linux version` string. Old files kept in
  `pre-thermal-20260929/`. SHA256SUMS updated (`sha256sum -c` OK), `frag-a6l-thermal.config` added.
- **Modules unchanged, nothing restaged.** `Module.symvers` is byte-identical (`ca2fe4dc…`), so every export CRC and
  the vermagic are the same. All 104 staged modules built from the r5 tree, plus `ramdisk-modules/sdhci-msm.ko` and
  `qemu-modules/virtio_blk.ko`, are byte-identical to the new build except `.note.gnu.build-id` (it depends on the out-dir
  path). The 8 staged modules that come from other dirs (hi846/i2c-qcom-cci camfix2, q6voice stack audio4) match the r5
  tree build too. The 15 out-of-tree modules link only against the unchanged CRCs.
- **Boot image:** `tools/Prepare-RomV2Boot.py` (`R5_IMAGE` = de4970b3), `A6L_KERNEL=r5`, DTB `dt-r5` `5d186e79…`:
  `ROM_V2_BOOT_PASS`, boot.img `458201f96c06238a1908bda29d9d6557850e35724c428f0bad057860b8685069` in
  `/home/a6l/rom-v2/boot-r5t` (ramdisk `ac6689a7…` and dtbo `69251122…` are the same as r5). QEMU variant `d3216d40…`
  (`boot-r5t-qemu`). Captured-ABL emulation (`Test-RomV2Abl.py`): passed. The `kit-r5`/`boot-r5` of the r5 build were
  not changed. The next `A6L_KERNEL=r5` pipeline run picks the new Image up.
- **Release fragment (kernel-gaps):** `build-release-config.sh` merges onto this `config`, so the release kernel inherits
  `DELAY_MS=100`. It now also checks that the value survives. `A6L_RELEASE_CONFIG_PASS` (run with a scratch `A6L_RELEASE_O`).
- **Other references updated:** `tools/stage-rom-v2-prebuilts.sh` (comment),
  `kernel/android-gaps/tests/test-android-gaps.sh` (expected r5 Image `de4970b3`).
- **Checks:** A6L_KSERIES_PASS, stage dry runs v67 + r5 `STAGE_ROM_V2_PREBUILTS_PASS`, A6L_ROM_STATIC_TEST PASS,
  A6L_R6_STATIC_TEST PASS.
- **QEMU:** `A6L_KERNEL=r5 Test-RomV1Qemu.py` (`boot-r5t-qemu`, the current Lineage out system/vendor images, 14:4x): **ROM_V1_QEMU PASS**. boot_completed at 1096 s, no panic, stable 49 s after boot, zygote started once, GNSS HAL declared. Module groups base/display/adsp/audio/misc have fails=0. There are 0 "disagrees about version", "Unknown symbol" or usercopy lines. The postmortem userdata loop mount failed (exit 32), which is tooling only and does not affect the verdict. Run dir: `/home/a6l/rom-v2/qemu-r5t`.
- **Attended (not done):** the r5 kernel is still unbooted on the phone. THERMAL_EMULATION=y, so the attended check is:
  `echo 111000 > /sys/class/thermal/thermal_zone<cpu>/emul_temp` (above the 110 °C critical trip). Expected result:
  "HARDWARE PROTECTION shutdown", then the phone powers off within about 100 ms. With the old r5 Image, the phone stayed on.

## Next steps

1. **Attended RAM boot of the r5 boot.img:** dmesg with no "disagrees about version" / "Unknown symbol" /
   usercopy BUG; all module groups load; display, touch, audio call, camera, charger, radio and Wi-Fi.
2. `iptables -m mark/connmark` work, and netd logs show only the expected quota2 failures.
3. After that, the FBE trial (`A6L_FSTAB=fbe`).
