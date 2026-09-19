# Boot and kernel investigation: independent of the system build

The completed Lineage system image is needed for final packaging checks and a
full Android boot test. It is not a prerequisite for finding kernel source,
auditing required kernel features, compiling a candidate kernel, or mapping the
phone's existing boot flow. The current compile probe builds no kernel or boot
image.

Latest progress: kernel-prototype-20260914.md records the built newer kernel,
minimal A6L board definition, successful diagnostic RAM boot in QEMU, and the
remaining bootloader image/DTBO integration work. Earlier findings below are
retained as the investigation chronology.

## Measurements on 2026-09-14

Read the boot partition directly from the authoritative firmware prefix using
the offset and length in firmware-verification.json. Its SHA-256 matches the
recorded partition hash and the earlier boot.bin slice:
55ad4747ea8d83a32edb09eafb01772a3cb170378331cdea3d45cd5a8cb34dbc.
Recovery and vbmeta slices also match the authoritative prefix and recorded
hashes. No phone writes were performed.

- Boot header: version 1, 1,648-byte header, 4,096-byte page alignment.
- Kernel: 12,378,977 bytes, including the previously decoded appended trees.
- Ramdisk: 1,024 bytes at boot-image offset 12,386,304; uncompressed newc CPIO.
- No second-stage payload in this boot image.
- Ramdisk entries: `.`, `.backup`, `.backup/.magisk`, `TRAILER!!!`. No executable
  init was found in this ramdisk. The 59-byte metadata file records
  KEEPVERITY=false, KEEPFORCEENCRYPT=false, RECOVERYMODE=false. These are metadata
  values, not measurements of runtime encryption or proof that Magisk is active.
- Captured mounts show /dev/root mounted as / and the separate vendor partition
  mounted as /vendor. Both decoded appended device trees specify the vendor
  block path and ext4 mount under /firmware/android/fstab/vendor.
- The trees list vbmeta, boot, system, vendor, dtbo and recovery as AVB parts.
  Actual bootloader-added arguments and active overlay selection remain to be
  established; reading /proc/cmdline was denied on the running phone.

Inspected vbmeta using the synced AOSP avbtool info_image command. SHA-256:
e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350.
It contains flags=2, rollback index 0 and SHA256_RSA4096 algorithm metadata.
The synced external/avb/libavb/avb_vbmeta_image.h defines bit 1 as
AVB_VBMETA_IMAGE_FLAGS_VERIFICATION_DISABLED. This inspection does not verify
the signature against a trusted Hisense release key or prove the bootloader's
runtime behavior. Do not infer either from the algorithm field alone.

This finding qualifies earlier references to "stock": the backup is a verified
copy of this phone's captured, successfully booting firmware state, not an
authenticated pristine Hisense release. The phone reported locked/green Android
properties, but those properties alone cannot resolve the metadata discrepancy.
The origin of the Magisk metadata and AVB flag remains unknown. Neither was
introduced by the read-only investigation. Preserve the captured files intact.

## Implementation sequence

Progress: the offline container roundtrip now succeeds for both boot and
recovery after explicitly preserving the unused second_addr header field.
Kernel symbol recovery and the initial compatibility matrix are complete; see
kernel-investigation-20260914.md. No candidate replacement kernel is built yet.

1. Reproduce the measured boot container and payload layout offline. Confirm
   unchanged payload hashes and exact differences before substituting anything.
   Signature bytes cannot simply be recreated without the corresponding key.
2. Trace the existing root mount and init flow into the downloaded modern init
   implementation, fstab handling and AVB configuration. Decide explicitly how
   early userspace will run and mount the existing fixed partitions. Do not
   assume a current phone's vendor_boot/init_boot layout applies to the A6L.
3. Build a feature matrix from the current userspace and the recovered kernel
   configuration: BPF/networking, Binder, memory/buffer interfaces, filesystems,
   encryption, SELinux, and vendor kernel module/IOCTL dependencies. The BPF
   failure in kernel-compatibility.md is established; it is not the whole matrix.
4. Seek matching Hisense source and identify the Qualcomm base and device
   changes. Qualify any candidate against the A6L's controllers, GPIOs, device
   trees and vendor interfaces. A kernel from another SDM660 phone is reference
   material until that support is demonstrated.
5. Prefer obtaining a reproducible kernel baseline for this hardware, then
   evaluate carrying the device support to a suitable newer kernel against
   verified feature backports. No kernel branch has yet passed this assessment.
   Extracted configuration, trees and binaries do not recover original C source.
   Without matching source, missing driver reconstruction becomes a separate
   reverse-engineering project; binary drivers are not assumed portable.
6. Compile kernel and boot components independently of the full Android build.
   A minimal diagnostic userspace can help isolate early boot from the full OS.
   Whether the bootloader supports temporary RAM boot is unverified. Establish
   the installation/recovery procedure and precise images before device tests.
7. Progress from kernel/init/debugging access to modern Android services, then
   front display/touch and the Hisense e-ink control path. Use the finished system
   image and incremental rebuilds when those integration tests become possible.

## References

- https://source.android.com/docs/core/architecture/bootloader/boot-image-header
- https://source.android.com/docs/core/architecture/partitions/system-as-root
- Synced source: system/core/init/first_stage_init.cpp and first_stage_mount.cpp.
- Synced source: external/avb/libavb/avb_vbmeta_image.h.
