#!/system/bin/sh
# volte3 agent (26 Sep 2026), bundle v75/volte3: VoLTE steps 3 (signalling: call domain IMS/CS) and 4 (audio: CVD
# session for the VoLTE call). Extends v75/volte2 (IMS PDN via our IMSDCM 770 server, IMSA registration).
# RF (MODE=call dials!): typed by Pierre only. Never changes the PDC/MCFG selection. Never dials 112.
# Bundles on the phone: /tmp/volte3 (this), /tmp/audio6 (v75/audio6), /tmp/radio2 (v74/radio2), same boot.
# usage (laptop ~/A6L-usb-20260915, `sha256sum -c SHA256SUMS` in v75/volte3 first):
#   MODE=audio   NO RF, NO SOUND. After audio6 MODE=ovl + the ADSP bundle (fresh boot): loads the audio6 module stack
#                with the volte3 q6mvm.ko (mmode1_session parameter).                       PASS = A6L_VOLTE3_AUDIO_READY
#   MODE=status  queries only (modem up): IMSA registration/services, NAS IMS voice support, the modem's VSIDs
#                (NAS 0x007C) and the audio session to use.                                  prints A6L_VOLTE3_AUDIO_SESSION
#   MODE=call    RF: A6L_RF_APPROVED=1 NUM=<own number> A6L_DIAL_TO=<same>. Modem online + registered (volte2-test.sh with
#                KEEP_ONLINE=1). Starts a6l-imsdcm (770) if not running, waits <= WAIT_REG s (180) for IMS registration,
#                picks the MVM session (SESSION=auto|default|volte|cs|mmode1|<hex>), dials with dial-ims (the modem chooses
#                IMS or CS), starts the call audio (audio6 MODE=call, headset, GAIN=0 = 0 dB) as soon as the call is
#                dialling, hangs up after SECS (30). Falls back to a CS call when IMS is not registered.
#                Final line: A6L_VOLTE3_RESULT domain=ims|cs|... session=... registered=... srvcc=...
# Env: SECS=30, WAIT_REG=180, SESSION=auto, A6L_CALL_TYPE=ip (force VOICE_IP), IMSDCM=0 (do not start a6l-imsdcm),
#   A6L_IMSDCM_* (see volte2), L=/tmp/volte3-logs.
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/volte3}; A=${AUDIO:-/tmp/audio6}; Q=$D/a6l-qmi; I=$D/a6l-imsdcm
MODE=${MODE:-status}; SECS=${SECS:-30}; WAIT_REG=${WAIT_REG:-180}; SESSION=${SESSION:-auto}
L=${L:-/tmp/volte3-logs}; mkdir -p $L
PARAM=${PARAM:-/sys/module/q6mvm/parameters/mmode1_session}
chmod 755 $Q $I $D/a6l-q6voiced 2>/dev/null
st() { echo "A6L_VOLTE3_STEP $*"; echo "A6L_VOLTE3_STEP $*" > /dev/kmsg 2>/dev/null; }
q() { "$Q" "$@" 2>&1 | grep -v "^WARNING: linker"; }
mss() { [ -n "${A6L_DRYRUN_MSS:-}" ] && { echo "$A6L_DRYRUN_MSS"; return; }; for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name 2>/dev/null)" = 4080000.remoteproc ] && cat $r/state; done; }
# toybox sha256sum has no --ignore-missing: check the listed files by hand
( cd $D && while read -r h f; do [ -f "$f" ] || { echo "A6L_VOLTE3_HASH missing $f"; exit 1; }; set -- $(sha256sum "$f"); [ "$1" = "$h" ] || { echo "A6L_VOLTE3_HASH mismatch $f"; exit 1; }; done < SHA256SUMS ) || { echo "A6L_VOLTE3_FAIL payload hash"; exit 3; }

