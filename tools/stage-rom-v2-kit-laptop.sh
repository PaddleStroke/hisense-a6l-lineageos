#!/usr/bin/env bash
# merge2 (25 Sep 2026): copy the rom-v2 kit (/home/a6l/rom-v2/kit-<tag>/rom-v2) to the laptop ~/A6L-usb-20260915/rom-v2
# and verify every sha on the laptop. NO adb, never touches the phone. Run with nohup (multi-GB copy).
# usage: stage-rom-v2-kit-laptop.sh <tag> [laptop dir name, default rom-v2]   (merge3: r3 -> rom-v3)
# bug hunt round2 install-tools (29 Sep 2026): the rom-v1 tools keep the install backup INSIDE the kit dir
# (<dir>/capture-rom-v1-install = the only way back to stock). Re-staging onto a dir that holds a capture used to move it to
# <dir>.prev (the new kit's restore no longer finds it) and the next re-stage `rm -rf <dir>.prev` DELETED it. Now refused
# (KIT_HAS_CAPTURE, exit 4) while <dir>, <dir>.prev or <dir>.new holds any capture-* directory: move the capture to a safe
# place first or stage under a new dir name. STAGE_KIT_DONE only after LAPTOP_SHA_OK + swap (else exit 3).
# Test overrides (tools/tests/test-stage-kit-laptop.sh): A6L_KIT_SSH, A6L_KIT_SCP, A6L_KIT_WSLPATH, A6L_KIT_BASE, A6L_KIT_WS.
set -uo pipefail
TAG=$1; N=${2:-rom-v2}; K=${A6L_KIT_BASE:-/home/a6l/rom-v2}/kit-$TAG/rom-v2
case "$N" in ''|*/*|.*|*[!A-Za-z0-9._-]*) echo "BAD_DIR_NAME $N"; exit 2;; esac
# Optional retained kit used only as a read-only rsync basis. Final hashes remain mandatory.
DELTA_BASE=${A6L_KIT_DELTA_BASE:-}
if [ -n "$DELTA_BASE" ]; then
  case "$DELTA_BASE" in */*|.*|*[!A-Za-z0-9._-]*) echo "BAD_DELTA_BASE $DELTA_BASE"; exit 2;; esac
  case "$DELTA_BASE" in "$N"|"$N.new"|"$N.prev") echo "DELTA_BASE_CONFLICT $DELTA_BASE"; exit 2;; esac
  command -v rsync >/dev/null || { echo "RSYNC_MISSING"; exit 2; }
fi
SSH=${A6L_KIT_SSH:-"/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes -o ConnectTimeout=10"}
SCP=${A6L_KIT_SCP:-"/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf -o BatchMode=yes -o ConnectTimeout=10"}
WSLPATH=${A6L_KIT_WSLPATH:-"wslpath -w"}
[ -f $K/images/SHA256SUMS ] || { echo "KIT_MISSING $K"; exit 1; }
# refuse before copying anything when the target (or its .prev/.new) holds a capture (install backup / restore record)
GUARD="( cd ~/A6L-usb-20260915 2>/dev/null || exit 0; for d in $N $N.prev $N.new; do for c in \$d/capture-*; do [ -e \"\$c\" ] && { echo \"KIT_HAS_CAPTURE \$c\"; exit 4; }; done; done; exit 0 )"
g=$($SSH a6l-laptop "$GUARD" < /dev/null | tr -d '\r'); grc=$?
[ -n "$g" ] && echo "$g"
if [ $grc = 4 ]; then
  echo "REFUSED: ~/A6L-usb-20260915/$N (or .prev/.new) holds a capture (the stock backup lives there). Move it to a safe place or use another dir name."
  exit 4
elif [ $grc != 0 ]; then
  echo "SSH_FAIL rc=$grc (capture check not done; nothing copied)"; exit 5
fi
# scp.exe needs a Windows-readable source: stage through C: (the WSL ext4 path is not visible to the Windows ssh client)
WS=${A6L_KIT_WS:-/mnt/c/Users/Pierre/Desktop/A6L/.relay/tmp}/rom-v2-kit-$TAG; rm -rf $WS; mkdir -p $WS; cp -r $K/. $WS/
( cd $K && find . -type f ! -name KIT-SHA256SUMS | sort | xargs sha256sum ) > $WS/KIT-SHA256SUMS
$SSH a6l-laptop "rm -rf ~/A6L-usb-20260915/$N.new" < /dev/null
W=$($WSLPATH $WS)
if [ -n "$DELTA_BASE" ]; then
  $SSH a6l-laptop "test -d ~/A6L-usb-20260915/$DELTA_BASE/images && command -v rsync >/dev/null" < /dev/null || { echo "DELTA_BASE_OR_RSYNC_MISSING"; rm -rf $WS; exit 2; }
  rsync -a --checksum --no-whole-file --compress --stats --copy-dest="../$DELTA_BASE" \
    -e "$SSH" "$WS/" "a6l-laptop:A6L-usb-20260915/$N.new/" < /dev/null || { echo "RSYNC_FAIL"; rm -rf $WS; exit 2; }
else
  $SCP -r -q "$W" "a6l-laptop:A6L-usb-20260915/$N.new" < /dev/null || { echo "SCP_FAIL"; rm -rf $WS; exit 2; }
fi
out=$($SSH a6l-laptop "cd ~/A6L-usb-20260915 && $GUARD && cd $N.new && sha256sum -c --quiet KIT-SHA256SUMS && (cd images && sha256sum -c --quiet SHA256SUMS) && echo LAPTOP_SHA_OK && cd .. && rm -rf $N.prev && { [ -d $N ] && mv $N $N.prev; true; } && mv $N.new $N && echo LAPTOP_SWAP_OK && ls -la $N $N/images" < /dev/null | tr -d '\r')
echo "$out"
rm -rf $WS
if ! grep -qx LAPTOP_SHA_OK <<<"$out" || ! grep -qx LAPTOP_SWAP_OK <<<"$out"; then
  echo "== $(date) STAGE_KIT_FAIL $TAG -> ~/A6L-usb-20260915/$N (verification or swap failed; the old kit dir is unchanged unless LAPTOP_SWAP_OK)"
  exit 3
fi
echo "== $(date) STAGE_KIT_DONE $TAG -> ~/A6L-usb-20260915/$N"
