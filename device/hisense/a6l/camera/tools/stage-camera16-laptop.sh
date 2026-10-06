#!/bin/bash
# camera16 (29 Sep 2026): hi846 2-lane bundle (docs/hi846-20260929.md).
#   firmware/extracted/camera-20260929-hi846 = camera-20260929-rom (camera15) with
#     hi846.ko             <- ROM source + camfix16 knob a6l_rd (build-camera16.sh); extra/hi846-rom.ko = the camera15/ROM build
#     extra/a6l_cam_ovl.ko <- + param hi846_lanes (4 = camera15 dtbo, 2 = a6l-camera-v75-hi846-2lane.dtbo); old one kept as
#                             extra/a6l_cam_ovl-camera15.ko
#     run-camera.sh        <- device/hisense/a6l/camera/run-camera16.sh
#   laptop ~/A6L-usb-20260915/v75/camera16 = cp -r camera15 + the changed files, then sha256sum -c.
# WSL only. Never deletes anything.
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; A=$W/firmware/extracted/camera-20260929-hi846
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
tr -d '\r' < $W/device/hisense/a6l/camera/run-camera16.sh > $A/run-camera.sh
( cd $A && sha256sum *.ko extra/*.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS
  echo "local sums $(wc -l < SHA256SUMS) ok $(sha256sum -c SHA256SUMS 2>&1 | grep -c ': OK$')" )
cd $W/.relay
out=$(./lap.sh 40 'cd ~/A6L-usb-20260915/v75 && { [ -d camera16 ] || cp -r camera15 camera16; } && mkdir -p camera16/extra logs/t37 && echo LAPTOP_COPY_OK'); echo "$out"
echo "$out" | grep -q LAPTOP_COPY_OK || { echo STAGE_CAMERA16_FAIL laptop unreachable; exit 1; }
for f in hi846.ko extra/a6l_cam_ovl.ko extra/a6l_cam_ovl-camera15.ko extra/hi846-rom.ko a6l-camera-v75-hi846-2lane.dtbo run-camera.sh SHA256SUMS; do
  $SCP "C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260929-hi846/$f" a6l-laptop:A6L-usb-20260915/v75/camera16/$f 2>&1 | tail -2
done
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera16 && echo "camera16 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; sha256sum hi846.ko extra/a6l_cam_ovl.ko run-camera.sh SHA256SUMS; ls | wc -l; ls extra'
echo STAGE_CAMERA16_DONE
