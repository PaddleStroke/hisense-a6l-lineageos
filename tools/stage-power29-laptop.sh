#!/usr/bin/env bash
# agent power29 (29 Sep 2026): copy firmware/extracted/power-20260929 to the laptop as a NEW folder
# ~/A6L-usb-20260915/v75/power29 and verify it there (sha256sum -c). Run in WSL; laptop must be reachable (a6l-laptop).
set -euo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; S=/mnt/c/Windows/System32/OpenSSH; CF=C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf
cd $R/.relay
./lap.sh 30 'test -e ~/A6L-usb-20260915/v75/power29 && echo EXISTS || echo NEW'
./lap.sh 30 'mkdir -p ~/A6L-usb-20260915/v75/power29-tmp'
cd $R/firmware/extracted
timeout 120 $S/scp.exe -r -F $CF -o BatchMode=yes power-20260929/. a6l-laptop:A6L-usb-20260915/v75/power29-tmp/
cd $R/.relay
./lap.sh 30 'cd ~/A6L-usb-20260915/v75 && { test -e power29 || mv power29-tmp power29; } && cd power29 && sha256sum -c SHA256SUMS && echo A6L_POWER29_LAPTOP_OK'
