A6L enforcing-prep policy (selinux-release, 29 Sep 2026; docs/selinux-release-20260929.md). Added to
BOARD_VENDOR_SEPOLICY_DIRS by rom/selinux/BoardConfig-selinux.mk only with A6L_SELINUX_PREP=1 or A6L_RELEASE=1, together
with rom/sepolicy/vendor. Rules proposed by tools/release/a6l-avc-plan.py from the first permissive install's avc log go
here (or in the per-area dir that owns the domain), after review. Check: bash tools/release/check-a6l-sepolicy.sh user
rom/sepolicy/vendor rom/selinux/sepolicy/vendor
