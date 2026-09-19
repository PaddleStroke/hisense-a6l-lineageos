#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run inside Ubuntu after Windows has restarted. No phone access or flashing.
set -euo pipefail

workspace=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
source_dir=${A6L_SOURCE_DIR:-"$HOME/android/a6l-lineage24"}
if [[ $(uname -s) != Linux ]]; then
    echo 'Run this script inside Linux/WSL.' >&2
    exit 1
fi
if [[ $(id -u) == 0 ]]; then
    echo 'Use a normal Linux build account, with sudo available.' >&2
    exit 1
fi
case "$source_dir" in
    /mnt/*) echo 'Source/build output must live on the Linux filesystem, not /mnt/c.' >&2; exit 1 ;;
esac

if [[ ${1:-} != --skip-deps ]]; then
sudo apt-get update
sudo apt-get install -y git git-lfs repo gnupg flex bison build-essential zip \
    curl zlib1g-dev libc6-dev-i386 x11proto-core-dev libx11-dev lib32z1-dev \
    libgl1-mesa-dev libxml2-utils xsltproc unzip fontconfig python3 \
    python3-venv rsync bc cpio device-tree-compiler lz4 lzop libssl-dev \
    libelf-dev e2fsprogs
fi

mkdir -p "$source_dir"
cd "$source_dir"
if [[ ! -d .repo ]]; then
    repo init -u https://github.com/LineageOS/android.git -b lineage-24.0 \
        --depth=1 --git-lfs
fi
repo sync -c --no-clone-bundle --no-tags -j8

# Copy our small device configuration; stock firmware is not added to source.
mkdir -p device/hisense/a6l
rsync -a "$workspace/device/hisense/a6l/" device/hisense/a6l/
repo manifest -r -o a6l-source-revisions.xml
printf '\nSource prepared at %s\n' "$source_dir"
printf '%s\n' 'Next: run tools/build-linux-probe.sh from the desktop workspace inside WSL.'
printf '%s\n' 'This is a compile probe. Do not flash its output; see device/hisense/a6l/README.md.'
