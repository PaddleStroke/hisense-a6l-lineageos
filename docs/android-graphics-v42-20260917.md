# V42: Android graphics services in a private RAM environment

V41 physically passed real minigbm buffer allocation and cross-process import.
V42 tests the service interfaces used by Android's compositor clients, using
the same kernel and guarded simpleDRM framebuffer adapter.

The pinned minigbm source includes an AIDL allocator version 3 and stable C
mapper version 5. This avoids introducing a separate HIDL service manager for
the earlier allocator/mapper 4 candidates. The earlier binaries remain built;
they are not used by this trial.

The new static supervisor creates a private mount namespace and a RAM chroot.
It supplies a new Binder context, minimal VINTF declarations, read-only access
to existing properties and sysfs, and only the device nodes needed for this
test. It provides no block devices or persistent mounts. All services and
their logs live under `/tmp/a6l-v42`. The existing property-service sockets
are bound into the private root so the real service manager can signal
`servicemanager.ready`; its previous value is restored on teardown and checked
by the outer runner. The setting is transient RAM state.

The unmodified platform servicemanager, locally adapted AIDL minigbm allocator, stable mapper
and DRM HWC3 composer run with their built dependency libraries. A small
client tests VINTF discovery, allocation through `GraphicBufferAllocator`,
mapper metadata and lock/unlock with a full-frame pixel check, compositor
client creation, display hotplug and native 1080x2340 configurations. Services
are supervised and terminated; private mounts are removed afterward.

This does not yet run SurfaceFlinger, render an Android app, or prove a
compositor-presented frame. The test should identify service integration gaps
before those are attempted. SwiftShader/ANGLE execution is still unvalidated.

Initial build passed in 4m44s. Diskless QEMU attempt 1 found a Binder-device
name collision: the kernel's binderfs mount already includes `binder`. The
supervisor now creates a distinct `a6l-v42` device and exposes it as
`/dev/binder` inside its private root. Attempts 2 and 3 reached service startup
but waited for readiness. Attempt 4 added a syscall trace and identified
`execve` failing with ENOENT: servicemanager requires
`/system/bin/bootstrap/linker64`, unlike the vendor executables. The packager
now inspects ELF interpreter paths and includes that linker. Attempt 5 passed
real AIDL allocation, mapper 5 metadata and all 2,527,200 pixels, with the
readiness property restored. Composer returned a 1024x768 headless display:
its log explicitly said DRM master access was unavailable. Android minigbm's
primary-node fallback had retained automatic master ownership. The analogous
upstream `minigbm_helpers.c` path already drops accidental master ownership.
The Android allocator path now does the same, scoped to simpledrm, and fails
initialization if that release fails. Patch and original source are archived
under `research/android-graphics-v42-20260917`. Build r5 is in progress.
The temporary servicemanager strace executable is omitted from subsequent
packages, avoiding a tracer descendant during teardown. All failed attempts
are preserved.
Build r5 passed in 2m18s. QEMU attempt 6 passed all declared checks, including
native 1080x2340 display enumeration and readiness/namespace cleanup.
The 74-file, 21,955,541-byte payload is staged and hash-verified on the laptop,
along with six metadata/tool pins and all nine established V38 tools.
Actual frame presentation is still untested: composer logs include failed
empty-state atomic commits during setup/teardown, which must be investigated
before claiming working composition. V42's success criteria cover allocation,
service integration and display enumeration, not presentation.

The physical capture coordinator started at 2026-09-17T15:52:30.846238Z
(PID 222950), after stock identity/port and host cleanup checks. Fastboot
18d1:d00d, serial 1e529013, and the logger were verified. The user has been
asked to select Recovery. The graphics runner has not yet been executed.
The installed V38 recovery is reused; there are no firmware writes.

## Physical result

V42 passed at 2026-09-17T15:54:39.400626Z. The phone verified all 74 payload
hashes, discovered both AIDL services through the private Binder/VINTF setup,
used mapper 5, allocated and checked all 2,527,200 pixels, and enumerated
display 0/config 2 at 1080x2340 with a 16,666,667 ns period. All three services
remained alive, namespace cleanup succeeded, the previous readiness property
was restored, global mounts were unchanged, and authenticated ADB remained
available. Evidence is archived in `captures/capture-graphics-user-v42/graphics`.
Manual return to stock Android was requested; final host cleanup is pending.

Next: a composer-presented client target using the real AIDL executeCommands
path, with explicit active configuration, buffer-slot setup, validation,
accept-changes and present handling. Check every command error and fence;
verify the scanout result before attempting SurfaceFlinger. The pinned
IComposerClient/DisplayCommand/ClientTarget/Buffer definitions were inspected.
No V43 sources have been created yet.

V41's capture coordinator finished with laptop services restored. Stock
Android's return was subsequently verified: serial 1e529013 on USB 3-2,
exact stock fingerprint, boot_completed=1, host pause cleared, ModemManager
active and fwupd inactive. This is recorded after the earlier coordinator
timed out; its original timeout report remains unchanged.

Sources: [upstream DRM composer](https://android.googlesource.com/platform/external/drm_hwcomposer/+/refs/heads/main/hwc3/),
[minigbm](https://android.googlesource.com/platform/external/minigbm/), and the
exact local allocator, mapper, libui, Binder and service-manager sources.
DRM ownership reference: https://docs.kernel.org/next/gpu/drm-uapi.html and
the primary-node `drmDropMaster` implementation in pinned minigbm_helpers.c.
