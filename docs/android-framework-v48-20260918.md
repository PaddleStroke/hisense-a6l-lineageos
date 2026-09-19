# V48 offline framework runtime preparation

The phone remains on stock Android with the validated V46 recovery. V47 proved
native Android input and rendering. The next independent work is ART, framework
JNI and SystemServer startup; Claude's hardware investigations are separate.

The existing product is a Lineage 24 GSI compile probe (Android API 37), not a
complete A6L device product. Its system image predates the recent graphics/input
diagnostics. Do not flash that image as though the tested HALs were integrated.

## Offline test scope

`tools/Test-FrameworkV48.py` packages the existing compiled framework, 64-bit
libraries, boot images and product-selected expanded APEX directories into a
diskless QEMU initramfs. A new private mount/Binder/property supervisor starts
the validated software graphics services and runs, in sequence:

1. The actual SDK Extensions `derive_classpath` against built classpath fragments.
2. ART Java execution and a core-library SHA-256 operation.
3. `app_process64`, AndroidRuntime JNI initialization, SystemClock/Process and a
   native Parcel roundtrip.
4. If those succeed, a bounded SystemServer startup to collect its first missing
   service or environment requirement. Early V48 used direct invocation; V49
   uses the genuine Zygote fork path described below.

This is not yet a complete Android boot. Expanded APEX directories do not validate
signed APEX activation by apexd. V49 exercises Zygote but does not establish a
working system-service graph or launcher. The report keeps
`system_ui_passed` false. The new supervisor rejects a non-QEMU device model.
There is no phone runner, deployment, firmware write or physical storage mapping.

Each attempt preserves its own manifest, output and source snapshot. Existing
V47 artifacts remain unchanged. Framework edits and any future phone deployment
will follow concrete failures observed here rather than assuming the generic
system is bootable on the A6L.

