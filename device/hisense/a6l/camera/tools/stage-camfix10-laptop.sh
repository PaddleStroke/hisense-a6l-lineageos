#!/bin/bash
# Stage camfix10 on the laptop as ~/A6L-usb-20260915/v75/camera12 = copy of camera11 + camfix10 files, then verify.
# Run from WSL. Never deletes anything on the laptop (camera12 must not exist yet, or is refreshed file by file).
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=firmware/extracted/camera-20260930-camfix10
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
cd $W/.relay
out=$(./lap.sh 30 'cd ~/A6L-usb-20260915/v75 && { [ -d camera12 ] || cp -r camera11 camera12; } && ls camera12 | wc -l && echo LAPTOP_COPY_OK'); echo "$out"
echo "$out" | grep -q LAPTOP_COPY_OK || { echo STAGE_CAMFIX10_FAIL laptop unreachable; exit 1; }
for f in qcom-camss.ko run-camera.sh SHA256SUMS; do
  $SCP "C:/Users/Pierre/Desktop/A6L/$A/$f" a6l-laptop:A6L-usb-20260915/v75/camera12/$f 2>&1 | tail -2
done
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera12 && echo "camera12 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; sha256sum qcom-camss.ko run-camera.sh SHA256SUMS'
echo STAGE_CAMFIX10_DONE
