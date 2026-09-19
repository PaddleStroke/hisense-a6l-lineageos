# Android startup after V37 storage validation

The spare returned to the exact stock Android baseline at 12:05:05.127945 UTC.
V37 remains in recovery. Host services were restored and the final session was
archived. V37 validated 153,268,224 bytes of direct reads in 4,351 ms: all ten
hashes matched the verified backups and USB remained configured through the
66-second heartbeat. This does not establish write, filesystem or Android boot
compatibility.

## Concrete offline findings

The diagnostic kernel had `CONFIG_SECURITY_NETWORK` disabled, making SELinux
unavailable. Pinned Android first-stage init mounts selinuxfs and treats failure
as fatal. The existing system image has a second-stage init and root symlink;
the independently built static first-stage executable is also available in the
Soong intermediates. Neither the original system image nor its root symlink is
a complete A6L boot arrangement.

`tools/build-android-init-kernel.sh` built a separate Linux 7.2.3 output tree at
`/home/a6l/kernel/out-a6l-android-init`, preserving the physically validated
baseline. It enables SECURITY_NETWORK, SECURITY_SELINUX and F2FS_FS_SECURITY
and their Kconfig dependencies. The F2FS setting prepares label support for a
future filesystem choice; current stock userdata is ext4. Source patches did
not change. Kernel, matching storage module, configuration diff and build log
are archived in `firmware/extracted/android-init-kernel-20260917`.

The real Android first-stage init was tested in diskless ARM QEMU with both
kernels. The old kernel reproducibly fails its selinuxfs mount. The new kernel
completes first-stage setup and executes `/system/bin/init`. For this bounded
test that path contains the V37 diagnostic executable as an exec sentinel,
not Android second-stage init. Its START message proves the handoff. Its later
refusal to remount Android's existing sysfs is expected and is not an Android
second-stage result. No policy was loaded and no Android services were started.

Final passing evidence: `firmware/extracted/android-first-stage-20260917-r4`.
Earlier attempts are preserved: r1 encountered a Windows file-timestamp copy
error; r2 exposed two missing ramdisk directories; r3 reached the handoff but
used an overbroad diagnostic-READY acceptance condition. R4 checks the intended
exec milestone and includes the required debug_ramdisk/second_stage_resources
directories. Both the negative and positive cases pass.

The captured Android 9 vendor requires SELinux policy version 28.0. The current
system build contains mapping versions 31 onward and no 28.0 mapping. This is a
separate compatibility problem from the previously identified missing VNDK 28
libraries. Do not simply combine the current GSI and stock vendor and assume
it can start. Source hashes and mapping inventory are in
`research/android-startup-20260917/audit.json`.

## Next integration work

1. Assemble a small Android RAM/recovery environment with actual second-stage
   init, a matching policy and debug access; test it offline before a phone run.
   Preserve the serial logger across first-stage mount setup and handoff, since
   Android mounts a new /dev and the current diagnostic expects to own sysfs.
2. Retain the working A6L DT, including both eMMC supply load permissions, and
   pair the new kernel with its rebuilt module. Recheck USB and storage in the
   same next phone run rather than scheduling separate enumeration tests.
3. Build an explicit A6L first-stage fstab and root layout for fixed partitions.
   Decide the vendor/policy/library compatibility approach before a complete
   Lineage system installation. A current minimal vendor environment can test
   Android core services separately; it would not supply Hisense hardware HALs.
4. Then proceed through core Android services, LCD/touch and hardware services,
   including the e-ink path. The bootloader framebuffer console is not an Android
   display driver, and legacy vendor kernel interfaces remain unresolved.

No new recovery image has been packaged or installed during this offline work.
The next image must pass packaging, captured-bootloader checks and full physical
readback, using the existing laptop installation workflow.

References consulted alongside the pinned source:

- [AOSP generic boot and first-stage layout](https://source.android.com/docs/core/architecture/partitions/generic-boot)
- [Linux ext4 mount semantics](https://cdn.kernel.org/doc/html/latest/admin-guide/ext4.html):
  a future read-only mount test must suppress journal replay (`ro,noload`),
  because `ro` alone can still replay the journal.
