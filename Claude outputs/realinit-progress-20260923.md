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
