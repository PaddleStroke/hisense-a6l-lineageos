#!/bin/bash
# camera15 (29 Sep 2026): CONFIRM bundle for the clean ROM camera modules (docs/camera-rom-20260929.md).
#   firmware/extracted/camera-20260929-rom = camera14 (camera-20260929-camfix12) with
#     qcom-camss.ko   <- camera-rom1-20260929/v67/qcom-camss.ko (the ROM rom1 build for the V67/V75-usb kernel)
#     imx576_a6l.ko, s5k3t1.ko <- camera-20260927-camfix5 (= the camfix2 builds the ROM stages; hi846/gt9769/cci identical)
#     extra/qcom-camss-camfix12.ko <- camera14 qcom-camss.ko (only for the optional MODE=hi846diag)
#     run-camera.sh   <- device/hisense/a6l/camera/run-camera15.sh
#   laptop ~/A6L-usb-20260915/v75/camera15 = cp -r camera14 + the changed files, then sha256sum -c.
# WSL only. Never deletes anything (camera15 on the laptop is created by cp -r or refreshed file by file).
set -u
W=/mnt/c/Users/Pierre/Desktop/A6L; X=$W/firmware/extracted
A=$X/camera-20260929-rom; OLD=$X/camera-20260929-camfix12; R1=$X/camera-rom1-20260929; C5=$X/camera-20260927-camfix5
SCP="timeout 90 /mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes"
( cd $R1 && sha256sum -c --quiet SHA256SUMS ) || { echo "STAGE_CAMERA15_FAIL rom1 sums"; exit 1; }
( cd $OLD && sha256sum -c --quiet SHA256SUMS ) || { echo "STAGE_CAMERA15_FAIL camera14 sums"; exit 1; }
mkdir -p $A/extra
for f in $(cd $OLD && ls); do [ -f $OLD/$f ] && cp $OLD/$f $A/; done
cp $OLD/extra/a6l_cam_ovl.ko $A/extra/
cp $R1/v67/qcom-camss.ko $A/qcom-camss.ko
cp $C5/imx576_a6l.ko $C5/s5k3t1.ko $A/
for m in hi846 gt9769 i2c-qcom-cci v4l2-cci; do cmp -s $C5/$m.ko $A/$m.ko && echo "$m.ko = ROM build" || echo "WARN $m.ko differs from the ROM build"; done
cp $OLD/qcom-camss.ko $A/extra/qcom-camss-camfix12.ko
tr -d '\r' < $W/device/hisense/a6l/camera/run-camera15.sh > $A/run-camera.sh
( cd $A && sha256sum *.ko extra/*.ko *.dtbo a6l_camcap raw10_to_png.py run-camera.sh load-order.txt > SHA256SUMS
  echo "entries $(ls | wc -l) + extra $(ls extra | wc -l); sums $(wc -l < SHA256SUMS) ok $(sha256sum -c SHA256SUMS 2>&1 | grep -c ': OK$')"
  sha256sum qcom-camss.ko imx576_a6l.ko s5k3t1.ko extra/qcom-camss-camfix12.ko run-camera.sh SHA256SUMS )
cd $W/.relay
out=$(./lap.sh 40 'cd ~/A6L-usb-20260915/v75 && { [ -d camera15 ] || cp -r camera14 camera15; } && mkdir -p camera15/extra && echo LAPTOP_COPY_OK'); echo "$out"
echo "$out" | grep -q LAPTOP_COPY_OK || { echo STAGE_CAMERA15_FAIL laptop unreachable; exit 1; }
for f in qcom-camss.ko imx576_a6l.ko s5k3t1.ko extra/qcom-camss-camfix12.ko run-camera.sh SHA256SUMS; do
  $SCP "C:/Users/Pierre/Desktop/A6L/firmware/extracted/camera-20260929-rom/$f" a6l-laptop:A6L-usb-20260915/v75/camera15/$f 2>&1 | tail -2
done
./lap.sh 40 'cd ~/A6L-usb-20260915/v75/camera15 && echo "camera15 bad=$(sha256sum -c SHA256SUMS 2>&1 | grep -vc ": OK$") ok=$(sha256sum -c SHA256SUMS 2>&1 | grep -c ": OK$")"; sha256sum qcom-camss.ko run-camera.sh SHA256SUMS; ls | wc -l; ls extra'
echo STAGE_CAMERA15_DONE
