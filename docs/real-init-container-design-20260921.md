# Real Android init from RAM — design (21 Sep 2026, offline, untested)

Goal of roadmap step 2: drop the hand-written supervisor (`framework_root_services.c`, ~40 start-up special cases) and let
Android's own `init` process the real rc files, property service, ueventd, apexd, vold…

Constraint (Pierre, 21 Sep): stay RAM-only on the phone. So PID 1 remains the diagnostic recovery's init and the OS image
still arrives over ADB. Solution: **run init as PID 1 of a private PID + mount namespace** (`unshare -m -p -f`), after a
launcher has done first-stage init's job. See `device/hisense/a6l/diagnostic/realinit-launch.sh`.

Inputs: `system.erofs` (system-as-root, SELinux-labeled, built by the Android build: `BOARD_SYSTEMIMAGE_FILE_SYSTEM_TYPE := erofs`)
and `vendor.erofs` (`PRODUCT_BUILD_VENDOR_IMAGE := true`; needed for the split sepolicy, VINTF, our HALs, `fstab.qcom`,
`init.qcom.rc`). Both are exactly the images a later persistent install will flash; only the launcher goes away then.

Known risks to resolve in the VM first:
1. `init selinux_setup` replaces the kernel-wide policy; requires `androidboot.selinux=permissive` (present on the recovery cmdline).
2. init inside `chroot` instead of a pivoted root (init remounts `/` shared; may need `pivot_root`).
3. ueventd coldboot + firmware loading from a namespace (uevents are per network namespace: we keep the host's).
4. `reboot`/`shutdown` from the container must not power-cycle the phone unexpectedly: init calls `reboot(2)`, which in a
   non-init PID namespace only kills the namespace (kernel behaviour) — good, to be verified.
5. Everything the supervisor special-cased (data dirs, fd limits, BPF, netd sockets, keystore/gatekeeper, audio policy) must
   come from proper vendor rc/fstab/HAL packaging in `device/hisense/a6l` instead.
VM harness: `tools/Test-RealInitV80.py` (to write once the images exist): initramfs = recovery-like environment + images, boots
the phone kernel in QEMU, runs the launcher, checks `sys.boot_completed` via the kernel log / `getprop` through `nsenter`.
