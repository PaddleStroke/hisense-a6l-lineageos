# V52 native Android startup dependencies — offline VM

V51 r6 passed real APEX activation and runtime checks; SystemServer then waited in StartPowerManager for `android.system.suspend.ISystemSuspend/default`. Full UI is still false.

V52 combines three evidence-backed setup corrections:

1. Genuine `aconfigd-system platform-init` on fresh RAM metadata, using its supported post-fs path when no early-init success marker exists. Require the generated system flag-value storage file, not just a zero exit code.
2. Genuine built `android.system.suspend-service`, with actual Binder registration checked before starting SystemServer. Its source explicitly supports virtual devices whose power sysfs cannot be opened writable: it serves wake locks without suspending the machine. Our VM keeps `/sys` read-only. **This does not validate A6L physical suspend, charging or wakeup.** Like the other staged native services this prototype runs as root in the permissive VM; final init must supply normal service UID, groups and capabilities.
3. Export normal init's `ASEC_MOUNTPOINT=/mnt/asec` and create the RAM directory. V51 native installd segfaulted just after startup. `globals.cpp` directly constructs a string from `getenv(ASEC_MOUNTPOINT_ENV_NAME)` without a null check; the staged environment had omitted that standard variable. Native installd readiness must still be verified from the next logs.

Reproduce: `tools/Test-FrameworkV48.py --rooted --runtime-kernel --apex-service --native-bootstrap --attempt N`. Uses the V51 build-r4 supervisor and V50 kernel, no kernel/source rebuild required. Fresh work path `/home/a6l/kernel/framework-v52-rN` and archive `firmware/extracted/android-framework-v52-20260918-rN`. Adds aconfig and suspend assertions to the previous ten checks. No physical device, host disk, network, SSH or flash access.

Attempt 1 launched. Results pending. This remains a staged service environment; proper init service lifecycle, cgroups, application packages, framework UI and device HALs are separate unfinished work.

Attempt 1: real aconfigd initialized system/system_ext/product but failed at vendor because the minimal diagnostic vendor has no aconfig storage. APEX remained a pass. Attempt 2 generates the four valid empty vendor storage files with the built host aconfig tool, from an explicitly vendor-filtered empty cache. It does not relabel system flags or synthesize live state. The genuine daemon then initializes all container metadata. Independent suspend/runtime steps now continue even if aconfig initialization fails, with that failure retained as a separate assertion.

**Attempt 2 PASS:** all 12 runtime/APEX/native assertions. SystemServer passes StartPowerManager, StartThermalManager, InitPowerManagement, Lights, DisplayManager and boot phase 100 / WaitForDisplay. It then fails PackageManager with `There must be exactly one installer; found []`. This is expected missing application content in the staged payload, not a validated full boot. Native installd no longer SIGSEGVs, but exits because `/data/misc/user` is absent. Adding the standard parent directory is next. V53 includes the built app/priv-app trees and records real installd Binder registration.

Archive `firmware/extracted/android-framework-v52-20260918-r2`, console SHA256 `3370d088636aec19383eff5216b9accee0e02971261fa7c9a06a92198381cd45`. Full UI=false. No hardware actions occurred.
