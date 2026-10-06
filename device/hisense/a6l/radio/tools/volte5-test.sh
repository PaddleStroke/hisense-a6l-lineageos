#!/system/bin/sh
# volte5 (28 Sep 2026), bundle v75/volte5: IMS registration on Orange FR, modem-centric VoLTE (plan B).
# What is new against volte2/volte4 (see docs/volte5-20260928.md):
#   * the stock qcril binds its IMSA/IMSS clients to the subscription (IMSA 0x0033, IMSS 0x0098) and Android's
#     ImsService switches IMS on through qcril (IMSS Set IMS Service Enable Config 0x008F, ims_service_enabled=1,
#     volte=1). Without an AP IMS stack nobody did this, so IMSA/IMSS answered INVALID_OPERATION and the modem never
#     asked for the IMS PDN. a6l-imsdcm now does both (A6L_IMSDCM_IMSS=enable in MODE=verify).
#   * the IMSDCM 0x2e/0x33/0x34 answers are the stock ones (checked by disassembly), QMI hex is never masked.
# ATTENDED ONLY. RF (online, IMS, dialling) = Pierre types the command (A6L_RF_APPROVED=1). Never dials 112.
# Bundles on the phone (same fresh boot): /tmp/ipa4 (MODE=load FIRST), /tmp/radio2 (A6L_KEEP=1), /tmp/volte5 (this).
# MODE (or $1):
#   check     no RF, no write: bundle hashes, ipa4 bound, modem state, PDC active config, IMS services, IMSS/IMSA read.
#   load      = pdc/volte4-test.sh MODE=load     (PDC Load Config of the pinned Orange MBN, RAM EFS; gates)  [Pierre]
#   activate  = pdc/volte4-test.sh MODE=activate (Set Selected + Activate; PASS restart=yes|no)            [Pierre]
#   verify    RF: Orange active? -> PIN (A6L_PIN) -> a6l-imsdcm (770 + IMSS enable + IMSA watch, stays running)
#             -> online -> wait <= WATCH s for IMS registration -> status. Stays online.   A6L_VOLTE5_RESULT
#   call      RF: NUM=<own number> A6L_DIAL_TO=<same>: waits for registration, dial-ims, SECS (30) then hangs up.
#             Call audio only when AUDIO=1 and the volte3/audio6 audio stack is ready (q6mvm mmode1_session).
#                                                                                           A6L_VOLTE5_CALL_RESULT
#   status    no RF: a6l-imsdcm --status (IMSS read-only) + a6l-qmi ims-status + imsdcm log tail.
#   stop      kill a6l-imsdcm (stops the IMS PDN) + low power.
# Env: WATCH=90 SECS=30 WAIT_REG=120 A6L_IMSDCM_IMSS=enable|read (verify default enable) A6L_IMSDCM_SUB=0
#   A6L_IMSDCM_MUX (default 9 when ipa2-lite is bound, else 0) NO_IPA_OK=1 (run without ipa4) A6L_QMI_LOG=4 (hex)
#   AUDIO=1 AUDIO_DIR=/tmp/audio6 SESSION=auto. Test hooks: A6L_DRYRUN_MSS A6L_V5_BIN A6L_V5_IPA A6L_V5_PROC A6L_T.
export PATH=/tmp/bin:$PATH TMPDIR=/tmp
MODE=${MODE:-${1:-check}}
D=${D:-/tmp/volte5}; BIN=${A6L_V5_BIN:-$D}; Q=$BIN/a6l-qmi; I=$BIN/a6l-imsdcm; P=$BIN/volte-probe
L=${L:-/tmp/volte5-logs}; mkdir -p $L; TS=$(date +%H%M%S); LOG=$L/volte5-$MODE-$TS.txt; : > $LOG
WATCH=${WATCH:-90}; SECS=${SECS:-30}; WAIT_REG=${WAIT_REG:-120}; SESSION=${SESSION:-auto}
PROC=${A6L_V5_PROC:-/proc}; IPA=${A6L_V5_IPA:-/sys/bus/platform/drivers/ipa2-lite/14780000.ipa}
PARAM=${PARAM:-/sys/module/q6mvm/parameters/mmode1_session}; AU=${AUDIO_DIR:-/tmp/audio6}
DLOG=$L/imsdcm.txt; PIDF=$L/imsdcm.pid
chmod 755 $Q $I $P 2>/dev/null

