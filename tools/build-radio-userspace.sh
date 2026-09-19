#!/usr/bin/env bash
# Build Qualcomm remote-processor companion userspace for Android (vendor): libqrtr, qrtr-lookup,
# rmtfs, tqftpserv from pinned linux-msm sources. Build only; nothing is staged or run.
set -eo pipefail
src=/home/a6l/src-radio; tree=/home/a6l/android/a6l-lineage24
test "$(git -C $src/rmtfs rev-parse HEAD)" = b30a3eb38f9af283f18dbd3c7755653efc52c094
test "$(git -C $src/tqftpserv rev-parse HEAD)" = c2559a26098f6d2b36a946ef0b6ad02223264c3e
test "$(git -C $src/qrtr rev-parse HEAD)" = 29e36ae164389580a0f8ea7a7fdb728140ae978d
mkdir -p $tree/external/linux-msm
for n in qrtr rmtfs tqftpserv; do rm -rf $tree/external/linux-msm/$n; cp -r $src/$n $tree/external/linux-msm/$n; rm -rf $tree/external/linux-msm/$n/.git; done
exec bash /mnt/c/Users/Pierre/Desktop/A6L/tools/build-framework-module.sh radio-userspace libqrtr qrtr-lookup qrtr-cfg rmtfs tqftpserv
