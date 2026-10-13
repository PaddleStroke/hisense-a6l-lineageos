#!/usr/bin/env bash
# Build AOSP's existing overlay test app locally, without the Android build graph.
set -euo pipefail
source_dir="$HOME/android/a6l-lineage24"
ufdt="$source_dir/system/libufdt"
fdt="$source_dir/external/dtc/libfdt"
output="$HOME/tools/a6l-overlay-validator"
mkdir -p "$output"
gcc -O2 -Wall -I"$ufdt/include" -I"$ufdt/sysdeps/include" -I"$ufdt/tests/src" -I"$fdt" \
    "$ufdt/tests/src/ufdt_overlay_test_app.c" "$ufdt/tests/src/util.c" \
    "$ufdt/ufdt_overlay.c" "$ufdt/ufdt_convert.c" "$ufdt/ufdt_node.c" \
    "$ufdt/ufdt_node_pool.c" "$ufdt/ufdt_prop_dict.c" \
    "$ufdt/sysdeps/libufdt_sysdeps_posix.c" "$fdt/"*.c \
    -o "$output/ufdt_apply_overlay"
git -C "$ufdt" rev-parse HEAD > "$output/libufdt-revision.txt"
git -C "$source_dir/external/dtc" rev-parse HEAD > "$output/libfdt-revision.txt"
echo A6L_HOST_OVERLAY_VALIDATOR_BUILD_SUCCESS
