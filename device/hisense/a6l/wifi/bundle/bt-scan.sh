#!/system/bin/sh
# A6L v75/wifi: Bluetooth discovery for the V74/V75 test recovery (wifi agent, 26 Sep 2026).
# ATTENDED ONLY, RF: NEEDS PIERRE'S EXPLICIT GO (A6L_RF_APPROVED=1). Run AFTER v74/radio2/run-bt.sh (hci0 present).
# LE + BR/EDR discovery through the mgmt socket for A6L_BT_SECS (15) s; no pairing, no connection.
# hci0 UNCONFIGURED (QCA default address) -> sets a RAM-only public address: A6L_BT_ADDR, else WLAN MAC + 1 from persist.
set -u
D=${D:-/tmp/wifi}; B=$D/bin; BB=$B/busybox
[ "${A6L_RF_APPROVED:-0}" = 1 ] || { echo "A6L_BT_FAIL RF run not approved"; exit 6; }
( cd "$D" && $BB sha256sum -c SHA256SUMS > /dev/null 2>&1 ) || { echo "A6L_BT_FAIL payload hash"; exit 3; }
chmod 755 $B/*
[ -e /sys/class/bluetooth/hci0 ] || { echo "A6L_BT_FAIL no hci0 (run v74/radio2/run-bt.sh first)"; exit 4; }
MARK="A6L_BTSCAN_$$"; echo "$MARK" > /dev/kmsg
for r in /sys/class/rfkill/rfkill*; do [ -e $r/type ] || continue; [ "$(cat $r/type)" = bluetooth ] || continue
    echo "A6L_RFKILL $(basename $r) $(cat $r/name) soft=$(cat $r/soft) hard=$(cat $r/hard)"; [ "$(cat $r/soft)" = 1 ] && echo 0 > $r/soft; done
ADDR=${A6L_BT_ADDR:-}
if [ -z "$ADDR" ]; then
    pdev=""; for u in /sys/class/block/*/uevent; do $BB grep -q "^PARTNAME=persist$" $u && pdev=$(dirname $u); done
    if [ -n "$pdev" ]; then mm=$(cat $pdev/dev); mknod /dev/a6l-ro-persist-bt b ${mm%%:*} ${mm##*:}
        W=$($BB dd if=/dev/a6l-ro-persist-bt bs=1M 2>/dev/null | $BB grep -a -o "Intf0MacAddress=[0-9A-Fa-f]\{12\}" | $BB head -n 1 | $BB cut -d= -f2); rm -f /dev/a6l-ro-persist-bt
        [ -n "$W" ] && ADDR=$(echo "$W" | $BB awk '{h="0123456789ABCDEF"; n=0; for(i=1;i<=12;i++) n=n*16+index(h,toupper(substr($0,i,1)))-1; n=n+1; s=""; for(i=0;i<6;i++){b=n%256; n=(n-b)/256; s=sprintf("%02X",b) (i?":":"") s}; print s}')
    fi
fi
echo "A6L_BT_ADDR_CANDIDATE ${ADDR:-none} (used only if hci0 is unconfigured; RAM only)"
$B/a6l-bt-scan -x 0 -t ${A6L_BT_SECS:-15} ${ADDR:+-a $ADDR}; rc=$?
dmesg | $BB sed -n "/$MARK/,\$p" | $BB grep -i -E "bluetooth|hci0|qca" | tail -n 15
echo "A6L_BT_SCAN_DONE rc=$rc"
exit $rc
