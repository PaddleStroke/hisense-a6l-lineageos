#!/system/bin/sh
# volte2 agent (25 Sep 2026), bundle v75/volte2: ONE-COMMAND attended VoLTE step 1-2 test (modem-centric IMS).
# RF (online + IMS PDN): typed by Pierre only. Never dials, never changes the PDC/MCFG selection.
# Precondition: V74 recovery, v74/radio2 pushed to /tmp/radio2, v75/volte2 pushed to /tmp/volte2 (sha256sum -c first).
# usage (laptop, ~/A6L-usb-20260915):
#   adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; export A6L_RF_APPROVED=1; export A6L_PIN=<PIN>; sh /tmp/volte2/volte2-test.sh' 2>&1 | tee v75/logs/volte2-$(date +%H%M).txt
# Steps: 1 radio2 (A6L_KEEP=1) unless the modem runs  2 PIN (only if A6L_PIN)  3 read-only PDC/MCFG + lookup (volte-probe all)
#   4 online, wait LTE/PS registration (<= 60 s)  5 lookup again (IMSA before our 770 server?)  6 a6l-imsdcm (hosts QMI 770,
#   answers PDP_ACTIVATE with a WDS call on the modem's profile, 6 = "ims")  7 volte-probe imsa-watch $WATCH (180 s)
#   8 a6l-imsdcm summary (stops the IMS PDN)  9 low power.
# Env: WATCH=180, SKIP_RADIO=1, A6L_IMSDCM_MUX (default 9 if ipa2-lite is bound, else 0), A6L_IMSDCM_ADDR=bin,
#   A6L_IMSDCM_UNKNOWN=notsup, A6L_IMSDCM_NOWDS=1 (handshake only), A6L_QMI_LOG=4 (hex of every QMI message), KEEP_ONLINE=1.
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/volte2}; R=${RADIO:-/tmp/radio2}; Q=$D/a6l-qmi; P=$D/volte-probe; I=$D/a6l-imsdcm
WATCH=${WATCH:-180}; L=${L:-/tmp/volte2-logs}; mkdir -p $L
[ "${A6L_RF_APPROVED:-0}" = 1 ] || { echo "A6L_VOLTE2_REFUSED needs A6L_RF_APPROVED=1 (Pierre types this command)"; exit 3; }
chmod 755 $Q $P $I 2>/dev/null
st() { echo "A6L_VOLTE2_STEP $*"; echo "A6L_VOLTE2_STEP $*" > /dev/kmsg 2>/dev/null; }
q() { "$Q" "$@" 2>&1 | grep -v "^WARNING: linker"; }
mss() { [ -n "${A6L_DRYRUN_MSS:-}" ] && { echo "$A6L_DRYRUN_MSS"; return; }; for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name 2>/dev/null)" = 4080000.remoteproc ] && cat $r/state; done; }
# 1. modem
if [ "${SKIP_RADIO:-0}" = 1 ] || [ "$(mss)" = running ]; then st "1 radio2 skipped (modem $(mss))"
else st "1 radio2"; A6L_KEEP=1 A6L_WATCH=${A6L_WATCH:-20} D=$R sh $R/run.sh 2>&1 | grep -E "A6L_(RADIO_DONE|MODEM_STABLE|MODEM_CRASH|HW_FAIL|KEEP)"; fi
[ "$(mss)" = running ] || { echo "A6L_VOLTE2_FAIL modem not running ($(mss))"; exit 5; }
# 2. PIN
if [ -n "${A6L_PIN:-}" ]; then st "2 SIM PIN"; q pin "$A6L_PIN" | grep -E "VERIFY_PIN|FAIL"; else st "2 no A6L_PIN"; fi
# 3. read-only: PDC active HW/SW config (Orange FR MBN?), config list, NAS/WDS/DSD, IMS services
st "3 volte-probe all (read-only, low power)"; "$P" all 2>&1 | grep -v "^WARNING: linker" | tee $L/probe-pre.txt | grep -E "PDC|IMS_SVC|IMSA|IMSS|IMSP|NAS_SYSINFO|LOOKUP|apn='ims'" | head -60
# 4. online
st "4 online"; q mode online | grep -E "SET_OPMODE|REFUSED"
n=0; reg=""; while [ $n -lt 12 ]; do reg=$(q reg | grep "A6L_QMI_SERVING"); case "$reg" in *reg=registered*" ps=1 "*) break;; esac; sleep 5; n=$((n+1)); done
echo "$reg"; case "$reg" in *reg=registered*" ps=1 "*) echo "  registered, PS attached after $((n*5)) s";; *) echo "  WARNING: not PS-attached after 60 s (continuing)";; esac
# 5. online snapshot (NAS ims_voice_support / lte_voice_domain, IMS services published by themselves?)
st "5 volte-probe all (online, before 770)"; "$P" all 2>&1 | grep -v "^WARNING: linker" | tee $L/probe-online.txt | grep -E "IMS_SVC|IMSA|NAS_SYSINFO|PDC_ACTIVE|PDC_INFO" | head -30
# 6. our IMSDCM server
if [ -z "${A6L_IMSDCM_MUX:-}" ]; then
  if [ -e /sys/bus/platform/drivers/ipa2-lite/14780000.ipa ]; then export A6L_IMSDCM_MUX=9; else export A6L_IMSDCM_MUX=0; fi
