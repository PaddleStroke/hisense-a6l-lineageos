#!/usr/bin/env bash
set -euo pipefail
[[ $(id -un) == a6l ]] || exit 1
mkdir -p "$HOME/logs"
# This dedicated build account never publishes commits with this identity.
git config --global user.name 'A6L local build'
git config --global user.email 'a6l@localhost'
export GIT_TERMINAL_PROMPT=0
bash /mnt/c/Users/Pierre/Desktop/A6L/tools/prepare-linux-build.sh --skip-deps 2>&1 | tee "$HOME/logs/source-sync.log"
