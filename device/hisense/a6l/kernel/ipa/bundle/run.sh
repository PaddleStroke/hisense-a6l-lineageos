#!/system/bin/sh
# ATTENDED ONLY, V74 diagnostic recovery. ipa2fix agent, bundle v75/ipa2b (24 Sep 2026): IPA v2.6L via ipa2-lite, STEP-LOGGED.
# The first v75/ipa2 MODE=load reset the phone with no log. This bundle walks the probe one hardware phase at a time.
#
# BEFORE ANY MODE=load, on the laptop (in ~/A6L-usb-20260915), start the kernel-log stream; it is the only log that
# survives a reset:
#     adb -s HLTE730T-PROBE shell dmesg -w > v75/logs/ipa-klog.txt &
# (if dmesg is not found: adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; dmesg -w' > v75/logs/ipa-klog.txt &)
# After a reset: grep -a "A6L_IPA" v75/logs/ipa-klog.txt | tail  -> the last "A6L_IPA_STEP n" line is the phase that killed it.
#
# usage (laptop): adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; export D=/tmp/ipa2b; export MODE=load; export STOP_AT=3; sh /tmp/ipa2b/run.sh'
#   load     NO RF. FRESH boot, BEFORE radio2 (the modem must NOT be running).
#            STOP_AT=n     the probe stops cleanly just BEFORE step n (0 or unset = full probe). Steps:
#                          0 probe entered (SMMU attach done) | 1 RPM IPA clock | 2 icc votes | 3 first IPA read (VERSION)
#                          4 IPA reads | 5 first BAM read | 6 IPA_COMP_SW_RESET | 7 COMP_CFG/BCR | 8 BAM global reset
#                          9 BAM global config/enable | 10 SRAM direct canaries | 11 endpoint/BAM pipe setup
#                          12 IPA_ROUTE | 13 SRAM init by immediate commands (first DMA) | 14 IRQs | 15 netdevs/QMI | 16 done
#            Re-running MODE=load with a larger STOP_AT in the same boot is allowed after a clean STOP (the driver is
#            re-inserted and re-probes the existing device; the overlay is applied only once).
#            NO_IPA_RESET=1 (skip step 6 pulse)  NO_BAM_RESET=1 (skip step 8 pulse)  NO_SRAM_DIRECT=1 (step 10 without SRAM writes)
#            NOIOMMU=1 (overlay without iommus: register steps only, use with STOP_AT<=13; first load of the boot only)
#            IDENTITY=1 (SMMU identity/bypass variant)   STEP_MS=<ms> pause per step (default 200 here)
#            IPA_PARAMS="..." extra ipa2_lite params (e.g. clk_hz=75000000 comp_cfg=0x1)
#   status   after Pierre started radio2 (RF, by hand): QMI handshake lines, rmnet_ipa0, register snapshot.
#   data     RF (needs A6L_RF_APPROVED=1, typed by Pierre): see v75/ipa2 (unchanged).
#   diag     register snapshot only (a6l_diag sysfs; only when the driver is bound).
#   There is no "off" once fully probed: removing ipa2_lite shuts the modem down (driver remove path). Reboot instead.
set -u
D=${D:-$(dirname "$0")}
Q=${Q:-/tmp/ril/a6l-qmi}
MODE=${MODE:-load}
[ "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo "A6L_HW_FAIL wrong image"; exit 2; }
# manual per-file hash check (toybox sha256sum has no -c/--ignore-missing); missing files are skipped
bad=0; while read -r h f; do f=${f#\*}; case "$f" in *SHA256SUMS) continue;; esac; [ -f "$D/$f" ] || continue
  [ "$(sha256sum "$D/$f" | cut -d' ' -f1)" = "$h" ] || { echo "  hash mismatch: $f"; bad=1; }; done < "$D/SHA256SUMS"
[ $bad = 0 ] || { echo "A6L_HW_FAIL payload hash"; exit 3; }
MARK="A6L_IPA2B_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\$p"; }
mss_state() { for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name 2>/dev/null)" = 4080000.remoteproc ] && cat $r/state; done; }
faults() { dmesg | grep -ciE "Unhandled context fault|smmu.*fault|Internal error|Oops|BUG:|A6L_IPA.*fail"; }
DEV=/sys/bus/platform/devices/14780000.ipa
diag() { cat $DEV/a6l_diag 2>/dev/null || echo "  (no a6l_diag: driver not bound)"; }
ipalog() { dmesg | grep -E "A6L_IPA|ipa2-lite|14780000\.ipa|rmnet|arm-smmu.*(fault|0x19c0)" | tail -n ${1:-40}; }
case "$MODE" in
load)
  case "$(mss_state)" in running|crashed) echo "A6L_HW_FAIL modem already $(mss_state): reboot, load IPA first, then radio2"; exit 5;; esac
  ST=${STOP_AT:-0}; P="stop_at=$ST step_ms=${STEP_MS:-200}"
  [ "${NO_IPA_RESET:-0}" = 1 ] && P="$P no_ipa_reset=1"
  [ "${NO_BAM_RESET:-0}" = 1 ] && P="$P no_bam_reset=1"
  [ "${NO_SRAM_DIRECT:-0}" = 1 ] && P="$P no_sram_direct=1"
  P="$P ${IPA_PARAMS:-}"
  echo "A6L_IPA2B load: ipa2_lite $P (laptop must be streaming: adb shell dmesg -w > v75/logs/ipa-klog.txt)"
  OVL=$(tr -d '\0' 2>/dev/null < /proc/device-tree/chosen/hisense,a6l-ipa)
  if [ -e /sys/bus/platform/drivers/ipa2-lite/14780000.ipa ]; then echo "A6L_IPA2 already bound (full probe done): reboot to step again"; diag | head -3; exit 0; fi
  case "$OVL" in ""|ipa2-lite-v75*) ;; *) echo "A6L_HW_FAIL a foreign IPA overlay is applied ($OVL): reboot"; exit 5;; esac
  F0=$(faults)
  while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); [ "$n" = ipa2_lite ] && continue
    grep -q "^$n " /proc/modules && continue
    insmod "$D/modules/$ko" || { echo "A6L_HW_FAIL insmod $ko"; klog | tail -n 20; exit 4; }; done < "$D/modules/order.txt"
  # (re)insert ipa2_lite with this run's parameters; after a clean STOP it re-probes the existing device
  grep -q "^ipa2_lite " /proc/modules && { rmmod ipa2_lite || { echo "A6L_HW_FAIL rmmod ipa2_lite"; exit 4; }; }
  echo "A6L_IPA2B_$$ insmod ipa2_lite $P" > /dev/kmsg
  sync; sleep 1
  insmod "$D/modules/ipa2_lite.ko" $P || { echo "A6L_HW_FAIL insmod ipa2_lite.ko"; klog | tail -n 20; exit 4; }
  if [ -z "$OVL" ]; then
    O=""; [ "${IDENTITY:-0}" = 1 ] && O="identity=1"; [ "${NOIOMMU:-0}" = 1 ] && O="noiommu=1"
    sync; sleep 1
    insmod "$D/extra/a6l_ipa2_ovl.ko" $O || { echo "A6L_HW_FAIL overlay"; klog | tail -n 20; exit 4; }
  else
    echo "  overlay already applied ($OVL): the insmod above re-probed the device"
  fi
  sleep 3
  klog | grep -iE "A6L_IPA|ipa2-lite|14780000|smmu|iommu|rmnet" | tail -n 40
  last=$(klog | grep -o "A6L_IPA_STEP [0-9]*" | tail -n 1 | cut -d' ' -f2)
  if [ "$ST" != 0 ]; then
    if klog | grep -q "A6L_IPA_STEP $ST STOP"; then
      echo "A6L_IPA2_STEP_PASS steps 0..$((ST-1)) survived (stopped cleanly before step $ST). Next: STOP_AT=$((ST+1)) (or STOP_AT=0 for the full probe)"
    else
      echo "A6L_IPA2_STEP_FAIL no 'A6L_IPA_STEP $ST STOP' line (last step seen: ${last:-none}; see the lines above)"
    fi
    [ "$(faults)" = "$F0" ] || echo "  NEW fault/oops lines in dmesg"
    exit 0
  fi
  ok=1
  [ -e /sys/bus/platform/drivers/ipa2-lite/14780000.ipa ] || { echo "  ipa2-lite NOT bound to 14780000.ipa (last step ${last:-none})"; ok=0; }
  klog | grep -q "A6L_IPA sram tables initialised" || { echo "  no 'A6L_IPA sram tables initialised' (immediate commands on pipe 3 failed?)"; ok=0; }
  [ -d /sys/class/net/rmnet_ipa0 ] || { echo "  no rmnet_ipa0 netdev"; ok=0; }
  [ "$(faults)" = "$F0" ] || { echo "  NEW fault/oops lines in dmesg"; ok=0; }
  diag | head -8
  if [ $ok = 1 ]; then echo "A6L_IPA2_LOAD_PASS next: Pierre starts radio2 by hand (RF), then MODE=status"
  else echo "A6L_IPA2_LOAD_FAIL (reboot before retrying; try IDENTITY=1 only if the log shows context faults for 0x19c0)"; fi;;