fi
st "6 a6l-imsdcm (host QMI 770, mux=$A6L_IMSDCM_MUX)"
"$I" --duration $((WATCH + 20)) --imsa > $L/imsdcm.txt 2>&1 &
ip=$!
sleep 2; grep -E "PUBLISHED|FAIL" $L/imsdcm.txt
# 7. IMSA: a6l-imsdcm --imsa decodes it as soon as service 33 appears; volte-probe imsa-watch adds raw TLV dumps
st "7 wait for IMSA (<= $WATCH s)"; t=0
while [ $t -lt $WATCH ]; do
  "$P" lookup 2>/dev/null | grep -q "IMS_SVC 33 .*PRESENT" && break
  sleep 10; t=$((t+10))
done
if [ $t -lt $WATCH ]; then
  echo "  IMSA published after ~$t s"; "$P" imsa-watch $((WATCH - t)) 2>&1 | grep -v "^WARNING: linker" | tee $L/imsa-watch.txt | grep -E "IMSA|DONE" | head -80
  "$P" all 2>&1 | grep -v "^WARNING: linker" | tee $L/probe-after.txt | grep -E "IMS_SVC|IMSA_|IMSS_|IMSP_" | head -40
else echo "  IMSA never published in $WATCH s"; : > $L/imsa-watch.txt; fi
# 8. imsdcm summary (stops the IMS PDN, withdraws 770)
st "8 a6l-imsdcm log"; kill $ip 2>/dev/null; sleep 3; wait $ip 2>/dev/null
grep -v "^WARNING: linker" $L/imsdcm.txt | grep -E "A6L_IMSDCM" | head -120
if [ "${KEEP_ONLINE:-0}" != 1 ]; then st "9 lowpower"; q mode lowpower | grep SET_OPMODE; fi
nreq=$(grep -c "A6L_IMSDCM_REQ" $L/imsdcm.txt); nact=$(grep -c "PDP_ACTIVATE apn=" $L/imsdcm.txt)
nup=$(grep -c "A6L_IMSDCM_IND PDP_ACTIVATE id=[0-9]* success" $L/imsdcm.txt)
imsa=absent; grep -qE "IMSA PRESENT|IMS_SVC 33 .*PRESENT" $L/imsdcm.txt $L/probe-online.txt && imsa=present
regd=no; grep -qE "status=registered" $L/imsdcm.txt && regd=yes
echo "A6L_VOLTE2_RESULT dcm_requests=$nreq pdp_activate=$nact pdp_up=$nup imsa=$imsa registered=$regd (logs $L)"
echo "A6L_VOLTE2_DONE (PASS = imsa=present registered=yes; see docs/volte2-20260925.md for the decision table)"
