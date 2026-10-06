# Update that keeps user data (agent `update-keepdata`, 29 Sep 2026)

Offline only. No phone, adb, fastboot, EDL or laptop access. Nothing was flashed and no build was run.
Completeness audit item P1 #10 ("the update path wipes data", release-prep S11/S15).

## 1. The problem
Every install today goes through `Run-LaptopRomInstall-v1` (EDL). It checks that the phone runs **stock** Android and
zeroes the first MiB of userdata and metadata, so each new build (r5 → r6 → …) costs all user data. The recovery slot holds
the V74 diagnostic recovery, which cannot install an OTA.

## 2. What is ready (code + tests, all offline)
The new mode writes **only boot, dtbo, vendor and system**. It never writes userdata, metadata, persist, modem EFS
(modemst1/2, fsg, fsc), misc, recovery, vbmeta, devinfo or the GPT.

| file | role |
|---|---|
| `tools/RomUpdateLayoutV1.py` | partition-**name** allowlist (`UPDATE_WRITABLE`) and denylist (`NEVER_WRITE`); `check_update_plan`: each write must start at the start of one allowed partition, stay inside it, overlap nothing else and not the GPT, no duplicates, label = partition; invariant regions; `compat_problems` data rules |
| `tools/RomUpdateEngineV1.py` | `run_update` (modes `update`, `backup-only`) and `run_rollback`. Device-independent: file, EDL or adb |
| `tools/RomUpdateAdbV1.py` | adb transport in the **V74 diagnostic recovery**. It avoids EDL, whose entry from LineageOS is unproven (see §4) |
| `tools/Write-LaptopRomUpdateV1.py` | worker: `--mode update|backup-only|rollback`, `--transport edl|adb-recovery`, `--dry-run`, `--userdata-full`, `--reflash`; `UpdateGuard` (Firehose XML allowlist) |
| `tools/Run-LaptopRomUpdate-v1.py`, `tools/Launch-RomUpdateV1.py` | coordinator and launcher (idle inhibitor, host pause for EDL, phone-state checks, `--dry-run`) |
| `tools/Make-RomUpdateCompat.py` | describes a kit (fsck.erofs extract): system fingerprint, build date, SDK, security patch (system and vendor), platform cert sha256, `/data` fs and encryption flags, image hashes |
| `tools/Prepare-RomUpdateStage.py` | adds the update tools and `update/` (new compat, prev pins and compat, tool pins, SHA256SUMS) to a kit dir. Never overwrites; runs both dry runs |
| `tools/tests/test-rom-update-keepdata.py` | host test, mini eMMC, **12/12 PASS** (repo tools and WSL) |
| `tools/Test-RomUpdateFlash.py` | end to end on a sparse **real-geometry** eMMC with the real stock GPT and bytes: r3 wipe install → "ROM ran" → backup-only → update to r5 → rollback. **4/4 PASS** (WSL) |

**Sequence (`update`).**
1. Offline checks, with no device access: payload hashes, tool pins, compat rules, plan allowlist.
2. Identity checks: GPT primary and tail are the spare's, geometry parsed; vbmeta is stock; devinfo is unlocked; the BCB
   is empty or bootonce-bootloader.
3. Hash the **invariant regions**: GPT, misc, metadata, persist, EFS, recovery, vbmeta, devinfo, userdata first 64 MiB and last MiB.
4. Full backup of boot, dtbo, vendor and system, with a second read of boot and dtbo.
5. **Predecessor check.** Each partition head must equal the image the previous kit installed (`prev-rom-v1-pins.json`).
   A stock or unknown build is refused. If the new build is already installed, the run is refused unless `--reflash` is given.
6. Write the four partitions and read back every written range.
7. Re-hash the invariant regions. They must be byte-identical, then power off.

**`backup-only`** is the rehearsal. It runs every check and reads everything, and writes nothing (the guard has no plan,
so every `program` is refused). It then resets. `--userdata-full` also copies the whole 107 GiB userdata (≈1.5–2 h,
~120 GB free needed). **`rollback`** writes back the four partitions saved by an update capture, keeping the data. It works
after a failed update too (state `failed-write`/`update`/`saved`) and refuses an unknown state or an edited capture layout.

**Compat rules** (`compat_problems`). All of these must hold before any device access:
- the same platform certificate;
- the same `/data` fs and encryption flags;
- SDK, security patch and vendor security patch must not go down;
- no field may be missing.

r3 → r5: compatible (ext4, no encryption, test-keys cert `c8a2e9bc…`, SDK 37, patch 2026-09-01; the build dates differ).
Every eng build carries the same fingerprint (`…eng.root:userdebug/test-keys`). The coordinator's early check therefore
compares the fingerprint **and** `ro.system.build.date.utc`. The real identity check is the partition hashes.

