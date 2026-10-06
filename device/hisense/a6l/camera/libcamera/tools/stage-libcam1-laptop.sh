#!/bin/bash
# Stage the libcamera phase-1 bundle (firmware/extracted/libcamera-20260929) on the laptop as
# ~/A6L-usb-20260915/v75/libcam1, then verify. Run from WSL. Never deletes anything on the laptop.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=firmware/extracted/libcamera-20260929
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -r -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
cd $W/.relay
out=$(./lap.sh 30 'cd ~/A6L-usb-20260915/v75 && ls -d camera15 && { [ -e libcam1 ] && echo LIBCAM1_EXISTS || echo LAPTOP_OK; }'); echo "$out"
echo "$out" | grep -q LAPTOP_OK || { echo "STAGE_LIBCAM1_FAIL (laptop unreachable, camera15 missing, or libcam1 already exists)"; exit 1; }
$SCP "C:/Users/Pierre/Desktop/A6L/$A" a6l-laptop:A6L-usb-20260915/v75/libcam1 2>&1 | tail -2
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/libcam1 && echo "libcam1 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; find . -type f | wc -l; sha256sum SHA256SUMS run-libcam.sh bin/cam'
echo STAGE_LIBCAM1_DONE
