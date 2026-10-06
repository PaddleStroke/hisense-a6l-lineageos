A6L ROM-level vendor sepolicy (release-prep, 27 Sep 2026; docs/release-prep-20260927.md section 3).
Covers everything the per-area dirs (radio, audio, kvoice, gnss, eink, dualux) do NOT: the rom/ scripts and init services
(a6l-modules.sh, a6l-radio.sh + rmtfs/tqftpserv/diag-router, a6l_macs, charger guard), unlabeled HAL binaries (KeyMint
nonsecure, minigbm allocator, QTI vibrator), the eMMC partition labels vold/init/uncrypt/recovery need in enforcing mode,
the a6l property namespaces and the sysfs nodes the HALs touch. Supersedes device/hisense/a6l/hals/sepolicy (never
compiled; its a6l_radio.te/a6l_macs.te content is folded in here with the real ROM paths) - do NOT add both dirs.
Enabled by BoardConfig-rom.mk only when A6L_SEPOLICY_ROM=1 (default 0 = r3 behaviour); the rc seclabel switch is
rom/sepolicy/rc-enforcing.patch (NOT applied). Offline check: bash tools/release/check-a6l-sepolicy.sh user rom/sepolicy/vendor
Every genfs path marked UNVERIFIED must be confirmed on the phone with `ls -lZ` (first install, permissive) before enforcing.