Source references: the pinned `AndroidRuntime.cpp`, `app_main.cpp`,
`system/core/rootdir/init.rc`, and SDK Extensions' `derive_classpath` implementation.
The official [APEX format documentation](https://source.android.com/docs/core/ota/apex)
and [linker configuration project](https://android.googlesource.com/platform/system/linkerconfig/)
explain why copying framework JARs alone is insufficient: module activation,
classpath order and native-library namespaces are runtime dependencies.

Supervisor build r1 passed. Initial emulator results identified three setup
requirements before framework startup:

- Attempt 1 derived classpaths but ART required the generated system namespace.
- Attempt 2 stopped during packaging because loose APEX directories omit their
  manifests; manifests must come from the real packages.
- Attempt 3 generated namespaces, then found missing APEX-private libc++.
  The harness now extracts complete product-selected APEX payloads using the
  build's deapexer, keyed by package hash. Loose product/apex directories are
  insufficient runtime images.
- Attempt 4 reached ART but encountered duplicate bootstrap Bionic loading.
  The small V47 payload carried a recovery linker; the full-runtime test now
  uses the normal /system/bin/linker64 alias into com.android.runtime.

Attempt 5 still mixed bootstrap library paths with generated namespaces. Attempt
6 removes the inherited LD_LIBRARY_PATH after namespace generation and PASSED
the runtime foundation: Java execution, SHA-256 provider, app_process native
framework initialization, SystemClock/Process, native Parcel roundtrip and
supervisor cleanup. Real SystemServer.main was then attempted; it failed when
the private property service rejected sys.system_server.start_count. This is
not SystemServer startup success.

The saved log also reports boot-image read-barrier incompatibility with an
imageless fallback, missing aconfig state, and composer commit errors in this
broader environment. None is hidden by the runtime-only success criterion;
V47's physical graphics result is separate. Framework launch/display readiness
will require resolving the relevant remaining failures.

Build r2 extends only the private RAM property service for SystemServer
bookkeeping; it has no persistent-property backing or power/init action handlers.
The next attempt also includes built font and system_ext resources. The phone
remains untouched and there is still no V48 deployment script.

Build r2 and attempt 7 passed the runtime foundation again. SystemServer entered
its initialization, then aborted in libmemevents' static initializer. Symbolized
return address 0x50cc resolves to `bpf::get_api_level_full`, which requires the
quarterly API metadata at /system/etc/init/netbpfload.rc. The harness had omitted
init scripts; this specific file is now copied as metadata without executing it.

Direct root invocation also emitted SystemConfig's identity warning. Build r3
adds the actual Zygote command with inherited zygote/usap sockets and its required
nice limit, allowing the real fork path to establish SystemServer identity.
This remains QEMU-only. The private property socket uses Android's intended
0666 mode, while its accepted writes remain confined to private RAM properties.

The built GSI embeds system_ext and product below system/, rather than in sibling
output directories. Runtime resources must account for that layout. The harness
now uses the correct embedded locations and binds its RAM initramfs payload
instead of duplicating it. Its RAM-filesystem guard remains enforced.

Kernel audit: CONFIG_USERFAULTFD is disabled. ART's `ShouldUseUserfaultfd` in
art/runtime/gc/collector/mark_compact.cc requires kernel support as well as the
runtime configuration. The current image reports no-read-barrier compilation,
while this runtime chooses a read-barrier collector and falls back to imageless
execution. This is a framework-kernel configuration gap; no physical kernel has
been changed to address it yet.

Build r3 passed. Attempt 8 reached Zygote but its rootfs mount-isolation operation
failed because the chroot root was not a mountpoint. Attempt 9 fixes that and
reaches the SystemServer fork, where descriptor validation rejects an inherited
JAR path exposed as /tmp/a6l-v48/root/apex/... . No descriptor allowlist or Zygote
check was weakened. Both runs retain passing ART and JNI probes.

## V49 normal-root emulator follow-up

The next test uses `Test-FrameworkV48.py --rooted --attempt N`, archived with V49
names. A separate `a6l_framework_root_services` binary retains the QEMU-model
guard. The VM mounts the RAM payload's system/APEX/vendor/product/system_ext at
their normal root paths, then creates the isolated device/Binder/property
environment there. No chroot prefix is introduced into Zygote's inherited FDs.

The QEMU command still has no network or disk device, and its initial init.rc
starts only this test. Old /dev property metadata is retained read-only while
the private property areas are active. Existing phone diagnostics and firmware
are unchanged. This is an offline test harness, not a phone deployment plan.

V49 attempt 1 passed the ART/JNI probes and reached Zygote's SystemServer fork.
Normal root paths fixed the rejected JAR descriptor from V48. The next rejected
descriptor was FD 80 pointing to `/logs/framework.log`, inherited through the
diagnostic shell. The launcher now redirects standard descriptors to `/dev/null`
and closes inherited descriptors before creating Android's Zygote sockets,
matching init-style descriptor hygiene. Android's descriptor checks are intact.
The corrected native target built successfully; V49 attempt 2 tests this change.

V49 attempt 2 passed the runtime foundation and Zygote forked a SystemServer
child. The child failed in `set_sched_policy`: `/etc` still contained the small
recovery fixture rather than the system configuration, so Android could not load
its task profiles. Separately, binding `/sys` onto itself hid the original
SELinux submount and caused servicemanager's status check to abort.

Attempt 3 addresses these environment errors together: bind `/system/etc` at
`/etc`, retain the original SELinux filesystem through a separate alias, and
mount real CPU/blkio v1 and memory v2 cgroups inside the disposable VM. Scheduling
profiles remain the built Android profiles. Cpuset v1 is disabled in this kernel
and remains an explicit integration gap. Production setup must use Android init
and cgroups/task-profile configuration, not this QEMU supervisor's bootstrap.
Reference: https://source.android.com/docs/core/perf/cgroups .

V49 attempt 3 reached `Entered the Android system server!` through the genuine
Zygote fork, with ART/JNI and cleanup still passing. SystemServer then stopped at
`ApplicationSharedMemory.nativeCreate`: libcutils selected ashmem, absent from
this mainline kernel. Attempt 4 sets the supported `sys.use_memfd=true` runtime
property; it does not change or bypass the shared-memory API.

Attempt 3 also exposed a further SELinux mount detail: libselinux's
`verify_selinuxmnt` explicitly rejects read-only mounts. The retained alias is
now a normal bind in this disposable VM. No SELinux checks were patched out.
Binder devices now have Android's normal 0666 access mode for future app UIDs.
SELinux remains permissive as in the earlier diagnostic, not production-ready.

Attempt 4 passed the runtime foundation and no longer hit the ashmem exception.
It reached SystemServer but stalled waiting for service-manager readiness. The
log identified an unset `ro.property_service.version`: newly launched clients
selected protocol 1, while the isolated property server implements protocol 2.
Attempt 5 explicitly advertises version 2, adds real SharedMemory create/map/
protect and Java-to-native Binder checks, and starts the built installd in RAM
before Zygote. This expands the success criteria rather than treating a timeout
or an entered-SystemServer marker as a successful boot.

## V49 result and next boot environment

Attempt 5 **passed all eight runtime checks**, including real SharedMemory
create/map/protect, Java-to-native Binder lookup, genuine Zygote/SystemServer
entry and cleanup. Both service managers publish readiness. SystemServer starts
its watchdog and ProtoLog services, then waits in PlatformCompat for
`apexservice`. The bounded launch times out with 124; this is not a full boot.
Its logs, manifest, source snapshots and report are archived under
`firmware/extracted/android-framework-v49-20260918-r5`.

The next environment needs the actual APEX daemon and activation path, plus
Android init's native-service lifecycle. Do not fake an empty APEX list or loosen
`ctl.*` permissions to get past this dependency. The current property helper has
no service-launch implementation, so accepting the request would not start it.

Preparation checklist:

- Preserve the physically verified V46 image and the V47 input/graphics bundle.
- Build a separate kernel from exactly the validated source patch. V50 adds
  USERFAULTFD, CPUSETS_V1, built-in EROFS/verity, and VirtIO MMIO/block for offline
  disk-image tests. It is not installed on the phone.
- Rebuild the small framebuffer module against that kernel's actual symbol table.
- Re-run the existing runtime checks with `--rooted --runtime-kernel`, requesting
  ART's supported UFFD collector. Check the previous read-barrier mismatch is gone.
- Assemble a fresh runtime image from current output, including the tested
  graphics integration. The September 14 system.img predates those changes.
- Use actual APEX packages and their signature/activation flow. Expanded cache
  directories only proved runtime linkage. Account for EROFS/loop/verity and
  writable RAM metadata/data; pinned-image FIEMAP is not available on tmpfs.
- Bring up normal cgroups, property/init service control, aconfig initialization,
  apexd, installd and other required native services. Supply normal data directory
  ownership and ANR/tombstone logging before attempting the full package scan.
- Include system applications only when that service graph is available; verify
  ActivityManager, WindowManager, SystemUI and the launcher explicitly.

The V49 supervisor is still QEMU-only. No new physical test or flashing package
has been produced from these framework experiments.

## V50 runtime-kernel result

The separate kernel and all configured modules built successfully. The external
framebuffer module was rebuilt after generating the complete kernel symbol table;
no unresolved-symbol checks were suppressed. Kernel source still matches the
validated V38 source patch exactly. Only the archived configuration diff changed.
Image SHA-256: `a8f5fc503627762e9ffc939c0a70f93b1acfbd6c83f6cf83e45bd42285278341`.

`Test-FrameworkV48.py --rooted --runtime-kernel --attempt 1` passed all eight
runtime checks and cleanup. ART, app_process, Zygote and SystemServer log
`Using generational CollectorTypeCMC GC.` The prior read-barrier mismatch is
absent. The rebuilt display module loads and the Java/Binder/shared-memory
checks remain successful. Archive:
`firmware/extracted/android-framework-v50-20260918-r1`.

SystemServer again reaches PlatformCompat and waits for the missing real
`apexservice`; the bounded run ends without a UI. All build and QEMU processes
have finished. V50 has not been deployed to the spare, and hardware compatibility
of this new configuration still needs a later physical regression check.
