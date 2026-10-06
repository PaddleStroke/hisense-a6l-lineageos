# rom r6b: boot-hang fix build (30 Sep 2026). Built and verified offline. NOT flashed.

Nothing ran on the phone (no adb, fastboot, EDL, no `.relay/ph.sh`). The laptop was used only to stage the kit and for the
read-only checks named in §7. Metadata/logs: `firmware/extracted/rom-r6b-20260930/`.

## 1. The r6 hang (evidence) and what is known about its cause
- r6 first boot (attended 30 Sep): LCD console to ~6.5 s: `A6L_ROM base: done fails=0`, display group insmods up to
  `drm_dp_aux_bus`, then msm probe (`adreno 5000000.gpu: supply vdd/vddcx not found, using dummy regulator`), then black LCD
  with the backlight on, forever; no USB. metadata/userdata still zero afterwards -> init never reached `mount_all`, i.e. it
  stayed in early-init `exec_start a6l_modules_display`.
- **The black LCD is not the hang point.** The console is the boot console `earlycon=a6lfb keep_bootcon`, which writes into
  the bootloader splash buffer. Once msm/MDSS takes the display path the splash buffer is no longer scanned out, so every
  later line is invisible. The hang can be anywhere after that: in msm, a panel, touch, or a kernel wait in the group.
- **kCFI hypothesis: weakened, not confirmed.**
  - The proven V67 kernel ALREADY had `CONFIG_CFI=y`, `CONFIG_CFI_ICALL_NORMALIZE_INTEGERS=y` and
    `CONFIG_SHADOW_CALL_STACK=y` (`kernel-r5-20260930/config-input-v67`), and the same display modules worked under it in
    the recovery bundles. kCFI/SCS are not r5 deltas.
  - Compile scan with `-Wcast-function-type-strict` (positive control: a `devm_add_action_or_reset((void (*)(void *))clk_disable_unprepare, ...)`
    test module IS flagged): **0 hits** in a full r5-config build of every in-tree module and built-in object
    (`out-a6l-r6b-cfiscan`, series applied: msm, drm helpers, edt-ft5x06, qcom-wdt, camss, q6*, smbx, ...) and **0 hits** in
    every out-of-tree module source the ROM stages (panel-ft8719 = repo source, panel-a6l-epd-dsi = repo = bh2k r5 source,
    tps65185, a6l_simplefb, camfix2 camera set, ipa2_lite (repo), tfa98xx, tmd3702, stk3338, a6l_gpio_vib, kvoice q6voice
    stack, xt_quota2, uid_sys_stats) (`-Wcast-function-type-strict -Wcast-function-type -Wincompatible-function-pointer-types-strict`, W=1).
  - No A6L patch touches msm/DSI/panel core: V67 `source.patch` changes only sdhci-msm, pmdomain core, tty Makefile, udc core;
    the rom-v2 series touches audio, camss, hi846, qcom_smbx, btqca, LPI pinctrl.
  - Remaining r5-only suspects (still hypotheses): HARDENED_USERCOPY, LIST_HARDENED + BUG_ON_DATA_CORRUPTION (a latent list
    bug becomes BUG -> task killed holding its locks), KFENCE, STATIC_USERMODEHELPER="" (only if a firmware user helper path
    is taken), and the ROM-only conditions never seen in the recovery bundles: display bring-up at ~6 s in early-init
    (regulator/clk/interconnect sync_state and `regulator_late_cleanup` at +30 s not yet run), ueventd coldboot running in
    parallel, the new qcom-wdt (base group, probe logged fine), the r5 rebuild of panel-a6l-epd-dsi (bh2k K1, never on the phone).
  - **Root cause: unknown.** r6b is built to name it on the next boot (§3), and to boot to adb even if it recurs.