audio_load() {
  # audio6's module order, with OUR q6mvm.ko (mmode1_session); audio6 run.sh MODE=call later finds the card and loads nothing
  [ -f $A/modules/order.txt ] || { echo "A6L_VOLTE3_FAIL no $A (push v75/audio6)"; exit 4; }
  [ -n "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-voice 2>/dev/null)" ] || { echo "A6L_VOLTE3_FAIL no voice DT: audio6 MODE=ovl first (fresh boot, before the ADSP bundle)"; exit 6; }
  if grep -q "^q6mvm " /proc/modules; then
    [ -e $PARAM ] && { echo "A6L_VOLTE3_AUDIO_READY (already loaded, param '$(cat $PARAM)')"; return 0; }
    echo "A6L_VOLTE3_FAIL the OLD q6mvm.ko is loaded (audio6 MODE=load ran before): reboot, or use SESSION=default only"; exit 12
  fi
  while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules && continue
    src=$A/modules/$ko; [ "$ko" = q6mvm.ko ] && src=$D/q6mvm.ko
    insmod "$src" || { echo "A6L_VOLTE3_FAIL insmod $ko"; dmesg | tail -n 15; exit 4; }
  done < $A/modules/order.txt
  sleep 8
  [ -e $PARAM ] && echo "A6L_VOLTE3_AUDIO_READY q6mvm mmode1_session='$(cat $PARAM)' card: $(grep -c 'Hisense A6L' /proc/asound/cards)" \
    || { echo "A6L_VOLTE3_FAIL q6mvm param missing"; exit 5; }
  echo "  next: MODE=status (after the modem is up), then Pierre: MODE=call"
}

session_for() {  # $1 = the A6L_VOLTE3_AUDIO_SESSION line; prints the q6mvm value
  case "$1" in *" lte=mmode1 "*) echo default;; *" lte=volte "*) echo volte;; *" lte=cs "*) echo cs;; *" lte=mmode2 "*) echo mmode2;;
    *" lte=hex "*) sed -n 's/.*lte_vsid=0x\([0-9A-Fa-f]\{8\}\).*/\1/p' $L/status.txt | head -n 1;; *) echo default;; esac; }

case "$MODE" in
audio) st "audio load (volte3 q6mvm)"; audio_load; exit 0;;
status) st "status (queries only)"; [ "$(mss)" = running ] || { echo "A6L_VOLTE3_FAIL modem not running ($(mss))"; exit 5; }
  q ims-status | tee $L/status.txt | grep -E "A6L_VOLTE3|FAIL"
  s=$(grep A6L_VOLTE3_AUDIO_SESSION $L/status.txt); echo "A6L_VOLTE3_SESSION_CHOICE $(session_for "$s") (from: $s)"; exit 0;;
call) ;;
*) echo "A6L_VOLTE3_FAIL unknown MODE=$MODE (audio|status|call)"; exit 7;;
esac

# ---------------------------------------------------------------- MODE=call (RF)
[ "${A6L_RF_APPROVED:-0}" = 1 ] || { echo "A6L_VOLTE3_REFUSED needs A6L_RF_APPROVED=1 (Pierre types this command)"; exit 3; }
[ -n "${NUM:-}" ] && [ "${A6L_DIAL_TO:-}" = "$NUM" ] || { echo "A6L_VOLTE3_REFUSED set NUM=<own number> and A6L_DIAL_TO=<same>"; exit 3; }
case "$NUM" in 112|911|999|15|17|18|115|119|110) echo "A6L_VOLTE3_REFUSED emergency number"; exit 3;; esac
[ "$(mss)" = running ] || { echo "A6L_VOLTE3_FAIL modem not running: volte2-test.sh (KEEP_ONLINE=1) first"; exit 5; }
[ -e $PARAM ] || { echo "A6L_VOLTE3_FAIL no q6mvm mmode1_session: run MODE=audio first (audio6 ovl + ADSP before)"; exit 6; }
st "1 serving"; reg=$(q reg | grep A6L_QMI_SERVING); echo "$reg"
case "$reg" in *reg=registered*) ;; *) echo "A6L_VOLTE3_FAIL not registered: run volte2-test.sh with KEEP_ONLINE=1 (or ril-test.sh online)"; exit 5;; esac
# 2. IMS PDN server (770) + wait for registration
if [ "${IMSDCM:-1}" = 1 ] && ! pidof a6l-imsdcm > /dev/null 2>&1; then
  [ -z "${A6L_IMSDCM_MUX:-}" ] && { [ -e /sys/bus/platform/drivers/ipa2-lite/14780000.ipa ] && export A6L_IMSDCM_MUX=9 || export A6L_IMSDCM_MUX=0; }
  st "2 a6l-imsdcm (770, mux=$A6L_IMSDCM_MUX)"; "$I" --duration $((WAIT_REG + SECS + 120)) --imsa > $L/imsdcm.txt 2>&1 &
  sleep 2; grep -E "PUBLISHED|FAIL" $L/imsdcm.txt
