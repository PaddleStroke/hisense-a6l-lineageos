#!/system/bin/sh
# ATTENDED ONLY, V74 diagnostic recovery. kvoice/kipa (24 Sep 2026): IPA v2.6L (ipa-legacy) bring-up for mobile data.
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/kipa MODE=load|status|off sh /tmp/kipa/run.sh
#   load   : NO RF. Fresh boot, BEFORE radio2 (modem must NOT be running yet). Applies the runtime DT overlay
#            (extra/a6l_ipa_ovl.ko: ipa@147c0000 + private-compatible IPA BAM), loads deps, a6l_ipa_bam.ko, ipa_legacy.ko, rmnet.ko.
#            PASS = A6L_KIPA_LOAD_PASS: "IPA driver initialized", both drivers bound, no SMMU context fault, no oops.
#            The IPA core clock is switched on and IPA/BAM registers are programmed; nothing is transmitted.
#   status : after Pierre ran radio2 (RF, by hand, as usual): netdev rmnet_ipa0 present?, IPA/QMI lines, SMMU faults, modem state.
#   off    : unload ipa_legacy + a6l_ipa_bam (overlay stays; reboot to drop it).
# Mobile data itself (QMI WDS start + QMAP rmnet_data0 + IP config) needs the ril tooling and `ip` with rmnet support:
# a ROM/LineageOS-from-RAM test, see docs/kvoice-20260924.md section IPA.
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
( cd "$D" && sha256sum -c --ignore-missing SHA256SUMS > /dev/null ) || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_KIPA_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\$p"; }
MODE=${MODE:-load}
mss_state() { for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name 2>/dev/null)" = 4080000.remoteproc ] && cat $r/state; done; }
faults() { dmesg | grep -ciE "Unhandled context fault|smmu.*fault|ipa.*(error|fail)|Internal error|Oops"; }
case "$MODE" in
load)
  case "$(mss_state)" in running) echo "A6L_HW_FAIL modem already running: load IPA first (fresh boot), then radio2"; exit 5;; esac
  grep -q "^ipa_legacy " /proc/modules && { echo "A6L_KIPA already loaded"; exit 0; }
  F0=$(faults)
  [ -n "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-ipa 2>/dev/null)" ] || insmod "$D/extra/a6l_ipa_ovl.ko" || { echo "A6L_HW_FAIL overlay"; klog | tail; exit 4; }
  while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules && continue
    insmod "$D/modules/$ko" || { echo "A6L_HW_FAIL insmod $ko"; klog | tail -n 20; exit 4; }; done < "$D/modules/order.txt"
  sleep 3
  klog | grep -iE "A6L_IPA|ipa|bam|smmu|iommu" | tail -n 30
  ok=1
  ls /sys/bus/platform/drivers/a6l-ipa-bam/ 2>/dev/null | grep -q "14784000" || { echo "  a6l-ipa-bam NOT bound"; ok=0; }
  ls /sys/bus/platform/drivers/ipa-legacy/ 2>/dev/null | grep -q "147c0000" || { echo "  ipa-legacy NOT bound"; ok=0; }
  klog | grep -q "IPA driver initialized" || { echo "  no 'IPA driver initialized'"; ok=0; }
  [ "$(faults)" = "$F0" ] || { echo "  NEW fault/error lines in dmesg"; ok=0; }
  [ $ok = 1 ] && echo "A6L_KIPA_LOAD_PASS now Pierre runs radio2 (RF) by hand, then MODE=status" || echo "A6L_KIPA_LOAD_FAIL";;
status)
  echo "modem: $(mss_state)"; ls /sys/class/net/ | tr '\n' ' '; echo
  [ -d /sys/class/net/rmnet_ipa0 ] && echo "A6L_KIPA_NETDEV rmnet_ipa0 present (mtu $(cat /sys/class/net/rmnet_ipa0/mtu))" || echo "A6L_KIPA_NETDEV none"
  dmesg | grep -iE "ipa|rmnet|qmi.*ipa|smmu.*fault|modem.*crash|fatal" | tail -n 30
  cat /sys/bus/platform/devices/147c0000.ipa/modem/* 2>/dev/null | head;;
off) rmmod ipa_legacy; rmmod a6l_ipa_bam; echo "A6L_KIPA_OFF (overlay kept until reboot)";;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
