#!/system/bin/sh
# A6L v75/wifi: Wi-Fi association test for the V74/V75 test recovery (wifi agent, 26 Sep 2026).
# ATTENDED ONLY, RF: NEEDS PIERRE'S EXPLICIT GO (A6L_RF_APPROVED=1). Run AFTER v74/radio2 with A6L_KEEP=1 (wlan0 present).
# Credentials: A6L_WIFI_SSID / A6L_WIFI_PSK, or (preferred, nothing on any command line) two lines on stdin: SSID, password.
#   They are never printed. The wpa_supplicant config lives only in /tmp/a6l-wifi (mode 700) and is deleted at the end.
#   Empty password = open network.
# Steps: static wpa_supplicant (nl80211; WPA2-PSK / WPA3-SAE / PMF optional; country FR) -> COMPLETED -> busybox udhcpc ->
#   default route (metric 1234, removed at the end) -> ping gateway + 1.1.1.1 -> DNS -> HTTP HEAD -> signal/bitrate -> disconnect.
# Knobs: A6L_WIFI_KEYMGMT (default "WPA-PSK WPA-PSK-SHA256 SAE"), A6L_WIFI_PMF (0/1/2, default 1), A6L_WIFI_COUNTRY (FR),
#   A6L_WIFI_TIMEOUT (association wait, 40 s), A6L_WIFI_HOLD (seconds to stay connected before disconnecting, 0),
#   A6L_WIFI_DEBUG=1 (wpa_supplicant -d to the tmp log; still never -K, so no key material), A6L_WLAN_MAC=xx:..:xx.
set -u
export TMPDIR=/tmp  # 27 Sep: recovery has no /data/local/tmp; mksh here-docs need a writable TMPDIR
D=${D:-/tmp/wifi}
B=$D/bin; BB=$B/busybox; WD=/tmp/a6l-wifi; CTRL=$WD/ctrl
[ "${A6L_RF_APPROVED:-0}" = 1 ] || { echo "A6L_WIFI_FAIL RF run not approved (A6L_RF_APPROVED=1 only with Pierre's explicit go)"; exit 6; }
( cd "$D" && $BB sha256sum -c SHA256SUMS > /dev/null 2>&1 ) || ( cd "$D" && sha256sum -c SHA256SUMS > /dev/null ) || { echo "A6L_WIFI_FAIL payload hash"; exit 3; }
chmod 755 $B/* $D/udhcpc.script
[ -e /sys/class/net/wlan0 ] || { echo "A6L_WIFI_FAIL no wlan0 (run v74/radio2 first with A6L_KEEP=1 A6L_WLAN_MAC=persist)"; exit 4; }

# --- credentials (never echoed) ---
SSID=${A6L_WIFI_SSID:-}; PSK=${A6L_WIFI_PSK:-}; unset A6L_WIFI_SSID A6L_WIFI_PSK
if [ -z "$SSID" ]; then IFS= read -r SSID || true; IFS= read -r PSK || true; fi
CR=$(printf '\r'); SSID=${SSID%"$CR"}; PSK=${PSK%"$CR"}
[ -n "$SSID" ] || { echo "A6L_WIFI_FAIL no SSID given (stdin line 1 or A6L_WIFI_SSID)"; exit 5; }
SLEN=${#SSID}; PLEN=${#PSK}
[ $SLEN -le 32 ] || { echo "A6L_WIFI_FAIL SSID longer than 32 bytes"; exit 5; }
if [ $PLEN -ne 0 ] && { [ $PLEN -lt 8 ] || [ $PLEN -gt 64 ]; }; then echo "A6L_WIFI_FAIL password length $PLEN (WPA needs 8..63 chars or 64 hex)"; exit 5; fi
echo "A6L_WIFI_CRED ssid_len=$SLEN password_len=$PLEN (values not shown)"

MARK="A6L_WIFI_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | $BB sed -n "/$MARK/,\$p"; }
mask() { $BB sed -e "s/SSID='[^']*'/SSID='<hidden>'/g" -e "s/ssid='[^']*'/ssid='<hidden>'/g" -e "s/SSID '[^']*'/SSID '<hidden>'/g" -e 's/ssid="[^"]*"/ssid=<hidden>/g' -e '/^ssid=/d' -e '/SSID:/d' -e '/hexdump/d' -e '/passphrase/d' -e '/psk=/d'; }
IWB=""; if [ -x $B/iw ]; then IWB="$B/iw"; elif [ -x /tmp/radio2/bin/iw ]; then IWB="env LD_LIBRARY_PATH=/tmp/radio2/bin /tmp/radio2/bin/iw"; fi
iwc() { [ -n "$IWB" ] && $IWB "$@" 2>&1; }
WPID=""; ROUTE=""; RES_ASSOC=FAIL; RES_DHCP=FAIL; RES_GW=FAIL; RES_PING=FAIL; RES_DNS=FAIL; RES_HTTP=FAIL
cli() { $B/wpa_cli -p $CTRL -i wlan0 "$@" 2>&1; }
cleanup() {
    trap '' 0 INT TERM HUP
    if [ -n "$WPID" ] && [ -d /proc/$WPID ]; then cli disconnect > /dev/null; sleep 1; cli terminate > /dev/null; sleep 1; kill $WPID 2>/dev/null; fi
    for p in $($BB pidof wpa_supplicant udhcpc 2>/dev/null); do case "$($BB tr '\0' ' ' < /proc/$p/cmdline 2>/dev/null)" in *"$B/"*) kill $p 2>/dev/null;; esac; done
    [ -n "$ROUTE" ] && $BB ip route del default via "$ROUTE" dev wlan0 metric 1234 2>/dev/null
    $BB ip -4 addr flush dev wlan0 2>/dev/null; $BB ip link set wlan0 down 2>/dev/null
    rm -rf $WD; SSID=""; PSK=""; HEX=""
    echo "A6L_WIFI_CLEANUP conf_deleted=$([ -e $WD ] && echo no || echo yes) wpa_supplicant=$($BB pidof wpa_supplicant > /dev/null 2>&1 && echo still-running || echo stopped) wlan0=$(cat /sys/class/net/wlan0/operstate 2>/dev/null)"
    echo "A6L_WIFI_RESULT assoc=$RES_ASSOC dhcp=$RES_DHCP gateway_ping=$RES_GW ping_1.1.1.1=$RES_PING dns=$RES_DNS http=$RES_HTTP"
    [ $RES_ASSOC = PASS ] && [ $RES_DHCP = PASS ] && [ $RES_PING = PASS ] && [ $RES_DNS = PASS ] && [ $RES_HTTP = PASS ] && echo A6L_WIFI_ALL_PASS
}
trap 'cleanup' 0; trap 'echo A6L_WIFI_INTERRUPTED; exit 130' INT TERM HUP

# --- stale daemons, rfkill, regulatory, MAC ---
for p in $($BB pidof wpa_supplicant udhcpc 2>/dev/null); do echo "A6L_WIFI_STALE pid $p killed"; kill $p; done
for r in /sys/class/rfkill/rfkill*; do [ -e $r/type ] || continue; echo "A6L_RFKILL $(basename $r) $(cat $r/type) $(cat $r/name) soft=$(cat $r/soft) hard=$(cat $r/hard)"
    [ "$(cat $r/type)" = wlan ] && [ "$(cat $r/soft)" = 1 ] && echo 0 > $r/soft; done
mkdir -p /lib/firmware; cp $D/firmware/regulatory.db $D/firmware/regulatory.db.p7s /lib/firmware/
COUNTRY=${A6L_WIFI_COUNTRY:-FR}
iwc reg reload > /dev/null; iwc reg set $COUNTRY > /dev/null
echo "A6L_WIFI_REG $(iwc reg get | $BB grep -m1 '^country')  (iw: ${IWB:-none})"
[ -n "${A6L_WLAN_MAC:-}" ] && { $BB ip link set wlan0 down; $BB ip link set wlan0 address "$A6L_WLAN_MAC"; }
echo "A6L_WIFI_MAC $(cat /sys/class/net/wlan0/address)"
dmesg | $BB grep -i -E "ath10k.*(board|qmi chip|fw_version|wmi|regd|country)" | tail -n 8

# --- config (heredoc: the secrets never appear on any command line) ---
rm -rf $WD; mkdir -p $WD; chmod 700 $WD; umask 077
$BB od -An -tx1 -v > $WD/h <<EOT
$SSID
EOT
HEX=$($BB tr -d ' \n' < $WD/h); HEX=${HEX%0a}; rm -f $WD/h
[ ${#HEX} -le 64 ] || { echo "A6L_WIFI_FAIL SSID longer than 32 bytes"; exit 5; }
KM=${A6L_WIFI_KEYMGMT:-WPA-PSK WPA-PSK-SHA256 SAE}; PMF=${A6L_WIFI_PMF:-1}
if [ $PLEN -eq 0 ]; then KM=NONE; KEYLINE="#open"
elif [ $PLEN -eq 64 ]; then case "$PSK" in *[!0-9a-fA-F]*) echo "A6L_WIFI_FAIL 64-char password must be hex"; exit 5;; esac; KEYLINE="psk=$PSK"
else KEYLINE="psk=\"$PSK\""; fi
cat > $WD/wpa.conf <<CONF
ctrl_interface=DIR=$CTRL
update_config=0
country=$COUNTRY
sae_pwe=2
pmf=$PMF
network={
	ssid=$HEX
	scan_ssid=1
	key_mgmt=$KM
	ieee80211w=$PMF
	$KEYLINE
}
CONF
KEYLINE=""; HEX=""
echo "A6L_WIFI_CONF key_mgmt=\"$KM\" pmf=$PMF country=$COUNTRY (file in $WD only, $(wc -c < $WD/wpa.conf) bytes)"

# --- associate ---
DBG=""; [ "${A6L_WIFI_DEBUG:-0}" = 1 ] && DBG=-d
$B/wpa_supplicant -B -i wlan0 -D nl80211 -c $WD/wpa.conf -P $WD/wpa.pid -f $WD/wpa.log -t $DBG > $WD/wpa.start 2>&1
rc=$?; sleep 1; WPID=$(cat $WD/wpa.pid 2>/dev/null)
echo "A6L_WIFI_SUPPLICANT rc=$rc pid=${WPID:-none} $($B/wpa_supplicant -v 2>&1 | head -n 1)"
[ -n "$WPID" ] || { mask < $WD/wpa.start; mask < $WD/wpa.log | tail -n 20; exit 7; }
T0=$(date +%s); TO=${A6L_WIFI_TIMEOUT:-40}; last=""
while :; do
    st=$(cli status | $BB sed -n 's/^wpa_state=//p')
    [ "$st" != "$last" ] && { echo "A6L_WIFI_STATE $st t=$(( $(date +%s) - T0 ))s"; last=$st; }
    [ "$st" = COMPLETED ] && break
    [ $(( $(date +%s) - T0 )) -ge $TO ] && break
    sleep 1
done
echo "--- supplicant events (SSID hidden)"
mask < $WD/wpa.log | $BB grep -E "CTRL-EVENT|Trying to associate|Associated with|WPA: Key negotiation|SME:|SAE|authentication|reason|status_code|TEMP-DISABLED" | tail -n 25
N=$(cli scan_results | $BB grep -c -E '^[0-9a-f]{2}:'); echo "A6L_WIFI_SCAN_RESULTS $N BSS"
if [ "$st" != COMPLETED ]; then
    echo "A6L_WIFI_ASSOC_FAIL state=$st after ${TO}s"
    mask < $WD/wpa.log | $BB grep -E "WRONG_KEY|NETWORK-NOT-FOUND|ASSOC-REJECT|AUTH-REJECT|DISCONNECTED" | tail -n 5
    klog | $BB grep -i -E "ath10k|wlan0|cfg80211" | tail -n 20
    exit 8
fi
RES_ASSOC=PASS
echo "A6L_WIFI_ASSOC_PASS after $(( $(date +%s) - T0 ))s"
cli status | $BB grep -E '^(bssid|freq|key_mgmt|pairwise_cipher|group_cipher|pmf|sae_group|wifi_generation|mode|address)=' | $BB sed 's/^/A6L_WIFI_STATUS /'

# --- DHCP ---
export A6L_BB=$BB A6L_LEASE=$WD/lease
$BB udhcpc -i wlan0 -f -q -n -t 5 -T 3 -A 2 -s $D/udhcpc.script -x hostname:a6l-test 2>&1 | $BB grep -v -E '^udhcpc: (started|broadcasting|sending)' | tail -n 8
if [ -s $WD/lease ]; then RES_DHCP=PASS; . $WD/lease; echo "A6L_WIFI_DHCP_PASS ip=$ip router=$router dns=$dns"
else echo "A6L_WIFI_DHCP_FAIL (no lease)"; exit 9; fi
GW=${router%% *}; DNS1=${dns%% *}
$BB ip -4 addr show dev wlan0 | $BB grep inet
if [ -n "$GW" ]; then $BB ip route add default via "$GW" dev wlan0 metric 1234 && ROUTE=$GW; fi
$BB ip route show | $BB sed 's/^/A6L_WIFI_ROUTE /'

# --- connectivity ---
[ -n "$GW" ] && { $B/a6l-net ping wlan0 "$GW" 3 2 | tail -n 1 | $BB grep -q PASS && RES_GW=PASS; echo "A6L_WIFI_GATEWAY_PING $RES_GW $GW"; }
out=$($B/a6l-net ping wlan0 1.1.1.1 4 2); echo "$out" | tail -n 3; echo "$out" | $BB grep -q A6L_NET_PING_PASS && RES_PING=PASS
[ $RES_PING = PASS ] || { $BB ping -c 3 -W 2 -I wlan0 1.1.1.1 2>&1 | tail -n 2; $BB ping -c 2 -W 2 -I wlan0 1.1.1.1 > /dev/null 2>&1 && RES_PING=PASS; }
for s in $DNS1 1.1.1.1; do
    out=$($B/a6l-net dns wlan0 "$s" connectivitycheck.gstatic.com 3); echo "$out" | tail -n 2
    echo "$out" | $BB grep -q A6L_NET_DNS_PASS && { RES_DNS=PASS; HIP=$(echo "$out" | $BB sed -n 's/^A6L_NET_DNS .* A \([0-9.]*\)$/\1/p' | head -n 1); break; }
done
if [ -n "${HIP:-}" ]; then $B/a6l-net http wlan0 "$HIP" 80 connectivitycheck.gstatic.com | tail -n 1 | $BB grep -q PASS && RES_HTTP=PASS
    echo "A6L_WIFI_HTTP $RES_HTTP connectivitycheck.gstatic.com ($HIP)"; fi
[ $RES_HTTP = PASS ] || { $B/a6l-net http wlan0 1.1.1.1 80 one.one.one.one | tail -n 1; $B/a6l-net http wlan0 1.1.1.1 80 one.one.one.one > /dev/null && RES_HTTP=PASS; }

# --- link quality ---
cli signal_poll | $BB sed 's/^/A6L_WIFI_SIGNAL /'
iwc dev wlan0 link | mask | $BB grep -E "Connected|freq|signal|bitrate" | $BB sed 's/^/A6L_WIFI_LINK /'
iwc dev wlan0 station dump | $BB grep -E "signal|bitrate|expected throughput|tx failed|rx drop" | $BB sed 's/^/A6L_WIFI_STA /'
H=${A6L_WIFI_HOLD:-0}; [ "$H" -gt 0 ] 2>/dev/null && { echo "A6L_WIFI_HOLD ${H}s"; sleep "$H"; cli signal_poll | $BB grep RSSI; }
klog | $BB grep -i -E "ath10k|wlan0" | tail -n 10
echo "A6L_WIFI_DISCONNECT"
[ $RES_PING = PASS ] && [ $RES_DNS = PASS ] && [ $RES_HTTP = PASS ] && exit 0
exit 1
