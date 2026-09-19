#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# One-time prerequisites in the dedicated Ubuntu distribution. No phone access.
set -euo pipefail
[[ $(id -u) == 0 ]] || { echo 'Run as the WSL root user' >&2; exit 1; }
if ! id a6l >/dev/null 2>&1; then
    useradd --create-home --shell /bin/bash a6l
fi
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y git git-lfs repo gnupg flex bison build-essential zip \
    curl zlib1g-dev libc6-dev-i386 x11proto-core-dev libx11-dev lib32z1-dev \
    libgl1-mesa-dev libxml2-utils xsltproc unzip fontconfig python3 \
    python3-venv rsync bc cpio device-tree-compiler lz4 lzop libssl-dev \
    libelf-dev e2fsprogs
