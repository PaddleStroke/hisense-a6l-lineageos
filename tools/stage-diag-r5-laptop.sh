#!/bin/bash
# diag-r5 (30 Sep 2026): copy /home/a6l/diag-r5/kit to the laptop ~/A6L-usb-20260915/diag-r5 and check every sha there.
# Same method as stage-rom-v2-kit-laptop.sh (scp.exe via .relay/tmp, verify in <dir>.new, then swap). An existing
# diag-r5/logs (attended results) is carried over into the new dir, never deleted. Laptop only, no phone.
set -uo pipefail
K=/home/a6l/diag-r5/kit; N=diag-r5
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes -o ConnectTimeout=10"
SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes -o ConnectTimeout=10"
[ -f $K/KIT-SHA256SUMS ] || { echo "KIT_MISSING"; exit 1; }
(cd $K && sha256sum -c --quiet KIT-SHA256SUMS) || { echo "LOCAL_SHA_FAIL"; exit 1; }
WS=/mnt/c/Users/Pierre/Desktop/A6L/.relay/tmp/diag-r5-kit; rm -rf $WS; mkdir -p $WS; cp -r $K/. $WS/
$SSH a6l-laptop "rm -rf ~/A6L-usb-20260915/$N.new" < /dev/null
$SCP -r -q "$(wslpath -w $WS)" "a6l-laptop:A6L-usb-20260915/$N.new" < /dev/null || { echo SCP_FAIL; rm -rf $WS; exit 2; }
rm -rf $WS
out=$($SSH a6l-laptop "cd ~/A6L-usb-20260915/$N.new && sha256sum -c --quiet KIT-SHA256SUMS && (cd phone && sha256sum -c --quiet SHA256SUMS) && echo LAPTOP_SHA_OK && cd .. && { [ -d $N/logs ] && mv $N/logs $N.new/logs; true; } && rm -rf $N.prev && { [ -d $N ] && mv $N $N.prev; true; } && mv $N.new $N && mkdir -p $N/logs && chmod 755 $N/host/*.sh $N/tests/*.sh && echo LAPTOP_SWAP_OK && ls -la $N $N/images && bash $N/tests/test-bootwrite-mock.sh | tail -n 1" < /dev/null | tr -d '\r')
echo "$out"; grep -qx LAPTOP_SHA_OK <<<"$out" && grep -qx LAPTOP_SWAP_OK <<<"$out" && echo STAGE_DIAG_R5_DONE || { echo STAGE_DIAG_R5_FAIL; exit 3; }
