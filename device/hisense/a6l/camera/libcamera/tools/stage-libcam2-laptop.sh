#!/bin/bash
# Stage the libcamera libcam2 bundle (firmware/extracted/libcamera-20260929b, 29 Sep 2026 lc1 fixes) on the laptop as
# ~/A6L-usb-20260915/v75/libcam2, then verify. Run from WSL. Never deletes anything on the laptop.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=firmware/extracted/libcamera-20260929b
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -r -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
cd $W/.relay
out=$(./lap.sh 30 'cd ~/A6L-usb-20260915/v75 && ls -d camera15 && { [ -e libcam2 ] && echo LIBCAM2_EXISTS || echo LAPTOP_OK; }'); echo "$out"
echo "$out" | grep -q LAPTOP_OK || { echo "STAGE_LIBCAM2_FAIL (laptop unreachable, camera15 missing, or libcam2 already exists)"; exit 1; }
$SCP "C:/Users/Pierre/Desktop/A6L/$A" a6l-laptop:A6L-usb-20260915/v75/libcam2 2>&1 | tail -2
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/libcam2 && echo "libcam2 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; find . -type f | wc -l; sha256sum SHA256SUMS run-libcam.sh bin/cam'
echo STAGE_LIBCAM2_DONE
