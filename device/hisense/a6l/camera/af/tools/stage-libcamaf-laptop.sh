#!/bin/bash
# Stage the camera phase 3 AF bundle (firmware/extracted/libcamera-af-20260929, docs/camera-af-video-20260929.md) on the
# laptop as ~/A6L-usb-20260915/v75/libcamaf, then verify. Run from WSL. Never deletes anything on the laptop.
# The sweep mode also needs v75/libcam2 (no-AF bundle) and v75/camera15 (modules + a6l_camcap) already staged.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=firmware/extracted/libcamera-af-20260929
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -r -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
cd $W/.relay
out=$(./lap.sh 30 'cd ~/A6L-usb-20260915/v75 && ls -d camera15 libcam2 && { [ -e libcamaf ] && echo LIBCAMAF_EXISTS || echo LAPTOP_OK; }'); echo "$out"
echo "$out" | grep -q LAPTOP_OK || { echo "STAGE_LIBCAMAF_FAIL (laptop unreachable, camera15/libcam2 missing, or libcamaf already exists)"; exit 1; }
$SCP "C:/Users/Pierre/Desktop/A6L/$A" a6l-laptop:A6L-usb-20260915/v75/libcamaf 2>&1 | tail -2
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/libcamaf && echo "libcamaf bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; find . -type f | wc -l; sha256sum SHA256SUMS run-af.sh bin/cam'
echo STAGE_LIBCAMAF_DONE
