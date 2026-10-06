# SELinux enforcing readiness + release pieces (29 Sep 2026)

Worker: selinux-release. Offline only: no phone, no adb/fastboot/EDL, nothing built with `m`, nothing flashed.
Follows docs/release-prep-20260927.md section 3 (policy, order of operations) and completeness audit item 16.
**The build is NOT switched to enforcing.** Every change is behind a flag and is off by default. Pierre's first install keeps
the developer settings: userdebug, adb on, `androidboot.selinux=permissive`.

## 1. What is ready (offline, tested)

| piece | where | state |
|---|---|---|
| Flag `A6L_SELINUX_PREP=1` (also implied by `A6L_RELEASE=1`) | `rom/selinux/{BoardConfig-selinux.mk,selinux.mk}`, `rom/BoardConfig-rom.mk` (filter + include), `rom/rom.mk` (inherit), `tools/rom-v2-pipeline.sh` (prep) | ready, default off |
| rc transform, replaces the stale static patch | `tools/release/a6l_selinux_prep.py apply` runs on the TREE copy in pipeline prep. It is idempotent and fails with `DRIFT:` when an anchor has moved. | ready |
| `rom/sepolicy/rc-enforcing.patch` | regenerated from the transform against the current tree. `git apply --check` passes. It is a reference only: never apply it to the repo. | ready |
| Known issue: a6l_logcat ran as su | with the flag, the service and its trigger leave init.qcom.rc and move to `rom/selinux/init.a6l.logcat-debug.rc`, which is installed only in userdebug/eng. A user build has no su service. The script runs `/system/bin/logcat`, which a vendor domain may not exec, so su is the only option. | ready |
| Known issue: /dev/dri 0666 | with the flag, `/dev/dri/card*` becomes 0660 system:graphics. Its users are the composer, the epdd lease, the e-ink mirror and dualux (all uid system or group graphics), plus the charger (uid system). `renderD*` stays 0666 for Mesa in apps. | ready |
| vendor_modprobe seclabels (11 services) | removed by the flag. The scripts get a6l_modules, a6l_radio_ctl, a6l_chg_guard and a6l_macs from the rom/sepolicy/vendor file labels. | ready |
| Service/domain checker | `a6l_selinux_prep.py services <dev> --aosp <tree>/system/sepolicy [--prep]` checks each service in the 18 installed vendor rc files. A service needs a seclabel or a file-label transition, otherwise init refuses to start it even in permissive mode. Default policy: PASS (12 WARN = vendor_modprobe/su). Prep: **PASS, 0 WARN**. The pipeline runs it after apply. | ready |
| Charger UI (off-mode charging) | `rom/charger/charger.mk`: the AIDL health HAL `--charger` (libhealthd_charger_ui, minui on DRM) with the LineageOS charger images `lineage_charger_animation_vendor` (already in the r5 out: /vendor/etc/res/{images,values}/charger). New `rom/charger/BoardConfig-charger.mk` sets `TARGET_SCREEN_DENSITY ?= 400`, which selects the xxhdpi images instead of mdpi (a 160 px battery) and the recovery density. It also emits `ro.sf.lcd_density=400`, the same value rom.mk sets, and post_process_props accepts an identical duplicate. Enforcing fix: `rom/selinux/sepolicy/vendor/charger_vendor.te`, because /dev/dri is `gpu_device` on this device and AOSP only grants charger `graphics_device`, so in enforcing mode the charger showed no UI ("gr_init failed"). | ready, **not built** |
| Updater placeholders | the non-release URI `.../updater/unpublished/{device}.json` (android.mk, overlays worker) and the release URI (release.mk) already existed. New `rom/release/updater/a6l.json` = `[]` is the initial public feed (an empty v2 list means "no update") and `README.txt` records the rules. | ready |
| Audit-driven policy plan | `tools/release/Collect-AvcV1.py <tag> [--root]` (ATTENDED, read-only adb): collects dmesg/logcat avc, `ps -AZ`, `ls -lZd` of every genfs path (including the UNVERIFIED ones), by-name/dev/vendor labels and getprop, then runs `tools/release/a6l-avc-plan.py`. The planner is offline. It groups denials by (scontext, tcontext, tclass) and classifies each group as LABEL (generic type: label the object), NEVERALLOW (vendor exec of system binaries, dac_override, core props, HAL sockets: fix the code), ALLOW (the rule goes to the .te of the dir that declares the domain), PLATFORM or DEBUG. It marks permissive=0 lines BLOCKING, lists processes still running as init/vendor_modprobe/su, and writes `plan.md` + `proposed/<dir>/<domain>.te`. It never edits policy dirs. | ready |
| Pipeline bug found and fixed | `rom/r6/r6.mk` inherits `device/hisense/a6l/watchdog/watchdog.mk`, but the prep sync never copied `watchdog/` into the tree, so the next r6 build would have failed at product config. `watchdog` has been added to the sync and crlf lists. | fixed |