**Safety layers.**
- **Names:** allowlist and denylist, asserted disjoint.
- **Geometry:** the 14 Sep GPT, checked against the live GPT.
- **Transport:**
  - EDL: `UpdateGuard` re-checks every program XML against the fixed plan and the update allowlist. The base `Guard` does
    not have to be the fixed version (see finding 1).
  - adb: preflight checks every sysfs PARTNAME start and size against the layout before any `mknod`. Programs go only
    to partition nodes (never the whole-disk node) and must start at a partition start.
- **Dry runs:** worker, coordinator and stage.
- **Captures:** `capture-rom-update-<mode>-<UTC>/` is never reused, and the stage script already refuses to overwrite any `capture-*`.

## 3. Findings while building it (fixed in the new code; old tools untouched)
1. **kit-r5 carries the pre-bug-hunt-2 rom-v1 worker.** It has no `Guard.set_plan`, a self-registering program allowlist
   and no `L.check_plan`. The first stage dry run on kit-r5 failed on this. `UpdateGuard` now fixes and enforces its own
   plan whatever base version the kit has; test `worker_update_guard_old_base` covers it. **The r5 kit's install/restore
   tools are still the unfixed ones.** Stage r6 (or re-stage) before the first install, as the ledger says.
2. **`dd` over a pipe truncates.** An `adb exec-in 'dd of=… bs=1M iflag=count_bytes'` wrote 64 KiB of 115 KiB in the host
   test (short pipe reads; toybox has no `iflag=fullblock`). flash-20260924 §5 plan B suggests exactly
   `adb exec-in 'dd of=/dev/mmcblk1pNN bs=4M'`. **Do not use that line as written.** The transport now uses
   `toybox cat > <node>`, then sync, flushbufs and drop_caches, and every range is read back.

## 4. Needs an attended test (in order, Pierre present)
0. Stage: `python3 tools/Prepare-RomUpdateStage.py /home/a6l/rom-v2/kit-r6/rom-v2 <kit of the installed build>` gives
   `A6L_UPDATE_STAGE_PASS`. Then run `stage-rom-v2-kit-laptop.sh` (it copies `update/`) and, on the laptop:
   `python3 Run-LaptopRomUpdate-v1.py --mode update --transport adb-recovery --dry-run` → `"passed": true`.
1. **Transport entry.** From LineageOS, `adb reboot recovery` (PON recovery is supported) should boot V74 with adb
   `HLTE730T-PROBE`. Check `toybox` and `/sdhci-msm.ko` and that `mmcblk1` enumerates.
   - The LineageOS adb serial is assumed to be `1e529013`. Confirm it.
   - EDL entry from LineageOS is **unproven**: our kernel has no `reboot edl`, so `--transport edl` is only for a phone
     already in EDL (`--already-in-edl`).
2. **`backup-only --transport adb-recovery`.** Proves the preflight geometry check on the real sysfs, adb read throughput
   (7.1 GB backup; time it) and invariants. Nothing is written.
3. With data on the phone (a few apps, Wi-Fi networks, an SMS, a PIN): **`update`**. Then boot and check that the data is
   still there. The first boot is slow: dalvik-cache is rebuilt and PackageManager rescans. Also check aconfig
   (/metadata kept) and that keystore/Gatekeeper still unlock with the PIN.
4. **`rollback`** rehearsal from that capture, then boot again.
5. Not tested on hardware at all: writes to the eMMC from the V74 kernel (only reads were done before), and adb `exec-in`
   of 1.7 GB.

## 5. Plan: Lineage recovery and signed OTA (later; V74 stays for now)
- **Now:** this EDL/adb keep-data kit is the update path between our test-keys builds. The V74 diagnostic recovery stays
  in `recovery`: it is the rescue path and the adb-recovery transport.
- **Before replacing V74:** prove an EDL entry that does not need stock Android (hardware keys or test point), since
  EDL is the only rescue path left without V74. Also prove the rollback (`Test-RomV1Flash` plus an attended restore).
  Our ABL has no `fastboot boot`, so the Lineage recovery cannot be RAM-tested. Its first boot means writing it to
  `recovery` (V74C-style EDL tool or an adb-recovery variant with `recovery` added to a *separate* allowlist).
- **Lineage recovery** (prepared, untested: `rom/release/`, release-prep §2) needs `A6L_RELEASE=1` and brings full
  recovery image, recovery-DTBO, simpledrm and adb via the soft_connect gate.
- **Signed OTA:** the switch test-keys → release-keys changes the platform cert, so `compat_problems` refuses it. It needs
  **one last wipe install** (EDL kit). After that, OTAs (Updater JSON, `make-updater-json.py`) keep data like this kit.
  Keep the V74 image in the kit.
- `AdbRecoveryDevice(expected_image=None)` should also work under a Lineage recovery's root adb shell (toybox present).
  Not tested.

## 6. Needs Pierre
- A go for steps 1–4 of §4.
- Whether to take a full userdata copy (`--userdata-full`, about 2 h) before the first real keep-data update.
- Whether a hardware EDL entry exists (14 Sep; not documented).
- Later: when the Lineage recovery replaces V74, and the release-key switch (one last wipe).
