#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# audio-20261010 ROM integration (11 Oct 2026): apply the AIDL audio HAL series audio/patches/0001..NNNN to
# hardware/interfaces, idempotent for the WHOLE series.
#   usage: apply-hal-patches.sh [TREE] [--check]     (TREE default /home/a6l/android/a6l-lineage24)
# Why not one reverse-check per patch (tools/rom-v2-pipeline.sh, eink apply-patches.sh): 0006 rewrites lines added by
# 0002 and 0007 lines of 0004/0005, so once 0006/0007 are in, "git apply --reverse --check 0002" fails although 0002 is
# applied, and the per-patch loop stops with "audio patch 0002". This script finds which prefix of the series the
# working files hold (HEAD + 0001..n, rebuilt in a scratch dir and compared byte for byte), then applies n+1..last.
# Any other state (foreign edits in these files) is refused, nothing is written then. --check: report only.
set -euo pipefail
T=/home/a6l/android/a6l-lineage24; CHECK=0
for a in "$@"; do case "$a" in --check) CHECK=1 ;; *) T=$a ;; esac; done
D=$(cd "$(dirname "$0")" && pwd); HI=$T/hardware/interfaces; G="git -c safe.directory=*"
[ -d "$HI/.git" ] || [ -f "$HI/.git" ] || { echo "A6L_HAL_PATCHES_FAIL not a git checkout: $HI"; exit 1; }
S=$(mktemp -d); trap 'rm -rf "$S"' EXIT
PATCHES=("$D"/0*.patch); N=${#PATCHES[@]}
for i in "${!PATCHES[@]}"; do tr -d '\r' < "${PATCHES[$i]}" > "$S/$i.patch"; done
FILES=$(sed -n 's#^+++ b/\([^[:space:]]*\).*#\1#p' "$S"/*.patch | sort -u)
# state k = HEAD + patches 1..k (scratch copy, no git repo needed for git apply)
build_state() {
  rm -rf "$S/st"; mkdir -p "$S/st"
  for f in $FILES; do mkdir -p "$S/st/$(dirname "$f")"; $G -C "$HI" show "HEAD:$f" > "$S/st/$f"; done
  for ((j = 0; j < $1; j++)); do (cd "$S/st" && $G apply "$S/$j.patch") || return 1; done
}
same_as_tree() { for f in $FILES; do cmp -s "$S/st/$f" "$HI/$f" || return 1; done; }
have=-1
for ((k = N; k >= 0; k--)); do
  build_state $k || { echo "A6L_HAL_PATCHES_FAIL the series does not apply on HEAD at patch $((k))"; exit 1; }
  if same_as_tree; then have=$k; break; fi
done
if [ $have -lt 0 ]; then
  echo "A6L_HAL_PATCHES_FAIL $HI holds neither HEAD nor HEAD + a prefix of the series in:"; echo "$FILES" | sed 's/^/  /'
  exit 1
fi
echo "hardware/interfaces = HEAD + ${have}/${N} patches of the series"
for ((j = 0; j < N; j++)); do
  [ $j -lt $have ] && echo "already applied: ${PATCHES[$j]##*/}" || echo "to apply:        ${PATCHES[$j]##*/}"
done
if [ $have -lt $N ] && [ $CHECK = 0 ]; then
  build_state $N || exit 1
  for ((j = have; j < N; j++)); do
    (cd "$HI" && $G apply --check "$S/$j.patch" && $G apply "$S/$j.patch") || { echo "A6L_HAL_PATCHES_FAIL ${PATCHES[$j]##*/}"; exit 1; }
    echo "applied: ${PATCHES[$j]##*/}"
  done
  same_as_tree || { echo "A6L_HAL_PATCHES_FAIL result differs from HEAD + the whole series"; exit 1; }
fi
[ $CHECK = 1 ] && [ $have -lt $N ] && { echo "A6L_HAL_PATCHES_PENDING $((N - have))"; exit 0; }
(cd "$HI" && sha256sum audio/aidl/default/primary/StreamPrimary.cpp audio/aidl/default/include/core-impl/StreamPrimary.h \
  audio/aidl/default/ModulePrimary.cpp)
echo A6L_HAL_PATCHES_OK