# IMSI (14-15 digits) / ICCID (19-20) masking; QMI hex (msg=0x, tlv=0x, [TT]value, len=N hex) is never touched.
mask() { sed -E '/msg=0x|tlv=0x|\[[0-9A-Fa-f][0-9A-Fa-f]\][0-9A-Fa-f]|len=[0-9]+ [0-9A-Fa-f]/!s/(^|[^0-9A-Za-z])([0-9]{4})[0-9]{8,14}([0-9]{2})([^0-9A-Za-z]|$)/\1\2*****\3\4/g'; }
log() { echo "$*" | mask | tee -a $LOG; }
st() { log "A6L_VOLTE5_STEP $*"; echo "A6L_VOLTE5_STEP $*" > /dev/kmsg 2>/dev/null; }
fail() { log "A6L_VOLTE5_FAIL $*"; log "A6L_VOLTE5_DONE mode=$MODE result=FAIL log=$LOG"; exit 1; }
nap() { sleep "${A6L_T:-$1}"; }
q() { "$Q" "$@" 2>&1 | grep -v "linker" | mask; }
mss() { [ -n "${A6L_DRYRUN_MSS:-}" ] && { echo "$A6L_DRYRUN_MSS"; return; }
  for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat $r/name 2>/dev/null)" = 4080000.remoteproc ] && cat $r/state; done; }
cmdl() { cat "$1/cmdline" 2>/dev/null | tr '\0' ' '; }
dpids() { for p in $PROC/[0-9]*; do case "$(cmdl $p)" in *a6l-imsdcm*--imsa*) echo "${p##*/}";; esac; done; }
ipa_ok() { [ -e "$IPA" ]; }
hashes() { # toybox sha256sum has no --ignore-missing: check the listed files by hand
  ( cd $D && while read -r h f; do [ -f "$f" ] || { echo "missing $f"; exit 1; }; set -- $(sha256sum "$f"); [ "$1" = "$h" ] || { echo "mismatch $f"; exit 1; }; done < SHA256SUMS ); }
