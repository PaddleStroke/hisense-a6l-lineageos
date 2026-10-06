#!/bin/bash
# Stage camfix12 on the laptop as ~/A6L-usb-20260915/v75/camera14 = copy of camera13 + camfix12 files, then verify.
# Run from WSL. Never deletes anything on the laptop (camera14 is created by cp -r, or refreshed file by file).
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=firmware/extracted/camera-20260929-camfix12
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
cd $W/.relay
out=$(./lap.sh 30 'cd ~/A6L-usb-20260915/v75 && { [ -d camera14 ] || cp -r camera13 camera14; } && ls camera14 | wc -l && echo LAPTOP_COPY_OK'); echo "$out"
echo "$out" | grep -q LAPTOP_COPY_OK || { echo STAGE_CAMFIX12_FAIL laptop unreachable; exit 1; }
for f in qcom-camss.ko run-camera.sh SHA256SUMS; do
  $SCP "C:/Users/Pierre/Desktop/A6L/$A/$f" a6l-laptop:A6L-usb-20260915/v75/camera14/$f 2>&1 | tail -2
done
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera14 && echo "camera14 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; sha256sum qcom-camss.ko imx576_a6l.ko s5k3t1.ko run-camera.sh SHA256SUMS; ls | wc -l'
echo STAGE_CAMFIX12_DONE
