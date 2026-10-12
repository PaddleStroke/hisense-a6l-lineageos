# SPDX-License-Identifier: Apache-2.0
# A6L `user` (release) variant additions (user-build-20261012; firmware/extracted/user-build-20261012/README.md).
# Inherited by rom/rom.mk ONLY when TARGET_BUILD_VARIANT=user. userdebug/eng builds are unchanged by this file.

# --- A user build is always SELinux enforcing (second-stage init is built without ALLOW_PERMISSIVE_SELINUX, whatever the
# boot cmdline says). Without the enforcing-prep policy (rom/sepolicy/vendor + rom/selinux/sepolicy/vendor) and the rc
# transform (no vendor_modprobe seclabels, debug logcat service removed) the A6L daemons have no domain and init refuses
# to start them: modules, radio, charger guard, ... Refuse such a build instead of producing a phone that cannot boot.
ifeq ($(filter 1,$(A6L_SELINUX_PREP) $(A6L_RELEASE)),)
$(error TARGET_BUILD_VARIANT=user needs A6L_SELINUX_PREP=1 (enforcing-prep policy + rc transform), see firmware/extracted/user-build-20261012/README.md)
endif

# --- GPU runtime-PM hold (r6j trial, docs/gpu-investigation-20261002.md). Every userdebug round since r6j runs with
# persist.vendor.a6l.gpu.pm_hold=1, applied by rom/debug/init.a6l.gpu-coredump-debug.rc (su exec + property triggers;
# userdebug/eng only). A user build would silently switch the GPU to runtime PM "auto", a behaviour no round has run
# since 2 Oct. Keep it identical: same default, same two property triggers (they run in vendor_init, which may write
# sysfs_gpu), no su helper, no status line, no coredump reader. Changing the default is a separate, tested decision
# (a user build cannot setprop persist.vendor.* from adb: it needs a rebuild).
PRODUCT_VENDOR_PROPERTIES += persist.vendor.a6l.gpu.pm_hold=1
PRODUCT_COPY_FILES += \
    device/hisense/a6l/rom/user/init.a6l.gpu-pm.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/init.a6l.gpu-pm.rc