pdc_active() { # -> ACTIVE_DESC (volte-probe, read-only; proven on this modem)
  "$P" all 2>&1 | grep -v linker > $L/probe-$1.txt
  a=$(sed -n 's/^A6L_VOLTE_PDC_ACTIVE type=SW id=\([0-9a-f]*\).*/\1/p' $L/probe-$1.txt | head -n 1)
  ACTIVE_DESC=$(grep "PDC_INFO type=SW id=$a " $L/probe-$1.txt | sed -n "s/.*desc='\(.*\)'.*/\1/p" | head -n 1)
  grep -E "IMS_SVC (18|33|770)|NAS_SYSINFO (ims_voice|lte_voice|lte_srv)" $L/probe-$1.txt | mask | sed 's/^/  probe| /' | tee -a $LOG
  log "A6L_VOLTE5_PDC tag=$1 active='${ACTIVE_DESC:-?}' id=${a:-none}"
}
start_daemon() { # $1 = IMSS mode
  if [ -n "$(dpids)" ]; then log "A6L_VOLTE5_NOTE a6l-imsdcm already running pid=$(dpids | tr '\n' ' ')"; return 0; fi
  if [ -z "${A6L_IMSDCM_MUX:-}" ]; then ipa_ok && export A6L_IMSDCM_MUX=9 || export A6L_IMSDCM_MUX=0; fi
  export A6L_IMSDCM_IMSS=$1
  st "a6l-imsdcm --imsa (770 server, IMSS=$1, sub=${A6L_IMSDCM_SUB:-0}, mux=$A6L_IMSDCM_MUX)"
  "$I" --duration ${DAEMON_S:-3600} --imsa > $DLOG 2>&1 &
  echo $! > $PIDF; nap 3
  grep -E "PUBLISHED|FAIL|IMSS|IMSA" $DLOG | mask | sed 's/^/  dcm| /' | tee -a $LOG
  grep -q "A6L_IMSDCM_PUBLISHED" $DLOG || fail "a6l-imsdcm did not publish service 770 (see $DLOG)"
}
wait_reg() { # $1 = seconds; sets REGD=1 when IMSA says registered
  REGD=0; t=0; seen=0
  while [ $t -le $1 ]; do
    n=$(grep -c . $DLOG); [ $n -gt $seen ] && { sed -n "$((seen+1)),${n}p" $DLOG | grep -E "A6L_IMSDCM_(IMSS|IMSA|REQ|PDP|IND|CLIENT)" | mask | sed 's/^/  dcm| /' | tee -a $LOG; seen=$n; }
    grep -q "A6L_IMSDCM_IMSA REGISTERED" $DLOG && { REGD=1; break; }
    nap 5; t=$((t+5))
  done
  log "A6L_VOLTE5_REG registered=$REGD after ${t}s"
}
summary() {
  nreq=$(grep -c "A6L_IMSDCM_REQ" $DLOG); nact=$(grep -c "PDP_ACTIVATE apn=" $DLOG)
  nup=$(grep -c "A6L_IMSDCM_IND PDP_ACTIVATE id=[0-9]* success" $DLOG)
  imss=$(sed -n 's/^A6L_IMSDCM_IMSS RESULT ims_service_enabled=\([^ ]*\) wrote=\([a-z]*\).*/\1,wrote=\2/p' $DLOG | tail -n 1)
  imsa=absent; grep -q "IMSA PRESENT" $DLOG && imsa=present
  bound=no; grep -q "A6L_IMSDCM_IMSA bind sub=[0-9]*: ok" $DLOG && bound=yes
  regd=no; grep -q "A6L_IMSDCM_IMSA REGISTERED" $DLOG && regd=yes
  voice=$(grep -E "A6L_IMSDCM_IMSA(_IND|_POLL)? .*services|A6L_IMSDCM_IMSA services" $DLOG | tail -n 1 | sed -n 's/.*voice=\([a-z]*\).*/\1/p')
  dom=$(grep "A6L_VOLTE3_NAS" $L/ims-status.txt 2>/dev/null | tail -n 1 | sed -n 's/.*lte_voice_domain=\([0-9-]*\).*/\1/p')
  log "A6L_VOLTE5_RESULT imss=${imss:-none} imsa=$imsa bound=$bound dcm_requests=$nreq pdp_activate=$nact pdp_up=$nup registered=$regd voice=${voice:--} lte_voice_domain=${dom:-?} (logs $L)"
}

log "A6L_VOLTE5_START mode=$MODE $(date) modem=$(mss) ipa=$(ipa_ok && echo bound || echo absent) log=$LOG"
case "$MODE" in
check|status|verify|call|stop)
  r=$(hashes) || fail "bundle hash: $r (sha256sum -c SHA256SUMS on the laptop, push again)";;
esac
case "$MODE" in
load|activate)
  [ -f $D/pdc/volte4-test.sh ] || fail "no $D/pdc/volte4-test.sh"
  [ -z "$(dpids)" ] || fail "a6l-imsdcm is running: the PDC gate refuses other QMI clients (MODE=stop first)"
  st "pdc $MODE (volte4 script, gates inside)"
  D=$D/pdc VOLTE2=$D MODE=$MODE sh $D/pdc/volte4-test.sh 2>&1 | grep -v linker | tee -a $LOG
  r=$(grep -E "A6L_VOLTE4_(LOAD_PASS|LOAD_SKIP|ACTIVATE_PASS|ACTIVATE_SKIP|ABORT|FAIL)" $LOG | tail -n 1)
  case "$r" in *_PASS*|*_SKIP*) log "A6L_VOLTE5_PDC_${MODE}_OK ${r#A6L_VOLTE4_}";; *) fail "pdc $MODE: ${r:-no result line}";; esac;;
check)
  st "check (no RF, no write)"
  log "A6L_VOLTE5_IPA $(ipa_ok && echo "bound ($IPA)" || echo "ABSENT: run ipa4 MODE=load before radio2 (fresh boot)") modules: $(grep -c '^ipa2_lite ' /proc/modules 2>/dev/null)"
  [ "$(mss)" = running ] || fail "modem not running ($(mss)): radio2 A6L_KEEP=1 first"
  pdc_active check
  A6L_IMSDCM_IMSS=read "$I" --status 2>&1 | grep -v linker | mask | tee $L/status-check.txt | tee -a $LOG
  log "A6L_VOLTE5_CHECK_PASS modem=running ipa=$(ipa_ok && echo bound || echo absent) active='${ACTIVE_DESC:-?}'";;