Tests: `bash device/hisense/a6l/rom/selinux/tests/test-selinux-prep.sh` gives A6L_SELINUX_PREP_TEST PASS: transform, drift refusal, idempotency, the patch is current and applies, flag wiring, charger, updater, planner on a fixture, and service domains against the real system/sepolicy (WSL). `check-a6l-sepolicy.sh user rom/sepolicy/vendor rom/selinux/sepolicy/vendor` PASS (255 types, 1086 allow). The same check on userdebug also PASS. ROM static PASS, r6 static PASS. No module was staged, so the stage dry runs were not needed.

## 2. How to use it
```
# first install (Pierre's go): unchanged default = permissive, explicit seclabels, no prep
# recommended when convenient (still permissive, attributable avc log):
A6L_SELINUX_PREP=1 bash tools/rom-v2-pipeline.sh r6p prep build boot
# attended, after exercising every feature:
python3 tools/release/Collect-AvcV1.py r6p --root      # -> captures/avc-r6p-*/plan.md
# fix per plan -> check-a6l-sepolicy.sh user ... PASS -> rebuild; then the enforcing trial (userdebug):
A6L_SELINUX_PREP=1 A6L_SELINUX=enforcing bash tools/rom-v2-pipeline.sh r6e prep build boot
```
`A6L_SELINUX_PREP=1` in the build phase without the prep phase stops the build with a clear error. The marker is written only by the transform.

## 3. Needs an attended test
- Off-mode charging UI: power off with the charger plugged in. Expect the Lineage battery animation on the LCD, the percentage, and a long press on power to boot. Check what appears on the e-ink side, because minui takes the first connected connector and that is not proven to be the LCD. Check `dmesg | grep charger`.
- The first permissive install with `A6L_SELINUX_PREP=1`: all services start (`ps -AZ | grep a6l`; no `init`/`vendor_modprobe` domains), then Collect-AvcV1 → plan.
- Check the UNVERIFIED genfs paths from labels.txt (remoteproc, ipa, DPU/DSI, iio, power_supply).
- `/dev/dri/card0` 0660: the composer, e-ink mirror, dualux and epdd lease still work, and `ls -l /dev/dri`.
- Enforcing trial on userdebug (rescue: reflash the permissive boot.img or use the V74 recovery).

## 4. Needs Pierre
- Whether the first install uses `A6L_SELINUX_PREP=1`. It is still permissive, but the services change domain. Recommended for a readable avc log. The default without the flag is exactly the r6 as merged.
- When to go enforcing and when to go to user builds. Release keys, adb off and radio default ON are covered in release-prep §6.
- Updater feed hosting (public repo `updater/a6l.json` = `[]` to start), and whether the Lineage recovery replaces the V74 diagnostic recovery.

## 5. Open (not done here)
- Real denials exist only after the first install. The policy is still unverified at runtime.
- The charger UI and the density change are not built. The first `m` with this tree confirms them.
- XTRA/PSDS download path (release-prep §3.2) and `/persist` stock xattrs: unchanged.
