exec < /dev/null
# Mock run of wifi-test.sh in a user+net namespace: veth wlan0<->peer0, busybox udhcpd on peer0, stub wpa_supplicant/wpa_cli.
T=/home/a6l/wifi-build/mock; rm -rf $T; mkdir -p $T; cp -r /home/a6l/wifi-build/build/bundle/wifi $T/wifi
cat > $T/wifi/bin/wpa_supplicant <<'S'
#!/bin/sh
[ "$1" = -v ] && { echo "wpa_supplicant v2.11 (STUB)"; exit 0; }
while [ $# -gt 0 ]; do case $1 in -P) P=$2; shift;; -c) C=$2; shift;; -f) F=$2; shift;; esac; shift; done
echo "STUB conf: mode=$(stat -c %a $C) lines=$(wc -l < $C) has_psk=$(grep -c 'psk=' $C) ssid_hex=$(grep -c 'ssid=4d79' $C) country=$(grep country $C)" >&2
cp $C /home/a6l/wifi-build/mock/conf.copy
echo "1700000000.1: wlan0: Trying to associate with 02:00:00:00:00:01 (SSID='My Box' freq=2437 MHz)" > $F
echo "1700000000.2: wlan0: CTRL-EVENT-CONNECTED - Connection to 02:00:00:00:00:01 completed [id=0 id_str=]" >> $F
sleep 300 & echo $! > $P; exit 0
S
cat > $T/wifi/bin/wpa_cli <<'S'
#!/bin/sh
eval last=\${$#}
case $last in status) printf 'bssid=02:00:00:00:00:01\nfreq=2437\nssid=My Box\nid=0\nmode=station\npairwise_cipher=CCMP\nkey_mgmt=SAE\npmf=2\nwpa_state=COMPLETED\naddress=7c:b3:7b:99:40:46\n';;
 signal_poll) printf 'RSSI=-52\nLINKSPEED=144\nNOISE=9999\nFREQUENCY=2437\n';;
 scan_results) printf 'bssid / frequency / signal level / flags / ssid\n02:00:00:00:00:01\t2437\t-52\t[SAE]\tMy Box\n';;
 terminate) pkill -f 'sleep 300';;
 *) echo OK;; esac
S
sed -i '1s#.*#\#!/bin/sh#' $T/wifi/udhcpc.script  # mock only: no /system/bin/sh in WSL
DBB=/home/a6l/wifi-build/deb-busybox/usr/bin/busybox
if [ ! -x $DBB ]; then mkdir -p /home/a6l/wifi-build/deb-busybox; cd /home/a6l/wifi-build/deb-busybox
  f=$(curl -sSf https://deb.debian.org/debian/pool/main/b/busybox/ | grep -o 'busybox-static_[^"]*_amd64.deb' | sort -u | tail -n 1)
  curl -sSf -o b.deb https://deb.debian.org/debian/pool/main/b/busybox/$f && dpkg-deb -x b.deb . && echo "host udhcpd from $f"; cd /; fi
chmod 755 $T/wifi/bin/*; ( cd $T/wifi && sha256sum bin/* firmware/* *.sh udhcpc.script | sed 's#  #  ./#' > SHA256SUMS )
cat > $T/inner.sh <<'S'
set -u; T=/home/a6l/wifi-build/mock; BB=$T/wifi/bin/busybox; HB=/home/a6l/wifi-build/deb-busybox/usr/bin/busybox
mount -t sysfs none /sys || echo "sysfs mount failed"
# "AP side" = a second netns holding peer0 (gateway 192.168.77.1, DHCP, DNS, and 1.1.1.1 with ICMP + HTTP)
unshare -n sleep 200 & NP=$!; sleep 0.5
ip link add wlan0 type veth peer name peer0; ip link set peer0 netns $NP; ip link set lo up
cat > $T/ap.sh <<'A'
T=/home/a6l/wifi-build/mock; HB=/home/a6l/wifi-build/deb-busybox/usr/bin/busybox
ip link set lo up; ip addr add 192.168.77.1/24 dev peer0; ip link set peer0 up; ip addr add 1.1.1.1/32 dev lo
echo 0 > /proc/sys/net/ipv4/ping_group_range 2>/dev/null
$HB udhcpd -f $T/udhcpd.conf > $T/udhcpd.log 2>&1 &
python3 -c '
import socket
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); s.bind(("0.0.0.0",53))
while True:
    q,a=s.recvfrom(512); i=12
    while q[i]: i+=q[i]+1
    qe=i+5
    s.sendto(q[:2]+b"\x81\x80"+q[4:6]+b"\x00\x01\x00\x00\x00\x00"+q[12:qe]+b"\xc0\x0c\x00\x01\x00\x01\x00\x00\x00\x3c\x00\x04\x01\x01\x01\x01",a)
' > $T/dnsd.log 2>&1 &
mkdir -p $T/www; echo hi > $T/www/index.html; $HB httpd -f -p 1.1.1.1:80 -h $T/www > $T/httpd.log 2>&1 &
sleep 60
A
printf 'interface peer0\nstart 192.168.77.10\nend 192.168.77.20\nlease_file %s/leases\noption router 192.168.77.1\noption dns 192.168.77.1\noption subnet 255.255.255.0\noption lease 600\n' $T > $T/udhcpd.conf; touch $T/leases
nsenter -t $NP -n --preserve-credentials sh $T/ap.sh & AP=$!
sleep 1; ls /sys/class/net
printf 'My Box\nZq$$ w"\\"x9Kp\n' | env A6L_RF_APPROVED=1 D=$T/wifi sh -x $T/wifi/wifi-test.sh > $T/out.txt 2> $T/trace.txt; echo "rc=$?" >> $T/out.txt
pkill -f deb-busybox; pkill -f 'SOCK_DGRAM'; kill $NP; tail -n 3 $T/udhcpd.log $T/dnsd.log $T/httpd.log
S
rm -rf /tmp/a6l-wifi; timeout 100 unshare -rnm sh $T/inner.sh 2>&1 | tail -12
cat $T/out.txt
echo "=== leak check (should be 0 0):"; grep -c 'My Box' $T/out.txt; grep -c 'x9Kp' $T/out.txt
ls -la /tmp/a6l-wifi 2>&1 | head -2
echo "=== conf seen by the stub (network block, psk line checked, not shown):"; grep -v psk= $T/conf.copy; grep -c '^	psk="Zq\$\$ w"\\"x9Kp"$' $T/conf.copy
echo "=== leftover processes:"; pgrep -f 'sleep 300' || echo none
echo '=== trace tail'; grep -v -E 'x9Kp|My Box' $T/trace.txt | tail -n 25
