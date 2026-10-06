#!/bin/bash
# camera17 (29 Sep 2026): hi846 round 2 bundle (docs/hi846-20260929.md "Round 2").
#   firmware/extracted/camera-20260929-hi846b = camera-20260929-hi846 (camera16) with
#     hi846.ko      <- camfix17 (s_ctrl fix a6l_fix=1 + runtime knobs); extra/hi846-camfix16.ko, extra/hi846-ctrlfix.ko (bare fix)
#     qcom-camss.ko <- rom1 (same source/srcversion as camera16) + diag17 runtime knobs (default off); extra/qcom-camss-rom1.ko
#     run-camera.sh <- device/hisense/a6l/camera/run-camera17.sh  (no hdiag, no rmmod)
#   laptop ~/A6L-usb-20260915/v75/camera17 = cp -r camera16 + the changed files (camfix12 camss removed from the copy's SHA list
#   is not needed: it is simply not referenced), then sha256sum -c.
# WSL only. Never deletes anything.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=$W/firmware/extracted/camera-20260929-hi846b
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
tr -d '\r' < $W/device/hisense/a6l/camera/run-camera17.sh > $A/run-camera.sh
( cd $A && sha256sum *.ko extra/*.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS
  echo "local sums $(wc -l < SHA256SUMS) ok $(sha256sum -c SHA256SUMS 2>&1 | grep -c ': OK$')" )
cd $W/.relay
out=$(./lap.sh 40 'cd ~/A6L-usb-20260915/v75 && { [ -d camera17 ] || cp -r camera16 camera17; } && mkdir -p camera17/extra logs/t38 && echo LAPTOP_COPY_OK'); echo "$out"
echo "$out" | grep -q LAPTOP_COPY_OK || { echo STAGE_CAMERA17_FAIL laptop unreachable; exit 1; }
for f in hi846.ko qcom-camss.ko extra/hi846-camfix16.ko extra/hi846-ctrlfix.ko extra/qcom-camss-rom1.ko run-camera.sh SHA256SUMS; do
  $SCP "C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260929-hi846b/$f" a6l-laptop:A6L-usb-20260915/v75/camera17/$f 2>&1 | tail -2
done
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera17 && echo "camera17 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; sha256sum hi846.ko qcom-camss.ko run-camera.sh SHA256SUMS; ls | wc -l; ls extra'
echo STAGE_CAMERA17_DONE