status)
  echo "modem: $(mss_state)"
  ipalog 40
  for s in "modem IPA QMI service up" "INIT_DRIVER response OK" "uC INIT_COMPLETED" "INIT_COMPLETE indication sent" "modem PRESENT"; do
    dmesg | grep -q "A6L_IPA $s" && echo "  [x] $s" || echo "  [ ] $s"; done
  ls /sys/class/net/ | tr '\n' ' '; echo
  if dmesg | grep -q "A6L_IPA modem PRESENT"; then echo "A6L_IPA2_MODEM_READY rmnet_ipa0 attached (mtu $(cat /sys/class/net/rmnet_ipa0/mtu))"
  else echo "A6L_IPA2_MODEM_NOT_READY (handshake incomplete; see the unchecked steps above)"; fi
  diag;;
data)
  [ "${A6L_RF_APPROVED:-0}" = 1 ] || { echo "A6L_IPA2_REFUSED data needs A6L_RF_APPROVED=1 (Pierre's explicit go)"; exit 3; }
  [ -x "$Q" ] || chmod 755 "$Q" 2>/dev/null; [ -x "$Q" ] || { echo "A6L_HW_FAIL no a6l-qmi at $Q (push the ril bundle to /tmp/ril)"; exit 4; }
  dmesg | grep -q "A6L_IPA modem PRESENT" || echo "  WARNING: IPA handshake not complete (MODE=status); trying anyway"
  grep -q "^rmnet " /proc/modules || insmod "$D/modules/rmnet.ko" || { echo "A6L_HW_FAIL insmod rmnet"; exit 4; }
  ip link set rmnet_ipa0 up 2>/dev/null || ifconfig rmnet_ipa0 up
  L=/tmp/ipa2-data-$$.txt; KEEP=${KEEP:-60}
  echo "A6L_IPA2_STEP a6l-qmi data '${APN:-}' v4 $KEEP"
  "$Q" data "${APN:-}" v4 "$KEEP" > $L 2>&1 &
  QP=$!
  n=0; while [ $n -lt 45 ] && ! grep -qE "A6L_QMI_DATA_UP|A6L_QMI_DATA_FAIL" $L; do sleep 1; n=$((n+1)); done
  cat $L
  up=$(grep "A6L_QMI_DATA_UP" $L | head -1)
  [ -n "$up" ] || { echo "A6L_IPA2_DATA_FAIL no data call (see above)"; wait $QP; exit 1; }
  IF=$(echo "$up" | sed -n 's/.*if=\([^ ]*\).*/\1/p')
  ADDR=$(echo "$up" | sed -n 's/.*addr=\[\([^] ]*\).*/\1/p')
  GW=$(echo "$up" | sed -n 's/.*gw=\[\([^] ]*\).*/\1/p')
  DNS=$(echo "$up" | sed -n 's/.*dns=\[\([^] ]*\).*/\1/p')
  echo "A6L_IPA2_CALL if=$IF addr=$ADDR gw=$GW dns=$DNS"
  case "$ADDR" in */*) ;; *) ADDR="$ADDR/32";; esac
  ip addr add "$ADDR" dev "$IF" 2>/dev/null || ifconfig "$IF" "${ADDR%/*}" netmask 255.255.255.255 up
  ip link set "$IF" up 2>/dev/null
  # only the test destinations go through the modem (Wi-Fi/USB routes untouched)
  for h in "$DNS" 1.1.1.1; do [ -n "$h" ] && { ip route add "$h/32" dev "$IF" 2>/dev/null || route add -host "$h" dev "$IF"; }; done
  ip addr show "$IF" 2>/dev/null; ip route 2>/dev/null | grep "$IF"
  pass=0
  [ -n "$DNS" ] && { ping -c 4 -W 3 -I "$IF" "$DNS" && pass=1; }
  ping -c 4 -W 3 -I "$IF" 1.1.1.1 && pass=1
  if command -v nslookup > /dev/null && [ -n "$DNS" ]; then nslookup lineageos.org "$DNS" && echo "A6L_IPA2_DNS_PASS" || echo "A6L_IPA2_DNS_FAIL"; fi
  for i in rmnet_ipa0 "$IF"; do echo "$i rx $(cat /sys/class/net/$i/statistics/rx_packets) tx $(cat /sys/class/net/$i/statistics/tx_packets) rx_drop $(cat /sys/class/net/$i/statistics/rx_dropped) tx_drop $(cat /sys/class/net/$i/statistics/tx_dropped)"; done
  echo "ipa_lan0 rx $(cat /sys/class/net/ipa_lan0/statistics/rx_packets 2>/dev/null) drop $(cat /sys/class/net/ipa_lan0/statistics/rx_dropped 2>/dev/null)"
  diag | head -12
  ipalog 15
  [ $pass = 1 ] && echo "A6L_IPA2_PING_PASS mobile data passes packets" || echo "A6L_IPA2_PING_FAIL (IP but no packets: compare tx/rx counters and a6l_diag rd/wr offsets of pipes 4/5)"
  echo "waiting for a6l-qmi to stop the call (KEEP=$KEEP s)"; wait $QP; tail -n 2 $L;;
diag) echo "modem: $(mss_state)"; diag; ipalog 20;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