status)
  st "status (queries only)"
  [ "$(mss)" = running ] || fail "modem not running ($(mss))"
  A6L_IMSDCM_IMSS=read "$I" --status 2>&1 | grep -v linker | mask | tee $L/status.txt | tee -a $LOG
  q ims-status | tee $L/ims-status.txt | grep -E "A6L_VOLTE3_(IMSA|NAS)" | tee -a $LOG
  [ -s $DLOG ] && { grep -E "A6L_IMSDCM_(IMSS|IMSA|PDP|IND)" $DLOG | tail -n 12 | mask | sed 's/^/  dcm| /' | tee -a $LOG; summary; };;
verify)
  [ "${A6L_RF_APPROVED:-0}" = 1 ] || fail "needs A6L_RF_APPROVED=1 (Pierre types this command)"
  [ "$(mss)" = running ] || fail "modem not running ($(mss)): radio2 A6L_KEEP=1 first"
  if ipa_ok; then log "A6L_VOLTE5_IPA bound"; else [ "${NO_IPA_OK:-0}" = 1 ] || fail "ipa4 not bound: fresh boot, ipa4 MODE=load BEFORE radio2 (or NO_IPA_OK=1)"; log "A6L_VOLTE5_IPA absent (NO_IPA_OK=1)"; fi
  st "1 PDC active config"; pdc_active pre
  [ "$ACTIVE_DESC" = France-Commercial-Orange ] || fail "active SW config is '${ACTIVE_DESC:-?}', not France-Commercial-Orange: MODE=load then MODE=activate"
  if [ -n "${A6L_PIN:-}" ]; then st "2 SIM PIN"; q pin "$A6L_PIN" | grep -E "VERIFY_PIN|FAIL|REFUSED" | tee -a $LOG; else st "2 no A6L_PIN (SIM without PIN or already verified)"; fi
  start_daemon ${A6L_IMSDCM_IMSS:-enable}
  st "3 online"; q mode online | grep -E "SET_OPMODE|REFUSED" | tee -a $LOG
  n=0; reg=""; while [ $n -lt 12 ]; do reg=$(q reg | grep "A6L_QMI_SERVING"); case "$reg" in *reg=registered*" ps=1 "*) break;; esac; nap 5; n=$((n+1)); done
  log "$reg"; case "$reg" in *reg=registered*" ps=1 "*) log "  registered, PS attached after $((n*5)) s";; *) log "  WARNING: not PS-attached after 60 s (continuing)";; esac
  st "4 wait for IMS registration (<= $WATCH s)"; wait_reg $WATCH
  st "5 status"
  q ims-status | tee $L/ims-status.txt | grep -E "A6L_VOLTE3_(IMSA|NAS|AUDIO_SESSION)" | tee -a $LOG
  "$P" all 2>&1 | grep -v linker > $L/probe-after.txt
  grep -E "IMS_SVC (18|33|770)|NAS_SYSINFO (ims_voice|lte_voice)|IMSA_REG|IMSS_" $L/probe-after.txt | mask | sed 's/^/  probe| /' | tee -a $LOG
  summary
  log "A6L_VOLTE5_NOTE a6l-imsdcm keeps running (IMS PDN up); next: MODE=call, or MODE=stop";;
