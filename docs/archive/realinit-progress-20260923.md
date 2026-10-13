# Real Android init from RAM — progress 23 Sep 2026 (agent `realinit`, VM only, phone untouched)

Roadmap step 2: replace the hand-written supervisor (`framework_root_services.c` + `framework-phone-v71.sh`) with
Android's own `init` (selinux_setup -> second stage) running as PID 1 of a private PID + mount namespace, fed by the
build's labeled `system.erofs` + `vendor.erofs`. Design: `docs/real-init-container-design-20260921.md`.
Launcher: `device/hisense/a6l/diagnostic/realinit-launch.sh`. Harness: `tools/Test-RealInitV80.py <attempt> [minutes]`
(QEMU virt + V67 phone kernel, run dirs `/home/a6l/kernel/realinit-v80-r<N>`, archives `firmware/extracted/realinit-v80-r<N>/`).

Status: **not at `sys.boot_completed` yet.** The three r18 blockers are explained and two of them fixed; boot now
gets through post-fs and post-fs-data (r18 was stuck in post-fs). New blocker: keystore2 SIGABRT loop in post-fs-data,
and the container's userspace seems to lose `/system/bin` exec after post-fs-data (APEX re-activation suspected), see below.
No `m` build was run this session. The images are still the 21 Sep build (system 1 327 452 160 B, vendor 15 245 312 B).

## Root causes found (r19–r23)

| r18 symptom | Real cause | Fix | Verified |
|---|---|---|---|
| hwservicemanager SIGABRT (every ~10 s) | `property_set("hwservicemanager.disabled", ...)` failed -> `LOG_ALWAYS_FATAL` (service.cpp:153/164). **Every property set from a non-root process failed.** Two layers: (1) the launcher runs from an Android service with **umask 077** and first-stage init (which does `umask(0)`) is skipped, so init created `/dev/__properties__/*` as 0400: non-root clients could not read `ro.property_service.version` (kmsg: `libc: Using old property service protocol`); (2) `/dev/socket` itself was created by the launcher under umask 077 = 0700, so `connect()` to `property_service` failed with EACCES (`libc: Unable to set property ... connection failed: Permission denied`). | `umask 022` at the start of the inner launcher + `chmod 0755 $R/dev/socket`; `umask 0` right before `exec init` | r23: no hwservicemanager abort, vold stops waiting for `hwservicemanager.disabled`, `vdc checkpoint markBootAttempt` exits 0, post-fs + post-fs-data processed |
| logcat "Failed to wait for logd.ready" | Same property bug: logd's `SetProperty("logd.ready")` failed (logd was otherwise running, parked in `pause()`). | Same fix. | r22 proved logd works once `logd.ready` is set (the harness sets it as root as a fallback; logcat then returned the buffers). r23 still to confirm logd sets it itself (the probe could not exec in the container, see below). |
| zygote / zygote_secondary exit 1 | Not a zygote bug: zygote was started **before APEX activation** by hwservicemanager's `onrestart class_restart --only-enabled main`; logcat: `CANNOT LINK EXECUTABLE "/system/bin/app_process64": library "libnativeloader.so" not found`. Also installd exited 1 (no `/data/misc/user/0`, post-fs-data not reached). | Disappears with the hwservicemanager fix (no more early class_restart). | r23: no zygote start before post-fs-data. |
| lmkd SIGABRT | Also property-related (lmkd sets `sys.lmk.*`); aborts stopped together with the above in r23's event list. | — | partly (r23 events show no lmkd abort before the probe) |
| boot stuck in post-fs (r18) | init was blocked in `exec vdc checkpoint markBootAttempt`: vold waits forever for `hwservicemanager.disabled`/ready. | Same fix. | r23 |

Other launcher changes this session (all in `realinit-launch.sh`, VM-tested):
- r19: debug rc (bound over `/system/etc/init/bootstat-debug.rc`) now starts **tombstoned** on `init` (mkdir `/data/tombstones`, `/data/anr`) and
  starts the kmsg logcat stream (`*:W`) only once `logd.ready=true`.
- r20/r21: two oneshot probe services `a6l_propprobe_logd` / `a6l_propprobe_sys` (setprop as uid logd / system, `stdio_to_kmsg`) — this is what exposed the non-root property failure.
- r23: bring-up substitute for `mount_all` (no vendor fstab yet): `on fs` -> `setprop ro.crypto.state unencrypted` + `trigger nonencrypted`
  (needed for `zygote-start` and `class_start main/late_start`). **Must move to the vendor `init.qcom.rc` + a real fstab.**