else st "2 a6l-imsdcm already running or IMSDCM=0"; fi
st "3 wait for IMS registration (<= $WAIT_REG s)"; t=0; regd=0
while [ $t -le $WAIT_REG ]; do
  q ims-status > $L/status.txt; grep -q "A6L_VOLTE3_IMSA now imsa=present registered=1" $L/status.txt && { regd=1; break; }
  sleep 10; t=$((t+10))
done
grep -E "A6L_VOLTE3" $L/status.txt
if [ $regd = 1 ]; then echo "  IMS registered after ~$t s -> the call should go over IMS (VoLTE)"
else echo "  WARNING: IMS NOT registered -> CS fallback call (CSFB to 3G), domain=cs expected"; fi
# 3. audio session
s=$(grep A6L_VOLTE3_AUDIO_SESSION $L/status.txt)
[ "$SESSION" = auto ] && { [ $regd = 1 ] && SESSION=$(session_for "$s") || SESSION=default; }
echo "$SESSION" > $PARAM; echo "A6L_VOLTE3_SESSION set '$SESSION' -> q6mvm.mmode1_session='$(cat $PARAM)' ($s)"
# 4. dial (background) + call audio as soon as the call is up
st "4 dial-ims $NUM ${SECS}s"; "$Q" dial-ims "$NUM" "$SECS" > $L/dial.txt 2>&1 &
dp=$!; t=0; up=0
while [ $t -lt 20 ]; do sleep 1; t=$((t+1))
  grep -qE "state=(origination|alerting|conversation|cc-in-progress)" $L/dial.txt && { up=1; break; }
  grep -qE "A6L_QMI_DIAL_IMS [^o]|DIAL_ENDED|REFUSED" $L/dial.txt && break
done
grep -E "A6L_QMI_DIAL_IMS|A6L_QMI_CALL_DOMAIN|A6L_QMI_CALL_RAT|A6L_VOLTE3_(IMSA|VSID)" $L/dial.txt | grep -v "^WARNING: linker" | head -20
if [ $up = 1 ]; then
  st "5 call audio (audio6 MODE=call GAIN=0, headset, session '$SESSION')"
  as=$((SECS - 6)); [ $as -lt 4 ] && as=4
  D=$A MODE=call GAIN=0 SECS=$as sh $A/run.sh 2>&1 | grep -v "^WARNING: linker" | tee $L/audio.txt | grep -E "A6L_Q6VOICE|A6L_Q6VOICED|A6L_HW_FAIL|A6L_AU4_CALL|mvm|ASK" | head -30
else echo "A6L_VOLTE3_WARN call never came up: no audio started"; fi
wait $dp 2>/dev/null
st "6 call log"; grep -v "^WARNING: linker" $L/dial.txt | grep -E "A6L_QMI_(CALL_DOMAIN|SRVCC|AUDIO_RAT|VOICE_IND|CALL_RAT|IND_IMSA|IND_SUBINFO|HANGUP|DIAL)|A6L_VOLTE3_CALL_RESULT" | head -60
dmesg | grep -E "A6L_Q6VOICE mvm passive session|mmode1_session" | tail -n 3
res=$(grep A6L_VOLTE3_CALL_RESULT $L/dial.txt | tail -n 1)
dom=$(echo "$res" | sed -n 's/.*domain=\([a-z-]*\).*/\1/p'); srv=$(echo "$res" | sed -n 's/.*srvcc=\([01]\).*/\1/p')
echo "A6L_VOLTE3_RESULT domain=${dom:-none} session=$SESSION registered=$regd srvcc=${srv:-?} audio=ASK-PIERRE (logs $L)"
echo "A6L_VOLTE3_DONE ASK PIERRE: did he hear the voicemail greeting (downlink) / will the message he spoke be on the voicemail (uplink)? any HD (wideband) sound?"
[ "${KEEP_IMSDCM:-0}" = 1 ] || { pkill a6l-imsdcm 2>/dev/null || killall a6l-imsdcm 2>/dev/null; }
exit 0
