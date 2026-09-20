# Attended session plan — V68 candidate (prepared 20 September 2026, nothing run on the phone)

Everything below was prepared offline. The spare phone is in stock Android with the verified V46
diagnostic recovery; only read-only `adb shell` queries were made today (`captures/stock-readonly-20260920`).
**Pierre must be at the phone for every step from 1 onward** (long-Power escape, screen observation).

## What V68 is

`firmware/extracted/recovery-v68-candidate-20260920/recovery-diagnostic-unsigned.img`
SHA-256 `2448b101eb780b1630cc6fd7181315da50211de2504b08cd6dad9e34d44a7520` = V46 with exactly:

| Change | Why | Risk note |
|---|---|---|
| Kernel → `phone-kernel-v67-candidate` (`0d7d2eb6…5acf7`) | Android networking built in (netd/Wi-Fi later), `QCOM_RMTFS_MEM`, UFFD/cpuset/verity/erofs for the framework | configuration-only delta from the validated V38 kernel; QEMU smoke only |
| DT + `a6l-eink-flash-read` | BLSP2 QUP4 as SPI on gpio28–31, child `eink,ed052tc2@0`, no MTD driver | i2c8 stays disabled; PIO only; nothing probes the flash by itself |
| DT + ADSP candidate A (`research/claude-adsp`) | `adsp_pil` okay + firmware-name | nothing loads at boot: the remoteproc modules are RAM payloads |
| ramdisk `sdhci-msm.ko` rebuilt from the V67 tree | same source/patch, consistent with the new kernel | same vermagic; storage is the first thing to verify after boot |

Checks passed: DT property allowlist (26 properties), boot-header invariants, mkbootimg round trip,
`Test-RecoveryV68.py` (captured ABL: header, overlay merge, fix-ups, gunzip, DTB selection, AVB path),
`Test-RecoveryTransitionV68.py` (4), `Test-DiagnosticRecoveryProtocolV68.py` (6). Install/restore/inspect
tools were generated from the hash-pinned V46 tools by `tools/Prepare-V68Trial.py`
(`tools/diagnostic-user-v68-tools.json`). The transition policy accepts only V46 as predecessor.

## Order of operations

0. **Review** this file and `report.json`; confirm the candidate hash. Decide go/no-go.
1. **Install V68** with the generated `Launch-ControlsV68Install.py` flow (same procedure as V46: staged
   write, full readback, stock Android returns). Keep the V46 image at hand as the known-good fallback;
   `Run-LaptopDiagnosticRestore-user-v68.py` is the restore path.
2. **Boot recovery once, baseline only**: authenticated ADB, eMMC enumerates, the ten firmware read hashes
   match (the V38 check), display/touch unchanged. Stop here if anything regresses.
3. **E-ink flash backup** (read-only): push `v68-attended-bundle-20260920/eink/*` to `/tmp/eink`, run
   `run-eink-read.sh`. Expected: `A6L_EPD_NOR_JEDEC …`, two identical passes, `A6L_EPD_NOR_PASS`, VCOM digits
   plausible (stock formula). Pull `epd-nor.bin`, hash it on the host, store under `firmware/` (never in git).
   Then offline: `python3 tools/Test-EinkSwtconQemu.py <n> <first 0x70080 bytes>` → real drive frames.
   Failure modes are benign: no JEDEC answer (power GPIO/pins wrong) → nothing else happens.
4. **ADSP candidate A**: push `adsp/` to `/tmp/adsp-diag`, run `adsp_diag_r2.sh`. PASS = running for 20 s and
   clean stop. `FAIL-START*` with "start timed out" → candidate B (CX proxy vote) is the prepared next step.
5. Return to stock Android; verify host cleanup as usual.

Not in V68 on purpose: modem/Wi-Fi (`device/hisense/a6l/kernel/a6l-modem-wifi.dtso`, candidate M1 — RF-capable,
needs rmtfs on RAM copies and a decision about SIM/antenna state), haptics changes, charger changes.

## After V68: framework on the phone

The VM boots the complete framework (V65/V66). The phone version needs the same payload delivered over ADB
into RAM (6 GB total, ~4 GB free under stock; plan: one read-only EROFS image loop-mounted from tmpfs
instead of an expanded tree) and phone variants of the `framework-*.sh` scripts, which deliberately refuse
to run outside QEMU today. That work is offline and continues independently of steps 1–5.
