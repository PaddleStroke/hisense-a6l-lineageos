# Hisense A6L bring-up

Target branch: LineageOS 24.0. **System compile probe built and filesystem-checked;
not boot-tested or ready to flash.**

`lineage_gsi_a6l` is an initial system-image compile probe using the upstream
LineageOS arm64 GSI product. It gives us a current framework baseline and a place
to add compatibility work. It is not full native A6L hardware support. Kernel,
boot, recovery, vendor, userdata and OTA outputs are disabled for this probe.
The system image size is constrained by the measured stock partition capacity.

## Known blockers

- Stock vendor is API 28 / VNDK 28. LineageOS 24's GSI product presently includes
  additional VNDK snapshots 31–34, not 28. An unmodified probe is not expected to
  satisfy this vendor. Preserve and audit the stock VNDK/LLNDK dependencies before
  selecting a legacy compatibility implementation or replacing vendor components.
- Kernel 4.4.153 has Hisense-specific e-ink changes. No matching A6L kernel source
  has been located in the searches recorded so far. New Android kernel/runtime
  requirements need auditing, not just a renamed device tree.
- The old `BOARD_BUILD_SYSTEM_ROOT_IMAGE` switch is obsolete in current build
  rules. The stock system-as-root boot flow must be reconciled explicitly.
- E-ink support crosses framework, SurfaceFlinger/libgui, HWC, a software TCON
  library and kernel/sysfs. Merely enabling fb1 or copying the old SurfaceFlinger
  binary is not an implementation for Android 17.
- Bootloader remains locked. There is a verified firmware backup, but no tested
  restore procedure or installation package. Nothing here authorizes automatic
  flashing, partition repartitioning, or rollback-index changes.

## First build after Linux setup

Place this tree at `device/hisense/a6l` inside a LineageOS 24 source checkout.
Run `tools/build-linux-probe.sh` from the desktop workspace inside WSL. It sources
the branch's aosp_target_release and uses the three-argument lunch interface,
then builds `systemimage`. Record all configuration failures.
Do not treat successful compilation as evidence it can boot the stock phone.

Stock binaries remain private under the workspace's ignored firmware directory.
Public device files contain configuration and independently recorded facts only.
