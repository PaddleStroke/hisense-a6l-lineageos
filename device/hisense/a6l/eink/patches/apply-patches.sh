#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# eink3: apply the e-ink patches to the Lineage tree (idempotent). usage: apply-patches.sh [TREE]
# (TREE default /home/a6l/android/a6l-lineage24). Checks first, applies only if everything applies cleanly.
set -euo pipefail
T=${1:-/home/a6l/android/a6l-lineage24}; D=$(cd "$(dirname "$0")" && pwd)
for p in "$D"/external/drm_hwcomposer/*.patch; do
  cd "$T/external/drm_hwcomposer"
  if git -c safe.directory='*' apply --reverse --check "$p" 2>/dev/null; then echo "already applied: ${p##*/}"; continue; fi
  git -c safe.directory='*' apply --check "$p" && git -c safe.directory='*' apply "$p" && echo "applied: ${p##*/}"
done
grep -q LeaseServer.cpp "$T/external/drm_hwcomposer/Android.bp" && echo EINK_PATCHES_OK
