# Next integration step: V39 live RAM probes

## Physical result

Passed on 2026-09-17 at 13:31:51 UTC. Both independent probes completed in
about two seconds after staging: Binder manager lookup/ping succeeded;
system and vendor mounted read-only with journal replay disabled; all five
file hashes matched; both mounts were removed. The global mount table was
unchanged. No recovery flash or persistent filesystem writes were performed.

Evidence: `captures/capture-integration-user-v39/integration/report.json`,
`binder.log`, `filesystems.log` and `dmesg.txt`. The preceding V38 storage
read test also passed all ten hashes. Payload r3 passed the diskless QEMU
Binder test and rejected storage access without the expected physical eMMC.

This proves Binder transactions and filesystem access, not the full Android
framework or vendor services. Missing generated linker configuration remains
a warning to resolve in the next Android environment. Normal Android returned, and host cleanup was verified at 13:33:56 UTC.

V38 has already proved Android init, SELinux policy loading (permissive),
authenticated ADB, USB and firmware reads on the physical phone. V39 reuses
that installed recovery and transfers a small tested payload into `/tmp`.
There is no V39 recovery flash.

The combined session tests:

1. Modern Android recovery `servicemanager` and a separate client communicating
   through a private binderfs device. The client resolves the manager service
   and issues a Binder ping. This exercises real Android Binder userspace and
   kernel transactions without replacing the diagnostic's services.
2. Fixed system and vendor ext4 partitions, after matching eMMC identity,
   geometry, partition name and device number. Their block devices are set
   read-only, and mounted with `ro,noload,nosuid,nodev,noexec`. Five files are
   checked against backup hashes. No vendor init scripts or binaries execute.

Both tests run in private mount namespaces. Block read-only flags remain until
reboot; the probe never clears them. Userdata, calibration partitions and boot
control are excluded. The payload has a 35-second alarm; Binder children are
terminated if the parent dies. The ordinary global mount table must be unchanged.

The diskless emulator test uses the same V38 kernel and Android environment. It
must pass actual Binder transactions and reject filesystem access when the
expected eMMC is absent. It is not evidence of physical ext4 mounts.

## Distance to a visible Android UI

Three major pieces remain: a system/vendor boot layout that matches the fixed
partitions, Android core services/framework startup, and a functioning display
stack. Getting a basic launcher is separate from having Wi-Fi, audio, modem,
touch, suspend and e-ink all working. There is no reliable test-count estimate.

Stock graphics library strings reference `/dev/graphics/fb1`, `/dev/epd_flash`
and `/dev/mdss_rotator`. Those vendor interfaces are not supplied merely by
enabling modern DRM. VNDK 28 and SELinux policy 28 compatibility are additional
known gaps in the current generic system build. The raw console framebuffer
does not constitute an Android graphics stack.

For the first UI, investigate the bootloader framebuffer through simpleDRM,
software rendering and upstream DRM hardware composer. This is an unvalidated
bring-up route, not a claim of working graphics or accelerated rendering.
The local source includes DRM composer and minigbm's dumb-buffer implementation.
Evidence is in `research/android-integration-v39-20260917/display-audit.json`.

References checked:

- [AOSP VNDK overview](https://source.android.com/docs/core/architecture/vndk)
- [AOSP DRM composer build definitions](https://android.googlesource.com/platform/external/drm_hwcomposer/+/refs/heads/main/Android.bp)
- [Linux KMS documentation](https://cdn.kernel.org/doc/html/latest/gpu/drm-kms.html)

Status: payload build and offline testing in progress; phone remains stock
Android with V38 recovery. No V39 physical test has started.

Further source check: minigbm has reusable dumb-buffer code but does not currently list a `simpledrm` backend. The built generic system contains no graphics HAL service and no SwiftShader GLES library. Thus the proposed first-UI path needs product configuration and allocator/backend work; it is not available just by adding a framebuffer node.

V39 r3 offline checks passed: real Android Binder manager/client exchange and absent-eMMC refusal. Earlier QEMU runs exposed the client's automatic fallback to RPC when /dev/binder is absent; r3 explicitly uses the private Binder context object. All 19 payload files were hash-verified on the laptop. Capture started at 13:30:13 UTC, reusing installed V38 recovery without flashing. Awaiting physical Recovery selection; integration probes have not run yet.


Final state: normal stock Android and exact baseline verified at 13:33:56 UTC; coordinator finished with Android return and host-service restoration confirmed. V38 recovery remains installed. Final session and post-return verification are archived beside the integration logs.