- Harness probe (ticks 2,4,7,12,20,…): container getprop subset, root setprop round trip, non-root setprop via ctl.start, sockets,
  binderfs, logd/hwsm/vold wchan, `/apex` + linker64 listing, tombstones read with the HOST toybox, logcat (after forcing `logd.ready`).
  Early stop once `A6L_RI_BOOT_COMPLETED` is seen. New report checks: `setprop_client`, `hwsm_alive`, `no_tombstones`.

Relay/tooling gotcha: `device_commit_files` re-committing the same staged path delivered a **stale copy** once (r20 ran the r19
launcher). Commit from a fresh staged filename and have the relay script `grep` for the new marker before launching (done from r21 on).

## Current blocker (r23) — next steps

1. **keystore2 SIGABRT loop** right after `start keystore2` in post-fs-data (~every 2 s). Most likely: no KeyMint HAL in the
   vendor image. `installed-files-vendor.txt` lists keymint/health/allocator/hwc3 HALs, but init in r18–r23 only found
   `boringssl_self_test.rc` and `rild.rc` in `/vendor/etc/init` -> the 21 Sep `vendor.img` does **not** contain the HALs the
   supervisor starts by hand. **Confirmed by r24 tombstones** (`/data/tombstones/tombstone_00..06`): `Abort message: 'Failed to create
   service android.system.keystore2.IKeystoreService/default because of system/security/keystore2/src/service.rs:78: Trying to
   construct mandatory security level TEE'`. Fix = rebuild the vendor image with `android.hardware.security.keymint-service.nonsecure`
   (and the other HALs) as PRODUCT_PACKAGES, then `m vendorimage` (not done: needs the device mk change first).
2. **Container exec breaks after post-fs-data**: from tick 2 of r23, `chroot /proc/<init>/root /system/bin/setprop` fails with ENOENT.
   Likely `/system/bin/linker64 -> /apex/com.android.runtime/bin/linker64` dangling after apexd's full activation (in the
   supervisor flow apexd needed `/data/apex/{active,decompressed,...}`, `/dev/loop-control`, `/dev/device-mapper` and 128
   `/dev/block/loop*`/`dm-*` nodes, see `framework-apex-v51.sh` / `setup_vm_apex_nodes`). With real init these nodes must come
   from ueventd — which runs in the container but gets uevents only for devices that appear later; coldboot relies on
   `/sys` walking, so this should work, but check `A6L_RI_APEX` / `A6L_RI_LINKER` in r24. Also possible: init's critical-service
   handling (`Reboot already performed in last 24hrs because of crash` — init chose not to reboot) — verify nothing unmounted /system.
   **r24 confirms** `ls /proc/<init>/root/apex/com.android.runtime/bin/linker64: No such file or directory` at tick 2 (≈60 s after
   init start, during post-fs-data): the runtime APEX is no longer mounted, so every new exec in the container fails. Next: capture
   apexd's log lines (`apexd:` in kmsg/logcat) around `start apexd` in post-fs-data; likely missing `/data/apex/*` dirs or loop/dm nodes.
3. Then expect: odsign (`wait_for_prop odsign.verification.done 1` gates zygote-start), installd/`/data` layout (init.rc post-fs-data
   should now create it), netd/BPF (`netbpfload`), SurfaceFlinger/HWC/gralloc HALs (VM uses virt DRM + SwiftShader/ANGLE like V64/V72).

**r24 was running** at hand-off (launched 13:47, 12 min budget, same images, richer probe): read
`/home/a6l/kernel/realinit-v80-r24/console.log` (grep `A6L_RI_APEX|A6L_RI_LINKER|A6L_RI_TOMBSTONE|Abort message|A6L_RI_NONROOT`) and
`firmware/extracted/realinit-v80-r24/report.json` when it ends.

## What the phone-side launcher will need (from the supervisor's special cases)

