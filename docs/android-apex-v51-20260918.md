# V51 — real APEX activation in the offline VM

Predecessor: V50 passes ART/JNI/SharedMemory/Binder, and genuine Zygote enters SystemServer. PlatformCompat then waits for the missing `apexservice`. V51 addresses that dependency with the built Android daemon and signed packages, rather than returning a fabricated package list.

Reference: [AOSP APEX format and activation](https://source.android.com/docs/core/ota/apex). Local authority for this build is `system/apex/apexd`: main, lifecycle, OnStart, OnAllPackagesActivated, snapshotde and loop setup. The V50 kernel already includes loopback, device mapper/verity and EROFS. For RAM-backed packages `apexd.config.use_fiemap=false` selects the supported non-pinned path.

## Scope

- QEMU `virt`, no network, no attached disks or host devices; same guarded VM supervisor.
- Original signed APEX payload from each CAPEX, or original APEX file, packaged in initramfs. Expanded directories are available only to bootstrap the initial tools.
- Remove the old generated APEX inventory before launching the actual daemon. Require newly emitted inventory, read-only mount for every expected package and real Binder registration.
- Run the genuine snapshotde phase and regenerate linkerconfig before classpaths/ART/SystemServer.
- Private property server accepts APEX status writes; no `ctl.*` action emulation, no manually invented ready state.
- Kernel messages are read through an independent `/dev/kmsg` reader; no dmesg clearing.
- Preserve the test process-group ID after the probe shell exits, so native daemon descendants are killed during cleanup.

This remains a staged runtime environment, not normal Android init/service boot and not a flashable image. Native services, init triggers, aconfig state and application packages still need integration. Full UI remains a separate result.

## Reproduction and evidence

`tools/Test-FrameworkV48.py --rooted --runtime-kernel --apex-service --attempt N`

Fresh attempt paths are required: `/home/a6l/kernel/framework-v51-rN` and `firmware/extracted/android-framework-v51-20260918-rN`. Manifest, exact harness/script/supervisor sources, console and JSON assertions are archived. Do not edit test inputs while an attempt is active.

Build r1 added APEX property support; r2 also retains the daemon process group for cleanup. Attempt 1 launched with r2. Its reused V40 device tree advertised only 2 GiB despite the command allocating 3 GiB. The new 1.70 GB payload plus 805 MB compressed initramfs did not reach the console; the identified QEMU process was stopped and the failure archived. Attempt 2 corrects both DT memory and QEMU allocation to 4 GiB and enables earlycon. No phone or laptop access.

Attempts 2–4 did not reach the runtime supervisor. Attempt 3's verbose log identifies `rootfs image is not initramfs (write error)` followed by incomplete write `-28` (ENOSPC). Entire recipe content is 1,721,246,985 bytes; the root tmpfs default is half `totalram_pages()` at creation, while the compressed initrd is still reserved. Attempt 4 confirmed the early-init action, but its script was not unpacked completely. Attempt 5 uses 6 GiB for this duplicated expanded-plus-packaged fixture. This is not a phone memory requirement. Also include `toolbox`, the real target of the getprop symlink. Init/shell markers and stdio-to-kmsg remain enabled for diagnostics.

Attempt 5: all 38 expected APEX packages mounted read-only and appeared active in apexd's newly generated inventory. `service check apexservice` returned found. Snapshotde then failed because `/data/misc_de` was missing, so runtime tests correctly did not proceed. Added real empty per-user misc_de/misc_ce directories. The daemon did not need a fabricated vold service to activate factory packages, but vold remains a normal-boot dependency.

Build r3 failed a misleading-indentation compiler check; bracing fixed and build r4 passed. Build r4 creates the VM's loop/mapper nodes natively, eliminating 256 emulated shell launches. Toybox timeout source review showed that default timeout children create new process groups: V51 now uses `--foreground` so the supervisor's group cleanup includes native descendants, with kill-after timeouts retained. Attempt 6 uses these changes. The historical V49 generator now refuses to overwrite existing supervisor files, protecting subsequent validated fixes.

**Attempt 6 PASS:** all ten assertions (the original eight runtime checks plus real APEX mounts and service readiness). All 38 expected packages mounted read-only, snapshotde completed, actual apexd status reached ready. ART/JNI/shared memory/Binder remain good. SystemServer enters, completes PlatformCompat, starts FileIntegrity, Installer wrapper, FeatureFlags, UriGrants, PowerStats, AccessChecking, ActivityManager and DataLoaderManager. It then waits for missing `android.system.suspend.ISystemSuspend/default` in StartPowerManager, until the bounded test terminates it (137). Full UI remains false. The installer wrapper starting does not mean native installd is healthy; its separate failure remains in the logs.

Archive: `firmware/extracted/android-framework-v51-20260918-r6`; console SHA256 `726e9fa962a0203f88484faf0e1512ff0dbe9b7cc19c79a6578de90d40dbdc9d`. No jobs remain from V51 r1–r6. Phone unchanged. Next offline dependency: real SystemSuspend service and normal native-service/init preparation.