## 2. Kernel: r5p = r5 thermal + CONFIG_CFI_PERMISSIVE=y (debug/first-install only)
- Fragment `device/hisense/a6l/kernel/configs/a6l-debug-cfi.config` (`CONFIG_CFI_PERMISSIVE=y`). Release keeps strict kCFI:
  `a6l-release.config` now has `# CONFIG_CFI_PERMISSIVE is not set` (checked by build-release-config.sh's value loop).
- Same tree `/home/a6l/kernel/a6l-rom-r5-src` (no source newer than the thermal Image), NEW out dir
  `/home/a6l/kernel/out-a6l-rom-r5p` (script `.relay/r6b-kbuild.sh`, copy in `kernel-r5p-20260930/scripts/build-r5p.sh`).
  diffconfig r5 -> r5p: only `CFI_PERMISSIVE n -> y`. W=1 warning count identical to the thermal build (1098).
- **Module ABI unchanged:** Module.symvers byte-identical to r5 and to the thermal build; System.map symbol set identical;
  all 7338 `__versions` imports of the 124 staged r5 modules match; in-tree modules rebuilt in r5p differ from the thermal
  ones only in `.note.gnu.build-id`. The staged r5 modules are used unchanged.
- Artifacts `firmware/extracted/kernel-r5p-20260930/` (README, SHA256SUMS): Image `124054c5…fdbd3b`, Image.gz `69ed5fce…`.
- Selected with `A6L_KERNEL=r5 A6L_KERNEL_IMAGE=r5p` (tools/Prepare-RomV2Boot.py, tools/Test-RomV1Qemu.py; the sha is asserted).
- Boot cmdline (all builds): `+ log_buf_len=4M` (LOG_BUF_SHIFT=17 = 128 KiB would lose the early boot before the log starts).
- **pstore/ramoops (added, cheap):** DT overlay `device/hisense/a6l/kernel/a6l-ramoops-v75.dtso` (merged by
  tools/build-rom-v2-dt.sh) turns the stock Hisense `recorder_mem` reservation (0xb0180000, 4 MiB, no-map; stock
  `rs-recorder` crash recorder, so DDR retention across reset is the stock design) into `ramoops` (console 2 MiB, pmsg 256 KiB,
  128 KiB oops records, max-reason OOPS). The r5 kernel has PSTORE_RAM=y. No new RAM taken. DT diff vs dt-r6: only these
  properties. Read on the NEXT r6b boot (the boot log copies /sys/fs/pstore to /metadata/a6l/pstore-prev/). NOT read from the
  V75 recovery: its DT has no ramoops node and /dev/mem of a no-map region would fault; do not try.

## 3. Vendor: init never blocks on module loading (userdebug and release)
`device/hisense/a6l/rom/init/init.qcom.rc`, `rom/bin/a6l-modules.sh`:
- early-init: `start a6l_modules_display` (was `exec_start`). Init goes on to mount_all, USB gadget (init.a6l.usb.rc
  early-boot) and adbd whatever the display group does. USB/adbd rc has no dependency on the group (test B8).
- The group: main process = loader (base + display lists, then LCD wait, simplefb fallback, e-ink hand-off). A decider child
  ALWAYS publishes `vendor.a6l.gpu` within a bound: mesa as soon as renderD128 exists; angle 10 s after the lists are loaded
  (as before) or 30 s after the start if they are still loading (an insmod hangs). State `vendor.a6l.display` =
  loading -> loaded -> done (stays loading if an insmod never returns).
- F11 semantics kept (comments updated): post-fs-data runs `exec_start a6l_display_wait` (= `a6l-modules.sh displaywait done 45`,
  bounded 45 s: returns when the group is done, i.e. LCD connector ready as the composer expects) and THEN
  `setprop persist.graphics.egl ${vendor.a6l.gpu:-angle}` (after load_persist_props, before zygote/SurfaceFlinger).
- Charger mode: `on early-init && property:ro.bootmode=charger` -> the same bounded wait (charger UI needs the panel).
- adsp group: waits <= 30 s while the display lists are loading (shared providers mdt_loader, qcom_aoss, llcc), then goes on.
- Composer: drm_hwcomposer's `vendor.hwcomposer-3` is not `critical`; if the display arrives late (hang case) init restarts it
  (and surfaceflinger via onrestart) until the card exists. In the normal case the 45 s wait keeps the old guarantee.
- Every insmod is logged BEFORE it runs (`A6L_ROM display: insmod msm.ko ...` then `... ok`), so the last line names the
  module that never returned.
- Stage markers to kmsg: `A6L_STAGE early-init`, `init`, `fs: mount_all`, `fs: mount_all done`, `post-fs-data`, `boot`
  (+ the existing `A6L_ROM_BOOT_COMPLETED`).
- rc-enforcing.patch regenerated (a6l_selinux_prep.py diff; the new a6l_display_wait service loses its vendor_modprobe
  seclabel under A6L_SELINUX_PREP like the others and runs as a6l_modules by file label).

## 4. Persistent boot log (userdebug/eng only; never in user)
- `rom/debug/bootlog.mk` (inherited by rom.mk, `ifneq ($(TARGET_BUILD_VARIANT),user)`), `rom/debug/init.a6l.bootlog-debug.rc`,
  `rom/debug/a6l-bootlog.sh`. Service `a6l_bootlog` (su, like the debug a6l_logcat) starts at **post-fs** (/metadata mounted).
- Streams /dev/kmsg (whole ring from 0 s thanks to log_buf_len=4M, then live) to **/metadata/a6l/boot-kmsg.txt**; the previous
  boot's file -> `boot-kmsg.txt.prev`; pstore of the previous boot -> `pstore-prev/`. Cap 8 MiB; stops 120 s after
  sys.boot_completed. sync every 0.5 s while the display group is loading, else every 2 s.
- **Debug builds load the display lists only after the log runs** (`a6l-modules.sh display` waits <= 20 s for
  `vendor.a6l.bootlog=running`; not in user builds, not in charger mode). In QEMU: log running at 14 s, lists at 19.5 s.
  A death inside a display insmod therefore leaves the log with its `insmod X ...` line on the eMMC (<= 0.5 s lost).
- If the display group is still running 30 s and 90 s after the log start: `ps` D-state tasks, their /proc/<pid>/stack,
  and `echo w > /proc/sysrq-trigger` (all blocked tasks' stacks into kmsg -> same file).
- Sepolicy: no new types (su + default metadata_file); a6l_selinux_prep.py lists the rc as DEBUG_RC (su expected);
  check-a6l-sepolicy user/userdebug PASS, test-selinux-prep PASS.

## 5. Build r6b
- Same config as r6 (docs/rom-r6-build-20260929.md): lunch userdebug, test keys, adb on (ro.adb.secure=1), permissive,
  A6L_SEPOLICY_ROM=1, `A6L_KERNEL=r5` + `A6L_KERNEL_IMAGE=r5p`; marker `ro.vendor.a6l.rom.build=r6b`.
- `systemd-run --unit=a6l-rom-r6b2 --setenv=A6L_KERNEL=r5 --setenv=A6L_KERNEL_IMAGE=r5p --setenv=A6L_SEPOLICY_ROM=1 ... rom-v2-pipeline.sh r6b prep build boot flash qemu`
  (first attempt stopped by hand during analysis to pick up the last script change; build 17:55).

| image | bytes | sha256 | vs r6 |
|---|---|---|---|
| boot.img | 67 108 864 | `3616b0fa63e8af3928d1d5df146f73327620270d81198968c82f8140be0a2947` | changed (r5p Image, DT + ramoops, cmdline) |
| dtbo.img | 8 388 608 | `6925112258af276ad2d16757df054bbc6e98148b5273f0a214e7d8ddd5044e7e` | same |
| system.erofs | 1 741 164 544 | `d6a6af4dcbbb6474bd8dc8c10f2ac34856a82ddf7dc5c22b7903fcc951332a39` | changed (build date props only; same fingerprint) |
| vendor.erofs | 181 751 808 | `e411a797fb056c72606b8bdfd1a93439751a78971d51c2d348c9c767aeb245c6` | changed |
| rom-v1-pins.json | 1 922 | `642df9f98ee9b81c8f93e139852c8195114ba6fd08568712dcb9ec7c2b12c00c` | |

QEMU variant boot `9d51b099…`. DT (dt-r6b) = dt-r6 + ramoops properties.

## 6. Verification (offline)
| check | result |
|---|---|
| preflight (before the build) | kseries PASS, stage dry runs v67 + r5 PASS, check-a6l-sepolicy user + userdebug PASS (256 types), ROM static PASS, r6 static PASS, selinux-prep PASS |
| test-rom-static.sh (new B1-B11) | PASS, incl. a simulated hang: msm.ko insmod blocks -> vendor.a6l.gpu=angle published, state stays loading, last line `display: insmod msm.ko ...`, loader resumes after |
| build | `#### build completed successfully (17:55)` |
| boot packaging | ROM_V2_BOOT_PASS (phone + QEMU); captured-ABL emulation `passed: true`, avb_loaded boot+dtbo |
| kit | ROM_V2_STAGE_PASS |
| virtual-eMMC install/restore | ROM_V1_FLASH_TESTS PASS 8/8 |
| check-rom-v2-image.sh (r6b pins + r6b items) | CHECK_ROM_V2_IMAGE PASS (98 ok) |
| QEMU (r5p Image) | see §6.1 |
| keep-data | test-rom-update-keepdata.py **13/13 PASS** (new: update_writes_only_changed); Prepare-RomUpdateStage r6 -> r6b **A6L_UPDATE_STAGE_PASS**; Test-RomUpdateFlash r6 -> r6b (real geometry) **4/4 PASS**, `unchanged_skipped=['dtbo']`, plan boot/vendor/system |

### 6.1 QEMU
**ROM_V1_QEMU PASS** (r5p Image): boot_completed 1233 s (r6: 1062 s; QEMU timing, the gap is init.rc's apexd wait), no panic,
stable 42 s, zygote once. Groups base/display/adsp/audio/misc/charger/camera all `fails=0`; 0 `CFI failure`, 0 "disagrees",
0 "Unknown symbol", 0 usercopy. New ordering seen in the log:
- `A6L_STAGE early-init` 5.3 s (display group started, not waited for), `fs: mount_all` 9.4 s, `mount_all done` 12.9 s
  (r6: mount_all only at 67 s, after the blocking display group);
- `a6l_bootlog` started 14.2 s (post-fs), `display: boot log running after 13x0.2s` 19.5 s, `insmod X ...`/`ok` pairs,
  `display: done fails=0` 42 s, decider `NO renderD128 -> EGL angle` (QEMU), LCD fallback;
- `A6L_STAGE post-fs-data` 183 s, `displaywait done: vendor.a6l.gpu='angle' vendor.a6l.display='done' after 0x0.2s`,
  `A6L_STAGE boot` 196 s, composer started 193 s.
AVC denials 1254 (1010 vendor_modprobe; permissive, same class as r6's 1042/784 - more module lines).

## 7. Keep-data update kit r6 -> r6b (adb-recovery)
- `tools/RomUpdateEngineV1.py` (r6b): writes only the partitions that CHANGED: a partition whose new image is byte-identical
  to the installed (previous-kit) image and whose head was just read as that image is not written (report
  `unchanged_skipped`); `--reflash` writes all four. A partition identical in both kits no longer counts in the
  "already installed" test. For r6 -> r6b: **boot, vendor, system written; dtbo skipped**. New host test
  `update_writes_only_changed`.
- WSL kit `/home/a6l/rom-v2/kit-r6b/rom-v2` (images + rom-v1 tools + update tools + `update/` with prev = kit-r6).
  Laptop: `~/A6L-usb-20260915/rom-r6b` (`stage-rom-v2-kit-laptop.sh r6b rom-r6b`: LAPTOP_SHA_OK, LAPTOP_SWAP_OK,
  STAGE_KIT_DONE). Laptop read-only checks: `Verify-RomV1Stage.py` "passed": true (r6b hashes); `Run-LaptopRomUpdate-v1.py
  --mode update|backup-only --transport adb-recovery --dry-run` both "passed": true, compat_problems [] (same fingerprint
  `…/CP2A.260605.016/eng.root:userdebug/test-keys`, build date 1790718145 -> 1790762556); no file created in the kit dir.
  The dry-run plan lists all four partitions; dtbo is dropped at run time once the predecessor read proves it identical.
- The adb transport expects `hisense,a6l-image` = `v74`; the installed V75-usb diagnostic recovery reports `v74` on purpose
  (docs/usbrec-20260926.md), and it insmods /sdhci-msm.ko itself.

### 7.1 Attended sequence (Pierre present; phone in the V75 diagnostic recovery, laptop port 3-2)
The phone state reported by Pierre (Power+Vol-down -> bootloader screen -> a console with USB "CONFIGURED" lines) is the
expected V75-usb diagnostic recovery (its A6L_USBWD watchdog prints `CONFIGURED`); it is what the adb-recovery transport needs.

On the laptop (desktop session, `pierrelouis@system76-pc`), battery >= 50 % advised:
```
cd ~/A6L-usb-20260915/rom-r6b
adb devices                                         # HLTE730T-PROBE  device   (only this one)
python3 Verify-RomV1Stage.py | tail -8              # "passed": true, boot 3616b0fa…, system d6a6af4d…, vendor e411a797…
python3 Run-LaptopRomUpdate-v1.py --mode update --transport adb-recovery --dry-run | grep -m3 -E '"passed"|compat_problems'
                                                    # "passed": true, "compat_problems": []
python3 Launch-RomUpdateV1.py --mode update --transport adb-recovery && tail -f rom-update-update-launch.log
```
Expected: `{"launched_pid": …, "log": "rom-update-update-launch.log"}`; in the log, after the dry-run JSON:
`backup boot …`, `backup dtbo …`, `backup vendor …`, `backup system …` (7.1 GB over adb, time not yet measured; worker bound
5500 s), `unchanged (not written) dtbo`, `written boot`, `written vendor`, `written system`, `readback ok boot|vendor|system`,
then `Update written and read back, protected regions unchanged. Phone off: press Power.` and
`{"error": null, "worker_exit": 0, "services_restored": true}`. **If it stops: keep USB connected, do not retry; inspect
`capture-rom-update-update-<UTC>/edl/report.json`.**

Verify, then keep the capture (it is the r6 backup for `--mode rollback`):
```
python3 -c "import json,glob;r=json.load(open(sorted(glob.glob('capture-rom-update-update-*/edl/report.json'))[-1]));print({k:r.get(k) for k in ('predecessor','unchanged_skipped','readback_verified','invariants_unchanged','power','error')})"
# -> predecessor all 'prev', unchanged_skipped ['dtbo'], readback_verified True, invariants_unchanged True, power 'off', error None
```
First r6b boot: press Power. The LCD console should now show `A6L_STAGE early-init …`, `A6L_STAGE fs: mount_all` (first boot
formats metadata + userdata), and in debug builds the display insmods AFTER the boot log starts (`A6L_ROM display: boot log
running`, then `insmod X ...` / `ok`). On the laptop: `watch -n2 adb devices` - within ~1-3 min a LineageOS adb device
(`1e529013` expected) must appear even if the display hangs; it is `unauthorized` until "Allow" is tapped on the LCD
(ro.adb.secure=1, no pre-trusted key). If the LCD works: tap Allow, then `adb shell getprop ro.vendor.a6l.rom.build` (= r6b) and
`adb shell logcat -b kernel -d | grep -E "A6L_(ROM|STAGE)|CFI failure"`.

Read the persistent boot log after a failed (or any) boot: force off (hold Power ~15 s), Power+Vol-down -> V75 recovery
(`adb devices` = HLTE730T-PROBE), then:
```
adb -s HLTE730T-PROBE shell 'T=/system/bin/toybox; $T grep -q "^sdhci_msm " /proc/modules || { $T insmod /sdhci-msm.ko; $T sleep 6; }; d=; for b in /sys/class/block/mmcblk*p*; do $T grep -qx PARTNAME=metadata $b/uevent && d=$($T cat $b/dev); done; echo metadata=$d; $T mkdir -p /dev/block/a6lmeta /mnt/a6lmeta; $T rm -f /dev/block/a6lmeta/md; $T mknod /dev/block/a6lmeta/md b ${d%%:*} ${d##*:} && $T mount -t ext4 -o ro,noload /dev/block/a6lmeta/md /mnt/a6lmeta && $T ls -la /mnt/a6lmeta/a6l /mnt/a6lmeta/a6l/pstore-prev'
adb -s HLTE730T-PROBE exec-out '/system/bin/toybox cat /mnt/a6lmeta/a6l/boot-kmsg.txt' > r6b-boot-kmsg-1.txt
adb -s HLTE730T-PROBE exec-out '/system/bin/toybox cat /mnt/a6lmeta/a6l/boot-kmsg.txt.prev' > r6b-boot-kmsg-1.prev.txt   # older boot, if any
adb -s HLTE730T-PROBE shell '/system/bin/toybox umount /mnt/a6lmeta'
grep -a -n -E "A6L_STAGE|A6L_BOOTLOG|A6L_ROM|CFI failure|Call trace|BUG:|Oops|Unable to handle|blocked|state:D" r6b-boot-kmsg-1.txt | tail -80
```
Expected `metadata=179:NN` (major 179), the mount succeeds read-only (noload: no journal replay, nothing written), the file
starts `A6L_BOOTLOG start uptime=… build=r6b kernel=7.2.3-a6l-probe+`, then kmsg records (`<level>,<seq>,<usec>,-;text`)
from 0 s. Reading: the last `A6L_ROM display: insmod X ...` without its `ok` names the module that never returned; the
`A6L_BOOTLOG 30s/90s` sections plus the sysrq-w `task:… state:D` stacks name the wait/lock; `CFI failure at …` lines (now
warnings) name a kCFI mismatch. If the mount fails (no ext4 on metadata), init never reached mount_all again: the kernel
itself stalled before post-fs -> next step is the pstore copy on the following boot (`pstore-prev/`) or a display-less boot.
Copy the capture and the log files to the desktop `captures/`.

Rollback to r6 (keeps user data; phone in the V75 recovery): `python3 Launch-RomUpdateV1.py --mode rollback --transport
adb-recovery --update-capture capture-rom-update-update-<UTC>/edl`. Back to stock: `Launch-RomV1Restore.py` with the r6 install
capture (`captures/capture-rom-r6-install-20260930`, laptop `rom-r6/capture-rom-v1-install`).

## 8. Open / notes
- Root cause of the r6 hang: unknown (§1). r6b should either boot (then the permissive CFI or timing difference matters:
  grep the log for `CFI failure`), or show the hanging module and its stack in /metadata/a6l/boot-kmsg.txt.
- **ro.adb.secure=1 and no pre-trusted key** (as r6): if the display is dead, "Allow USB debugging" cannot be tapped. `adb
  devices` still proves USB/adbd (state `unauthorized`); the log is read from the recovery (§7.1). A debug variant with the
  laptop key pre-trusted exists (`A6L_DEBUG_ADBKEY=1`, rom/debug/adbkey.mk) and needs Pierre's decision.
- r6b is a DEBUG build: CFI permissive + boot log. A release build keeps strict kCFI and no boot log (user variant).
- Writes to the eMMC from the V74/V75 recovery kernel were never done on hardware (only reads); the r6 install capture
  (`captures/capture-rom-r6-install-20260930`) is the way back to stock via EDL.
- WSL leftovers: `/home/a6l/kernel/out-a6l-rom-r5p`, `/home/a6l/kernel/out-a6l-r6b-cfiscan` (scan only, can be deleted),
  `/home/a6l/rom-v2/{boot-r6b,boot-r6b-qemu,dt-r6b,flashtest-r6b,qemu-r6b,kit-r6b}`.

## 9. Files changed
- kernel: `configs/a6l-debug-cfi.config` (new), `configs/a6l-release.config` (CFI_PERMISSIVE not set), `a6l-ramoops-v75.dtso` (new)
- rom: `init/init.qcom.rc`, `bin/a6l-modules.sh`, `rom.mk` (r6b marker, bootlog.mk), `debug/{bootlog.mk,init.a6l.bootlog-debug.rc,a6l-bootlog.sh}` (new),
  `tests/test-rom-static.sh` (B1-B11), `sepolicy/rc-enforcing.patch` (regenerated)
- tools: `Prepare-RomV2Boot.py` (A6L_KERNEL_IMAGE=r5p, log_buf_len), `Test-RomV1Qemu.py` (r5p), `build-rom-v2-dt.sh` (ramoops),
  `check-rom-v2-image.sh` (r6b), `RomUpdateEngineV1.py` (only changed partitions), `tests/test-rom-update-keepdata.py`,
  `release/a6l_selinux_prep.py` (DEBUG_RC)
- `.relay/r6b-{kbuild,kcollect,cfiscan,cfiscan-oot,preflight,kit}.sh`
- `firmware/extracted/kernel-r5p-20260930/`, `firmware/extracted/rom-r6b-20260930/`