Launcher (`realinit-launch.sh`) — already in it or required:
- guard (a6l-guard / approval file / virt), `androidboot.selinux=permissive` on the cmdline (policy load is kernel-wide);
- `unshare -m -p -f`, rprivate `/`, loop nodes, tmpfs skeleton root with bind-mounted top-level dirs (overlayfs root is rejected), vendor erofs;
- **umask 022 for the launcher, umask 0 for init** (new, mandatory);
- `/dev` tmpfs with `/dev/socket` 0755, devpts, kmsg/null/random/…; fresh `proc` (hidepid=2,gid=3009); filtered `/proc/cmdline`
  (drop `androidboot.init_rc=`); sysfs; selinuxfs; `/mnt` `/debug_ramdisk` `/second_stage_resources` `/metadata` tmpfs; `/data` tmpfs (1.5 GB);
- `mount --move` of the new root onto `/` then `runcon u:r:kernel:s0 chroot . init selinux_setup`;
- GSI-only bind masks: `init.vndk-nodef.rc` (empty). On the phone, keep `/proc/sys/kernel/printk` quiet as v71 does.
- Phone additions the supervisor had and init will NOT do by itself: `/dev/dri/card*` + `renderD128` and `/dev/input/event*`
  come from ueventd **only if** `ueventd.rc` rules exist -> ship `/vendor/etc/ueventd.rc` (drm 0666 system graphics, input 0660 system input,
  kgsl not used, `/dev/binderfs` handled by init.rc); backlight sysfs write permission (`/sys/class/backlight/*/brightness` chown system in init.qcom.rc);
  `a6l_simplefb.ko`/`overlay.ko`/display+GPU modules must be loaded BEFORE the launcher (the recovery does it; init's `/lib/modules` is absent: log shows "Unable to open /lib/modules").

Vendor image (`device/hisense/a6l`, needs `m vendorimage`, not yet done):
- `PRODUCT_PACKAGES`: `android.hardware.graphics.allocator-service.minigbm` (+ mapper.minigbm), `android.hardware.composer.hwc3-service.drm`,
  `android.hardware.health-service.example`, `android.hardware.security.keymint-service.nonsecure` (keystore2 needs a KeyMint),
  `com.android.hardware.audio` (vendor APEX, AIDL example audio HAL), later real QTI HALs. Their `.rc` + VINTF fragments come with the modules.
- `/vendor/etc/init/hw/init.qcom.rc` (imported via `ro.hardware=qcom`): `on fs` -> `mount_all /vendor/etc/fstab.qcom --early/--late` once a
  real fstab exists; until then the r23 `ro.crypto.state unencrypted` + `trigger nonencrypted` stanza; cgroups/blkio writes from init.rc fail
  with EACCES in the VM (harmless); backlight/DRM/input permissions; `setprop ro.vendor.init_dev_config.path` is unset (harmless).
- `/vendor/etc/fstab.qcom`: RAM phase = none (tmpfs `/data` from the launcher); persistent phase = the stock userdata/metadata GPT entries.
- `/vendor/etc/ueventd.rc` as above.
- The supervisor's per-daemon scripts map to stock rc as follows: apexd (`framework-apex-v51.sh`) -> init.rc apexd + ueventd loop/dm nodes;
  BPF (`framework-bpf-v55.sh`) -> `netbpfload.rc` (needs `ulimit -l`, bpf fs: init.rc mounts it); aconfig + system_suspend
  (`framework-native-v52.sh`) -> aconfigd.rc/suspend rc (already started by init in r18–r23); health (`-v54`), media/audio (`-v60`),
  netd (`-v59`, needs iptables + cgroup2), security/keymint (`-v62`), storage/datadirs (`-v58`, `framework-datadirs-v64.sh`) -> vendor HAL
  packages above + init.rc post-fs-data. fd limit 32768 (V66 EMFILE) -> init's default `rlimit nofile` is fine; RLIMIT_NICE 40 -> init.zygote rc.

## Files changed (not committed)
- `device/hisense/a6l/diagnostic/realinit-launch.sh` (umask fixes, /dev/socket mode, debug rc: tombstoned, propprobes, ro.crypto bring-up)
- `tools/Test-RealInitV80.py` (probe, checks, early stop)
- run archives `firmware/extracted/realinit-v80-r19 … r23` (console.log + report.json, written by the harness)

Untested on hardware. Nothing ran on the phone or the laptop.

## Session 2 (agent `realinit2`, 23 Sep afternoon, VM only, phone untouched) — r24 → r28

Status: **still no `sys.boot_completed`, but the boot now reaches system_server** (PackageManager, dex2oat, idmap2d,
SurfaceFlinger + HWC3 + minigbm on simpleDRM all up). Blocker is now the audio HAL: system_server's `AudioService.<init>`
blocks >60 s waiting for audioserver and the Watchdog kills system_server (r27, r28).

### Findings
| Run | Symptom | Cause | Fix |
|---|---|---|---|
| r24 | keystore2 SIGABRT loop ("mandatory security level TEE") | vendor.img (21 Sep) had **none** of the HALs: they were only in the staging dir (built by module name), not in the product, so `m vendorimage` never packaged them; also **no device VINTF manifest** (servicemanager: `Cannot read /vendor/manifest.xml`). | `lineage_gsi_a6l.mk`: inherit `a6l-software-graphics.mk` (minigbm allocator + mapper, hwc3 drm, ANGLE, pastel) + `android.hardware.security.keymint-service.nonsecure`, `android.hardware.health-service.example`, `vndservicemanager`; `BoardConfig.mk`: `DEVICE_MANIFEST_FILE := device/hisense/a6l/manifest.xml` (new, empty device manifest; fragments come with the modules). |
| r24 | "linker64 gone after post-fs-data" | **Not a separate bug.** init.rc post-fs-data does `enter_default_mount_ns` *before* `exec vdc keymaster earlyBootEnded` and only then `restart apexd`. vdc blocked on keystore2 → apexd never ran in the default namespace → `/apex` empty there → every new exec failed. | Disappeared with the keystore fix (r25: apexd full activation, zygote starts). |
| r25 | allocator exits 1 ("Failed to initialize driver"), SF/zygote restart cascade | no DRM device: the harness never loaded `a6l_simplefb.ko` (the V48 supervisor harness and the phone recovery do). | `Test-RealInitV80.py`: ships + `insmod /ri-in/a6l_simplefb.ko` before the launcher (`A6L_RI_SIMPLEFB`). r26: simpledrm card0, allocator + hwc3 + SF alive, system_server started. |
| r26 | audioserver SIGSEGV in `AudioFlinger::onFirstRef` (no audio HAL); hwc "could not create drm fb -22"; logcat stream replayed whole buffer | no audio HAL; supervisor graphics props missing | `PRODUCT_PACKAGES += com.android.hardware.audio`; launcher: cmdline gets `androidboot.vendor.apex.com.android.hardware.audio=com.android.hardware.audio`; debug rc `on early-init` sets the supervisor props (`sys.use_memfd true`, `ro.surface_flinger.default_composition_pixel_format 5`, `debug.renderengine.backend skiaglthreaded`, `ro.sf.lcd_density 400`, `debug.sf.nobootanimation 1`, `service.sf.prime_shader_cache false`, `apexd.config.use_fiemap false`); logcat `-T 1`. r27: drm fb errors gone. |
| r27 | effect HAL exits 1 (`audio_effects_config.xml not found`) → onrestart loop of audio HAL + audioserver → Watchdog kills system_server in AudioService | missing vendor config | `PRODUCT_COPY_FILES += frameworks/av/media/libeffects/data/audio_effects.xml:vendor/etc/audio_effects_config.xml` |
| r28 | effect HAL now stays up (effect .so libs missing, non-fatal); core HAL registers `IConfig/default` but **never `IModule/default`** ("No audio configuration files found"); audioserver waits → AudioService blocks → Watchdog kills system_server at ~323 s | example core HAL has no audio policy/module configuration | **next** (not done): `$(call inherit-product, hardware/interfaces/audio/aidl/default/audio_effects.mk)` (effect libs, as cuttlefish does) + the cuttlefish/AOSP audio policy config (`audio_policy_configuration.xml` & co. under `vendor/etc`), or check what the V61 supervisor had (`framework-media-v60.sh`: A6L_AUDIO_HAL_SERVICE_PASS). Then continue r29. |

Builds: 3 × `m vendorimage` via `tools/build-vendorimage-realinit.sh` (new; copies BoardConfig.mk, lineage_gsi_a6l.mk,
a6l-software-graphics.mk, manifest.xml into the tree; `A6L_TARGETS` overrides the target). Last vendor.img 23 Sep 16:20 (erofs ≈ 44 MB),
system.img unchanged (21 Sep). Run archives: `firmware/extracted/realinit-v80-r25 … r28`.
Known and harmless for now: vendor HAL binaries are labelled `vendor_file` (no device `file_contexts`) so init runs them in
`u:r:init:s0` (works only because SELinux is permissive) → needs `BOARD_VENDOR_SEPOLICY_DIRS` with hal_*_default_exec labels;
device manifest has no `target-level` (hwservicemanager warns); traced/perfetto exit 1 (no vsock); vold: no default fstab.

### Phone-side test (when the VM reaches boot_completed; RAM only, via the V71/V74 diagnostic recovery; needs Pierre)
1. Boot the V71 recovery as usual (`androidboot.selinux=permissive` on its cmdline — the launcher refuses otherwise), `insmod /sdhci-msm.ko` not needed (no eMMC use);
   the recovery loads `a6l_simplefb.ko`/display+GPU modules as today (the launcher does NOT; phone uses the real LCD simpledrm/msm path as in V71).
2. Push `system.erofs` (trimmed, ~1.33 GB), `vendor.erofs` (~44 MB), `overlay.ko`, `realinit-launch.sh` to `/tmp/a6l-ri/` over adb (RAM tmpfs; ~1.4 GB of the 4 GB RAM).
3. Create the approval file `/tmp/a6l-framework-phone-approved` (guard), then `P=/tmp/a6l-ri sh /tmp/a6l-ri/realinit-launch.sh &`.
4. Watch `dmesg | grep -E 'A6L_RI_|init:|E/|F/'`; expected sequence: `A6L_RI_INNER`, `A6L_RI_EXEC_INIT`, selinux_setup, second stage, apexd, keystore2 stays up,
   zygote, SurfaceFlinger on the LCD, system_server, then `A6L_RI_BOOT_COMPLETED`. Probe via `toybox chroot /proc/<container init>/root /system/bin/getprop sys.boot_completed`.
5. Abort = reboot the phone (everything is RAM; nothing is written to eMMC: `/data` is a tmpfs, no fstab, no mount_all).
Phone-specific differences still unverified: the kernel-wide SELinux policy load replaces the recovery's policy (permissive, but host adbd domain changes);
recovery's own adbd/USB gadget must keep running (container init must not touch configfs USB: `/sys` is shared!); the stock rild.rc in vendor is harmless (no modem blobs);
real LCD DRM device name/format differ from the VM simpledrm.

### Remaining before boot_completed (VM)
audio HAL config (above) → then expect: odsign/compos (odsign got SIGKILL, check it sets `odsign.verification.done`), netd/BPF (no BTF; BpfLoader warnings),
Setup wizard/launcher as in V64/V70. Longer term: real vendor sepolicy + file_contexts, `init.qcom.rc` + fstab replacing the debug-rc bring-up stanzas.

Files changed this session (not committed): `device/hisense/a6l/{lineage_gsi_a6l.mk,BoardConfig.mk,manifest.xml(new)}`,
`device/hisense/a6l/diagnostic/realinit-launch.sh` (sha256 4276060c…8de3), `tools/Test-RealInitV80.py` (5d02a85d…bbcb),
`tools/build-vendorimage-realinit.sh` (new). Nothing ran on the phone or the laptop.

## Session 3 (agent `realinit3`, 23 Sep evening, VM only, phone untouched) — r29 → r31

Status: **audio blocker fixed** — the AIDL example core HAL now registers `IModule/default`, `/r_submix` and `/bluetooth`,
audioserver finishes AudioPolicyManager init and AudioService no longer blocks. system_server now completes startup and
reaches its main `Looper.loop()` (SystemServer.java:1100). Still **no `sys.boot_completed`** as of r30: the Watchdog killed
system_server at ~328 s because the main looper spent 66 s in `MediaRouter2ServiceImpl.onPermissionsChanged` (first-boot
permission grants on QEMU TCG = slowness, not a deadlock). r31 (Watchdog multiplier) was launched but its result could not be
read: the Windows bridge disconnected (read `/home/a6l/kernel/realinit-v80-r31/console.log` / `firmware/extracted/realinit-v80-r31/report.json`).

| Run | Symptom | Cause | Fix |
|---|---|---|---|
| r29 | core HAL: `No audio configuration files found`, only `IConfig/default` | no `audio_policy_configuration.xml` anywhere (the V61 supervisor never had one either: it only checked that `IModule/default` came up, `framework-media-v60.sh`) | new `device/hisense/a6l/audio/realinit/audio_policy_configuration.xml` (7.0 format; `primary` module = speaker + built-in mic, 48 kHz 16-bit; includes `audio_policy_volumes.xml` + `default_volume_tables.xml`) copied to `/vendor/etc`; `lineage_gsi_a6l.mk`: `inherit-product hardware/interfaces/audio/aidl/default/audio_effects.mk` (AIDL effect config, libs from the APEX — replaces the frameworks/av `audio_effects.xml` copy whose `/vendor/lib64/soundfx` libs did not exist) + PRODUCT_COPY_FILES for the policy xml and the two volume files. r29: `IModule/default` registered, effect libs load from `/apex/com.android.hardware.audio/lib64/soundfx`. |
| r29 | audioserver loops on `IModule/r_submix` (lazy start fails); Watchdog kills system_server at 453 s in AudioService | the APEX VINTF fragment (`android.hardware.audio.service-aidl.xml`) declares `IModule/default`, `/r_submix`, `/bluetooth`; libaudiohal waits for every declared instance, but the HAL only creates the modules listed in the policy XML (`AudioPolicyConfigXmlConverter::init`; `primary` → `default`; r_submix content ignored, built-in config used) | added `r_submix` and `bluetooth` modules (AOSP r_submix / bluetooth 7.0 content) to the policy XML. r30: all three modules registered, APM initialised (harmless `invalid volume index range` for curves 8–13), audioserver waiting only for `activity` as normal. |
| r30 | Watchdog `Blocked in handler on main thread (main) for 66s` in `MediaRouter2ServiceImpl.onPermissionsChanged` → GOODBYE at 328 s | QEMU TCG (cortex-a53 emulated) is far slower than the phone; Watchdog timeout = 60 s × `ro.hw_timeout_multiplier` (Watchdog.java:880), unset here | launcher debug rc `on early-init`: `setprop ro.hw_timeout_multiplier $WDM`, `WDM=5` only when `/proc/device-tree/compatible` contains `dummy-virt` (phone keeps 1). No build.prop defines it (checked). **r31 result unread.** |

Builds: 2 × `m vendorimage` (`tools/build-vendorimage-realinit.sh`, now also copies `device/hisense/a6l/audio/realinit/*.xml` into the tree).
vendor.img 23 Sep 16:52 (erofs 96 518 144 B, up from ≈44 MB: the effect libs are in the audio APEX). system.img unchanged (21 Sep).
Run archives: `firmware/extracted/realinit-v80-r29`, `-r30` (r30 was stopped early by killing its QEMU after the Watchdog finding; its harness writes the archive on exit), `-r31`.

Next steps:
1. Read r31: if the Watchdog no longer fires, look for `A6L_RI_BOOT_COMPLETED` / `A6L_RI_BOOTPROP=1`; else the next blocker from `W/Watchdog` / `E/`/`F/` lines.
   If MediaRouter2 is still pathological (not just slow), consider `-smp 4` → more vCPUs or `-accel` options, or disabling the media router
   via `config_...` overlay — first check how many `onPermissionsChanged` messages are queued.
2. Expected after that: launcher/SetupWizard (V64/V70 showed LineageSetupWizard), odsign already sets `odsign.verification.done` (r28/r29 OK),
   traced/perfetto vsock errors (harmless), vendor `file_contexts` + sepolicy (HALs run as `u:r:init:s0` under permissive; audio HAL is correctly
   `hal_audio_default` because it comes from the APEX with its own file_contexts).
3. Phone: the audio config is a VM placeholder (no ALSA routing; the example `primary` module falls back to stub streams without a card).
   A real A6L config (sound card `sdm660-...`, "Digital " control prefix, headset/speaker, broken earpiece) is separate work.

Files changed this session (not committed): `device/hisense/a6l/lineage_gsi_a6l.mk` (sha256 f1921c4d…66c1),
`device/hisense/a6l/audio/realinit/audio_policy_configuration.xml` (new, c2183a02…b2b4),
`device/hisense/a6l/diagnostic/realinit-launch.sh` (60afd285…0443), `tools/build-vendorimage-realinit.sh` (53406d07…0bf1).
Nothing ran on the phone or the laptop.
