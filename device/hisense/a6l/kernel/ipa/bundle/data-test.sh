#!/system/bin/sh
# data2 agent (25 Sep 2026), bundle v75/ipa3: ONE-COMMAND attended mobile-data test. RF: typed by Pierre only.
# data3 agent (25 Sep 2026 evening), bundle v75/ipa4: same command with D=/tmp/ipa4 (default); step 6 now pings/DNS/TCP
#   through the bundled a6l-net (raw socket bound to rmnet_data0), KEEP default 90 s, IPA status decode (A6L_IPA_ST).
# Precondition: fresh V74 boot, v75/ipa4 pushed to /tmp/ipa4 and v74/radio2 to /tmp/radio2, and
#   MODE=load of this bundle PASSED first (no RF; the IPA must probe BEFORE the modem starts).
# usage (laptop, ~/A6L-usb-20260915):
#   adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; export A6L_RF_APPROVED=1; export A6L_PIN=<PIN>; sh /tmp/ipa4/data-test.sh' 2>&1 | tee v75/logs/ipa4-data-$(date +%H%M).txt
# Steps: 1 radio2 (modem, A6L_KEEP=1, 20 s watch)  2 SIM PIN (only if A6L_PIN is set; never printed)  3 online + wait for
#   LTE/PS registration (<= 60 s)  4 IPA handshake (MODE=status, no register dump)  5 QMI DPM open port + WDA data format
#   (MODE=dataformat)  6 data call + ping by IP + DNS (MODE=data, KEEP s)  7 low power (modem stays up, stops transmitting).
# Env: APN= (default profile), KEEP=90, TARGETS="1.1.1.1 8.8.8.8", DUMP=1, IPV6=0, SKIP_RADIO=1 (modem already up), A6L_DPM=0 / A6L_WDA_UL=7 A6L_WDA_DL=7 (variants).
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/ipa4}; R=${RADIO:-/tmp/radio2}; Q=$D/a6l-qmi
[ "${A6L_RF_APPROVED:-0}" = 1 ] || { echo "A6L_DATA3_REFUSED needs A6L_RF_APPROVED=1 (Pierre types this command)"; exit 3; }
[ -e /sys/bus/platform/drivers/ipa2-lite/14780000.ipa ] || { echo "A6L_DATA3_FAIL ipa2-lite not bound: run MODE=load of $D first (fresh boot, before the modem)"; exit 4; }
[ -e /sys/module/ipa2_lite/parameters/status_log ] || echo "  WARNING: the loaded ipa2_lite is not the ipa4 build (no A6L_IPA_ST status decode / QMI filter-rule replies / route index 7)"
chmod 755 $Q 2>/dev/null
st() { echo "A6L_DATA3_STEP $*"; echo "A6L_DATA3_STEP $*" > /dev/kmsg; }
q() { "$Q" "$@" 2>&1 | grep -v "^WARNING: linker"; }
mss() { for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name 2>/dev/null)" = 4080000.remoteproc ] && cat $r/state; done; }
# 1. modem
if [ "${SKIP_RADIO:-0}" = 1 ] || [ "$(mss)" = running ]; then st "1 radio2 skipped (modem $(mss))"
else st "1 radio2"; A6L_KEEP=1 A6L_WATCH=${A6L_WATCH:-20} D=$R sh $R/run.sh 2>&1 | grep -E "A6L_(RADIO_DONE|MODEM_STABLE|MODEM_CRASH|HW_FAIL|KEEP)" ; fi
[ "$(mss)" = running ] || { echo "A6L_DATA3_FAIL modem not running ($(mss))"; exit 5; }
# 2. PIN
if [ -n "${A6L_PIN:-}" ]; then st "2 SIM PIN"; q pin "$A6L_PIN" | grep -E "VERIFY_PIN|FAIL"; else st "2 no A6L_PIN (SIM without PIN?)"; fi
# 3. online + registration
st "3 online"; q mode online | grep -E "SET_OPMODE|REFUSED"
n=0; reg=""; while [ $n -lt 12 ]; do reg=$(q reg | grep "A6L_QMI_SERVING"); case "$reg" in *reg=registered*" ps=1 "*) break;; esac; sleep 5; n=$((n+1)); done
echo "$reg"; case "$reg" in *reg=registered*" ps=1 "*) echo "  registered, PS attached after $((n*5)) s";; *) echo "  WARNING: not PS-attached after 60 s (continuing)";; esac
# 4. IPA handshake (no register dump)
st "4 ipa status"; MODE=status D=$D sh $D/run.sh 2>&1 | grep -E "\[.\]|A6L_IPA2_MODEM|rpm="
# 5. DPM + WDA
st "5 dataformat"; MODE=dataformat D=$D sh $D/run.sh 2>&1 | grep -E "A6L_QMI_DATA|A6L_HW_FAIL|WARNING"
# 6. data call + ping
st "6 data"; MODE=data KEEP=${KEEP:-90} D=$D sh $D/run.sh 2>&1 | grep -vE "^WARNING: linker"
# 7. stop transmitting (modem stays up for more tests; ril-test.sh stop or reboot to finish)
st "7 lowpower"; q mode lowpower | grep SET_OPMODE
echo "A6L_DATA3_DONE (PASS markers: A6L_QMI_DATAFORMAT_PASS, A6L_QMI_DATA_UP, A6L_NET_PING_PASS/A6L_IPA2_PING_PASS, A6L_IPA2_DNS_PASS, A6L_IPA2_TCP_PASS)"
