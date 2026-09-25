#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copies the two e-ink blobs the ROM needs into device/hisense/a6l/eink/proprietary/ (git-ignored). Read-only on the
# sources; run from anywhere. usage: extract-eink-blobs.sh [REPO]   (REPO default: /mnt/c/Users/Pierre/Desktop/A6L)
set -euo pipefail
R=${1:-/mnt/c/Users/Pierre/Desktop/A6L}
D=$(cd "$(dirname "$0")" && pwd)/proprietary
mkdir -p "$D/lib64" "$D/etc"
install -m 0644 "$R/firmware/extracted/vendor/lib64/libtcon_eink.so" "$D/lib64/libtcon_eink.so"
head -c $((0x70080)) "$R/firmware/extracted/eink-spi-nor-20260920/epd-nor.bin" > "$D/etc/epd-nor.bin"
[ "$(stat -c %s "$D/etc/epd-nor.bin")" = $((0x70080)) ] || { echo "EINK_BLOBS_FAIL waveform size"; exit 1; }
sha256sum "$D/lib64/libtcon_eink.so" "$D/etc/epd-nor.bin"
echo EINK_BLOBS_OK
