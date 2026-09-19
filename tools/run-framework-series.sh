#!/usr/bin/env bash
# usage: run-framework-series.sh <series> <attempt>   (run inside WSL, in the background)
cd /mnt/c/Users/Pierre/Desktop/A6L
exec python3 tools/Test-FrameworkV48.py --rooted --runtime-kernel --apex-service --native-bootstrap --applications --health --bpf --hint-compat --series "$1" --attempt "$2" > ~/logs/test-framework-v$1-r$2.log 2>&1