call)
  [ "${A6L_RF_APPROVED:-0}" = 1 ] || fail "needs A6L_RF_APPROVED=1 (Pierre types this command)"
  [ -n "${NUM:-}" ] && [ "${A6L_DIAL_TO:-}" = "$NUM" ] || fail "set NUM=<own number> and A6L_DIAL_TO=<same>"
  case "$NUM" in 112|911|999|15|17|18|115|119|110) fail "emergency number refused";; esac
  [ "$(mss)" = running ] || fail "modem not running ($(mss))"
  [ -n "$(dpids)" ] || { log "A6L_VOLTE5_NOTE a6l-imsdcm not running: starting it (IMSS=${A6L_IMSDCM_IMSS:-enable})"; start_daemon ${A6L_IMSDCM_IMSS:-enable}; }
  st "1 serving"; reg=$(q reg | grep A6L_QMI_SERVING); log "$reg"
  case "$reg" in *reg=registered*) ;; *) fail "not registered on the network: MODE=verify first";; esac
  st "2 IMS registration (<= $WAIT_REG s)"; wait_reg $WAIT_REG
  [ $REGD = 1 ] || log "A6L_VOLTE5_WARN IMS NOT registered -> the modem will use CS fallback (domain=cs)"
  au=0
  if [ "${AUDIO:-0}" = 1 ] && [ -e $PARAM ] && [ -f $AU/run.sh ]; then
    q ims-status > $L/ims-status.txt; s=$(grep A6L_VOLTE3_AUDIO_SESSION $L/ims-status.txt)
    if [ "$SESSION" = auto ]; then case "$s" in *" lte=volte "*) SESSION=volte;; *" lte=cs "*) SESSION=cs;; *" lte=mmode2 "*) SESSION=mmode2;; *) SESSION=default;; esac; [ $REGD = 1 ] || SESSION=default; fi
    echo "$SESSION" > $PARAM; au=1; log "A6L_VOLTE5_AUDIO session='$(cat $PARAM)' ($s)"
  else log "A6L_VOLTE5_AUDIO off (AUDIO=1 needs audio6 ovl + ADSP + volte3 MODE=audio first): signalling-only call, no sound on the phone"; fi
  st "3 dial-ims $NUM ${SECS}s"; "$Q" dial-ims "$NUM" "$SECS" > $L/dial.txt 2>&1 &
  dp=$!; t=0; up=0
  while [ $t -lt 20 ]; do sleep 1; t=$((t+1))
    grep -qE "state=(origination|alerting|conversation|cc-in-progress)" $L/dial.txt && { up=1; break; }
    grep -qE "A6L_QMI_DIAL_IMS [^o]|DIAL_ENDED|REFUSED" $L/dial.txt && break; done
  if [ $up = 1 ] && [ $au = 1 ]; then
    st "4 call audio (audio6 MODE=call GAIN=0, session $SESSION)"; as=$((SECS - 6)); [ $as -lt 4 ] && as=4
    D=$AU MODE=call GAIN=0 SECS=$as sh $AU/run.sh 2>&1 | grep -v linker | grep -E "A6L_Q6VOICE|A6L_HW_FAIL|A6L_AU4_CALL|ASK" | head -20 | tee -a $LOG
  fi
  wait $dp 2>/dev/null
  st "5 call log"; grep -v linker $L/dial.txt | grep -E "A6L_QMI_(CALL_DOMAIN|SRVCC|AUDIO_RAT|CALL_RAT|IND_IMSA|HANGUP|DIAL)|A6L_VOLTE3_(CALL_RESULT|IMSA)" | head -40 | mask | tee -a $LOG
  res=$(grep A6L_VOLTE3_CALL_RESULT $L/dial.txt | tail -n 1)
  dom=$(echo "$res" | sed -n 's/.*domain=\([a-z-]*\).*/\1/p'); srv=$(echo "$res" | sed -n 's/.*srvcc=\([01]\).*/\1/p')
  log "A6L_VOLTE5_CALL_RESULT domain=${dom:-none} registered=$REGD srvcc=${srv:-?} audio=$([ $au = 1 ] && echo ASK-PIERRE || echo off) (logs $L)";;
stop)
  st "stop"; for p in $(dpids); do kill $p 2>/dev/null; done; nap 3
  [ -s $DLOG ] && { grep -E "A6L_IMSDCM_(PDP|SUMMARY|DONE)" $DLOG | tail -n 6 | mask | tee -a $LOG; summary; }
  q mode lowpower | grep SET_OPMODE | tee -a $LOG;;
*) log "A6L_VOLTE5_FAIL unknown MODE=$MODE (check|load|activate|verify|call|status|stop)"; exit 2;;
esac
log "A6L_VOLTE5_DONE mode=$MODE result=OK log=$LOG"
