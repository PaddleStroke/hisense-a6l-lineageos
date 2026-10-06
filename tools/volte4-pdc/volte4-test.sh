#!/system/bin/sh
# volte4 (27 Sep 2026), bundle v75/volte4: load + activate the OFFICIAL stock "France-Commercial-Orange" SW MBN
# (from this phone's own NON-HLOS: MCFG_SW/GENERIC/EU/ORANGE/COMMERCI/FRANCE/MCFG_SW.MBN) with the modem's standard
# PDC Load Config / Set Selected Config / Activate Config, using the off-the-shelf libqmi qmicli 1.36.0
# (Alpine v3.22 aarch64 packages, run by the bundled musl loader; see q/SOURCES.txt). No custom QMI writer.
# ATTENDED ONLY. The modem EFS lives in RAM copies (rmtfs -o /tmp/rmtfs): nothing reaches modemst1/modemst2,
# and a reboot restores the original configuration (ROW_Commercial).
#
# usage (laptop ~/A6L-usb-20260915, after `cd v75/volte4 && sha256sum -c SHA256SUMS`, after radio2 KEEP):
#   adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH; export MODE=ro; sh /tmp/volte4/volte4-test.sh'
# MODE (or $1):
#   ro        read-only: PDC list (qmicli) + PDC active/list (volte-probe cross-check). No write.
#   load      PDC Load Config of the pinned Orange MBN (EFS write -> RAM copy). No reset.
#   activate  PDC Set Selected + Activate (qmicli --pdc-activate-config) -> modem restarts (SSR); waits for
#             remoteproc running + PDC back, checks rmtfs/tqftpserv/diag-router, watches 60 s for new crashes.
#   verify    Orange active? IMS services published? Then chains /tmp/volte2/volte2-test.sh (KEEP_ONLINE=1,
#             SKIP_RADIO=1) unless NO_VOLTE2=1. RF: needs A6L_RF_APPROVED=1 (and A6L_PIN for the SIM).
#   revert    Set Selected + Activate ROW_Commercial (id 7af52eaa...) -> modem restarts. (Or simply reboot.)
# SAFETY GATES (load/activate/revert abort before any write unless ALL hold):
#   A6L_RF_APPROVED=1; bundle SHA256SUMS ok; MBN sha256+size = pinned; modem remoteproc running;
#   exactly one rmtfs, started with "-o /tmp/rmtfs"; /tmp/rmtfs/modem_fs1+modem_fs2 present, sizes = partitions;
#   no process has the real modemst1/modemst2/fsg/fsc open; no other QMI client running (a6l-qmi, a6l-imsdcm,
#   volte-probe, another qmicli, qcrild/rild/radio HAL).
# PASS markers: A6L_VOLTE4_RO_PASS, A6L_VOLTE4_LOAD_PASS (or LOAD_SKIP), A6L_VOLTE4_ACTIVATE_PASS (or ACTIVATE_SKIP),
#   A6L_VOLTE4_VERIFY_PASS + A6L_VOLTE4_VOLTE2_RESULT, A6L_VOLTE4_REVERT_PASS. Failures: A6L_VOLTE4_GATE_FAIL /
#   A6L_VOLTE4_ABORT (nothing written) / A6L_VOLTE4_FAIL. Logs: /tmp/volte4-logs/. IMSI/ICCID-like digit runs are masked
#   (never QMI hex). volte5 (28 Sep): ACTIVATE_PASS ... restart=yes|no (this modem applied Orange without an SSR on 28 Sep).
# Env: D=/tmp/volte4 RADIO=/tmp/radio2 VOLTE2=/tmp/volte2 WATCH_AFTER=60 READ_REAL=1 NO_VOLTE2=0 MANUAL_RESTART=0
#   REVERT_ID=<hex> (default ROW id). Test hooks (never needed on the phone): A6L_PROC A6L_RPROC A6L_BLOCK
#   A6L_RMTFS_DIR A6L_QMICLI A6L_KMSG A6L_T (sleep scale).
export PATH=/tmp/bin:$PATH TMPDIR=/tmp
MODE=${MODE:-${1:-ro}}
D=${D:-/tmp/volte4}; RADIO=${RADIO:-/tmp/radio2}; V2=${VOLTE2:-/tmp/volte2}; L=${L:-/tmp/volte4-logs}
PROC=${A6L_PROC:-/proc}; RP=${A6L_RPROC:-/sys/class/remoteproc}; BLK=${A6L_BLOCK:-/sys/class/block}
RMTFS_DIR=${A6L_RMTFS_DIR:-/tmp/rmtfs}; KMSG=${A6L_KMSG:-/dev/kmsg}
QMICLI=${A6L_QMICLI:-$D/qmicli}; DEV=qrtr://0; PROBE=$V2/volte-probe
MBN=$D/mbn/France-Commercial-Orange.mbn
PIN_SHA256=336ac0f81cf6daa6e1424331a8d2f8d4e0030daeb24b89d3b15de54e6e003c4c
PIN_SHA1=965d8d766f9ac0e698ca5d4433395a31526d2d8d   # = the config id qmicli --pdc-load-config gives it (SHA-1 of the file)
PIN_SIZE=50348; ORANGE_DESC=France-Commercial-Orange
ROW_ID=7af52eaad094b6617fd163ca7370863364601f22; ROW_DESC=ROW_Commercial
WATCH_AFTER=${WATCH_AFTER:-60}
chmod 755 "$D/qmicli" "$D/q/ld-musl-aarch64.so.1" "$D/q/qmicli.bin" 2>/dev/null   # adb push/scp may drop +x
mkdir -p "$L"; TS=$(date +%H%M%S); LOG=$L/volte4-$MODE-$TS.txt; : > "$LOG"
MARK="A6L_VOLTE4_${MODE}_$$_$TS"

# IMSI (14-15 digits) / ICCID (19-20) masking. volte5 (28 Sep): QMI hex is never masked: lines carrying a hex dump
# (msg=0x / tlv=0x / [TT]value / len=N hex) are left alone, and only decimal runs of 14-20 digits are touched
# (the old {7,} rule turned "[01]0000000000000000" into "[01]0000*****00").
mask() { sed -E '/msg=0x|tlv=0x|\[[0-9A-Fa-f][0-9A-Fa-f]\][0-9A-Fa-f]|len=[0-9]+ [0-9A-Fa-f]/!s/(^|[^0-9A-Za-z])([0-9]{4})[0-9]{8,14}([0-9]{2})([^0-9A-Za-z]|$)/\1\2*****\3\4/g'; }
log() { echo "$*" | mask | tee -a "$LOG"; }
kmsg() { echo "$*" >> "$KMSG" 2>/dev/null; }
step() { log "A6L_VOLTE4_STEP $*"; kmsg "A6L_VOLTE4_STEP $*"; }
fail() { log "A6L_VOLTE4_FAIL $*"; log "A6L_VOLTE4_DONE mode=$MODE result=FAIL log=$LOG"; exit 1; }
nap() { sleep "${A6L_T:-$1}"; }
alive() { s=$(cut -d' ' -f3 /proc/$1/stat 2>/dev/null); [ -n "$s" ] && [ "$s" != Z ]; }
cmdl() { cat "$1/cmdline" 2>/dev/null | tr '\0' ' '; }   # volte5: no "can't open .../cmdline" noise for exited pids
argv0() { set -- $(cmdl "$1"); [ -n "${1:-}" ] && echo "${1##*/}"; }
# pids whose argv[0] basename is $1 (in $PROC)
pids_named() { for p in $PROC/[0-9]*; do [ "$(argv0 $p)" = "$1" ] && echo "${p##*/}"; done; }
mssdir() { for r in $RP/remoteproc*; do case "$(cat $r/name 2>/dev/null)" in *4080000*|mss|modem) echo "$r"; return;; esac; done; }
mstate() { M=$(mssdir); [ -n "$M" ] && cat "$M/state" 2>/dev/null || echo none; }
klog() { dmesg 2>/dev/null | sed -n "/$MARK/,\$p"; }

# ---- qmicli runner (own timeout; no dependency on a timeout applet) ----
qrun() { # qrun <timeout_s> <outfile> <qmicli args...>
  t=$1; o=$2; shift 2
  log "A6L_VOLTE4_CMD qmicli -d $DEV $*"
  "$QMICLI" -d "$DEV" "$@" > "$o" 2>&1 &
  qp=$!; n=0
  while alive $qp && [ $n -lt $t ]; do sleep 1; n=$((n+1)); done
  if alive $qp; then kill $qp 2>/dev/null; sleep 1; kill -9 $qp 2>/dev/null; wait $qp 2>/dev/null; QRC=124; echo "(timeout after ${t}s)" >> "$o"
  else wait $qp; QRC=$?; fi
  mask < "$o" | sed 's/^/  | /' | tee -a "$LOG"
  log "A6L_VOLTE4_QMICLI_RC $QRC (${n}s)"
}

# ---- PDC state: qmicli list (primary) + volte-probe (cross-check, proven on this modem 27 Sep) ----
parse_list() { # qmicli --pdc-list-configs output -> desc|size|status|version|id
  desc=; size=; st=; ver=; id=; any=0
  while IFS= read -r line; do
    line=$(echo "$line" | tr -d '\t\r')
    case "$line" in
      Configuration*) [ $any = 1 ] && echo "$desc|$size|$st|$ver|$id"; desc=; size=; st=; ver=; id=; any=1;;
      Description:*) desc=$(echo "${line#Description:}" | sed 's/^ *//; s/ *$//');;
      Size:*) size=$(echo "${line#Size:}" | tr -d ' ');;
      Status:*) st=$(echo "${line#Status:}" | tr -d ' ');;
      Version:*) ver=$(echo "${line#Version:}" | tr -d ' ');;
      ID:*) id=$(echo "${line#ID:}" | tr -d ' :' | tr 'A-F' 'a-f');;
    esac
  done < "$1"
  [ $any = 1 ] && echo "$desc|$size|$st|$ver|$id"
}
pdc_state() { # $1 = tag. Sets ACTIVE_ID ACTIVE_DESC ORANGE_ID ROW_SEEN PENDING_ID Q_TOTAL
  ACTIVE_ID=; ACTIVE_DESC=; ORANGE_ID=; ROW_SEEN=0; PENDING_ID=; Q_TOTAL=
  qrun 40 "$L/list-$1.raw" --pdc-list-configs=software
  Q_TOTAL=$(sed -n 's/^Total configurations: *\([0-9]*\).*/\1/p' "$L/list-$1.raw" | head -n 1)
  parse_list "$L/list-$1.raw" > "$L/cfg-$1.txt"
  while IFS='|' read -r c_desc c_size c_st c_ver c_id; do
    log "A6L_VOLTE4_CFG src=qmicli desc='$c_desc' size=$c_size status=$c_st version=$c_ver id=$c_id"
    [ "$c_st" = Active ] && { ACTIVE_ID=$c_id; ACTIVE_DESC=$c_desc; }
    [ "$c_st" = Pending ] && PENDING_ID=$c_id
    [ "$c_desc" = "$ORANGE_DESC" ] && [ "$c_size" = "$PIN_SIZE" ] && ORANGE_ID=$c_id
    [ "$c_id" = "$ROW_ID" ] && ROW_SEEN=1
  done < "$L/cfg-$1.txt"
  if [ -x "$PROBE" ]; then
    "$PROBE" all 2>&1 | grep -v "^WARNING: linker" | grep -E "A6L_VOLTE_(PDC|IMS_SVC)" > "$L/probe-$1.txt"
    mask < "$L/probe-$1.txt" | grep -E "PDC_(ACTIVE|INFO|LIST)|IMS_SVC 33" | sed 's/^/  probe| /' | tee -a "$LOG"
    p_act=$(sed -n 's/^A6L_VOLTE_PDC_ACTIVE type=SW id=\([0-9a-f]*\).*/\1/p' "$L/probe-$1.txt" | head -n 1)
    p_or=$(grep "desc='$ORANGE_DESC'" "$L/probe-$1.txt" | sed -n 's/.* id=\([0-9a-f]*\) .*/\1/p' | head -n 1)
    grep -q "id=$ROW_ID " "$L/probe-$1.txt" && ROW_SEEN=1
    if [ -z "$ACTIVE_ID" ] && [ -n "$p_act" ]; then ACTIVE_ID=$p_act
      ACTIVE_DESC=$(grep "PDC_INFO type=SW id=$p_act " "$L/probe-$1.txt" | sed -n "s/.*desc='\(.*\)'.*/\1/p" | head -n 1); fi
    [ -n "$p_act" ] && [ -n "$ACTIVE_ID" ] && [ "$p_act" != "$ACTIVE_ID" ] && log "A6L_VOLTE4_WARN qmicli active=$ACTIVE_ID but volte-probe active=$p_act"
    [ -z "$ORANGE_ID" ] && [ -n "$p_or" ] && ORANGE_ID=$p_or
  else log "A6L_VOLTE4_NOTE no $PROBE (cross-check skipped)"; fi
  log "A6L_VOLTE4_STATE tag=$1 qmicli_total=${Q_TOTAL:-?} active='${ACTIVE_DESC:-?}' active_id=${ACTIVE_ID:-none} pending_id=${PENDING_ID:-none} orange_id=${ORANGE_ID:-not-loaded} row_listed=$ROW_SEEN"
}

# ---- EFS evidence: RAM copies change, real partitions do not ----
partdev() { for u in $BLK/*/uevent; do grep -q "^PARTNAME=$1$" "$u" 2>/dev/null && { echo "${u%/uevent}"; return; }; done; }
efs_sha() { # $1 tag
  for f in modem_fs1 modem_fs2; do log "A6L_VOLTE4_EFS tag=$1 ram/$f sha256=$(sha256sum "$RMTFS_DIR/$f" 2>/dev/null | cut -c1-16)"; done
  [ "${READ_REAL:-1}" = 1 ] || return 0
  for part in modemst1 modemst2; do
    dv=$(partdev $part); [ -n "$dv" ] || { log "A6L_VOLTE4_EFS tag=$1 real/$part not-found"; continue; }
    mm=$(cat "$dv/dev"); n=/dev/a6l-v4ro-$part
    [ -e $n ] || mknod $n b ${mm%%:*} ${mm##*:} 2>/dev/null
    log "A6L_VOLTE4_EFS tag=$1 real/$part sha256=$(dd if=$n bs=1M 2>/dev/null | sha256sum | cut -c1-16) (read-only)"
    rm -f $n
  done
}

# ---- gates ----
GATE_OK=1
gfail() { log "A6L_VOLTE4_GATE_FAIL $*"; GATE_OK=0; }
common_gates() {
  ( cd "$D" && sha256sum -c SHA256SUMS > "$L/sha-check.txt" 2>&1 ) || gfail "bundle SHA256SUMS (see $L/sha-check.txt)"
  [ "$(mstate)" = running ] || gfail "modem remoteproc not running ($(mstate)); run radio2 with A6L_KEEP=1 first"
}
write_gates() {
  [ "${A6L_RF_APPROVED:-0}" = 1 ] || gfail "A6L_RF_APPROVED!=1 (Pierre types this command)"
  s=$(sha256sum "$MBN" 2>/dev/null | cut -c1-64); [ "$s" = "$PIN_SHA256" ] || gfail "MBN sha256 '$s' != pinned"
  sz=$(wc -c < "$MBN" 2>/dev/null | tr -d ' '); [ "$sz" = "$PIN_SIZE" ] || gfail "MBN size '$sz' != $PIN_SIZE"
  # rmtfs: exactly one, RAM mode
  rp=$(pids_named rmtfs); nr=0; for p in $rp; do nr=$((nr+1)); c=" $(cmdl $PROC/$p) "
    case "$c" in *" -o $RMTFS_DIR "*|*" -o $RMTFS_DIR/ "*) log "A6L_VOLTE4_GATE rmtfs pid=$p RAM mode ($c)";; *) gfail "rmtfs pid=$p NOT in RAM mode: $c";; esac; done
  [ $nr = 1 ] || gfail "expected exactly 1 rmtfs process, found $nr"
  for pair in modemst1:modem_fs1 modemst2:modem_fs2; do part=${pair%%:*}; f=$RMTFS_DIR/${pair##*:}
    [ -s "$f" ] || { gfail "$f missing/empty"; continue; }
    dv=$(partdev $part); fs=$(wc -c < "$f" | tr -d ' ')
    if [ -n "$dv" ]; then ps=$(( $(cat "$dv/size") * 512 )); [ "$fs" = "$ps" ] || gfail "$f size $fs != $part $ps"
    else gfail "partition $part not visible (sdhci-msm?)"; fi
    log "A6L_VOLTE4_GATE ram copy $f bytes=$fs"
  done
  # nobody holds the real EFS partitions
  pat=""; for part in modemst1 modemst2 fsg fsc; do dv=$(partdev $part); [ -n "$dv" ] && pat="$pat ${dv##*/} $part"; done
  for fd in $PROC/[0-9]*/fd/*; do l=$(readlink "$fd" 2>/dev/null) || continue
    for w in $pat; do case "$l" in */$w) gfail "real partition open: $fd -> $l";; esac; done; done
  # no other QMI client (the modem's PDC must see only us)
  for n in a6l-qmi a6l-imsdcm volte-probe qcrild rild a6l-radio-hal android.hardware.radio-service.a6l; do
    for p in $(pids_named $n); do gfail "other QMI client running: $n pid=$p"; done; done
  for p in $PROC/[0-9]*; do case "$(cmdl $p)" in *qmicli.bin*) gfail "another qmicli running: pid=${p##*/}";; esac; done
}
gates() { # $1 = ro|write
  GATE_OK=1; common_gates; [ "$1" = write ] && write_gates
  if [ $GATE_OK = 1 ]; then log "A6L_VOLTE4_GATES_PASS ($1)"
  else log "A6L_VOLTE4_ABORT gates failed: nothing was written"; log "A6L_VOLTE4_DONE mode=$MODE result=ABORT log=$LOG"; exit 20; fi
}

# ---- modem restart after Activate ----
restart_diag() {
  LD_LIBRARY_PATH=$RADIO/bin A6L_DIAG_EDGES=modem A6L_DIAG_QRTR=modem $RADIO/bin/diag-router >> /tmp/diag-router.log 2>&1 &
  log "A6L_VOLTE4_DIAG_RESTARTED pid=$!"
}
wait_modem_back() { # after activate: see it go down, come back running, PDC answering, no new crash
  M=$(mssdir); seen=0; t=0
  while [ $t -lt 45 ]; do s=$(mstate); c=$(klog | grep -cE "fatal error received|crash detected|recovering|is now up|stopped remote processor")
    [ "$s" != running ] || [ "$c" -gt 0 ] && { seen=1; break; }; nap 1; t=$((t+1)); done
  log "A6L_VOLTE4_SSR seen=$seen after ${t}s state=$(mstate) recovery=$(cat $M/recovery 2>/dev/null)"
  klog | grep -iE "fatal error|crash|recover|mcfg|q6v5|remoteproc" | head -n 12 | mask | sed 's/^/  klog| /' | tee -a "$LOG"
  if [ $seen = 0 ]; then
    if [ "${MANUAL_RESTART:-0}" = 1 ]; then log "A6L_VOLTE4_NOTE modem did not restart by itself: MANUAL_RESTART=1 -> remoteproc stop/start"
      echo stop > "$M/state" 2>/dev/null; nap 3; echo start > "$M/state" 2>/dev/null
    else log "A6L_VOLTE4_NOTE modem did not restart within 45 s (28 Sep: this modem applies the SW config without an SSR)"
      nr_watch; return $?; fi
  fi
  t=0; stuck=0
  while [ $t -lt 180 ]; do s=$(mstate); [ "$s" = running ] && break
    case "$s" in crashed) stuck=$((stuck+1)); [ $stuck = 20 ] && { log "A6L_VOLTE4_NOTE crashed for 20 s -> recovery 'recover'"; echo recover > "$M/recovery" 2>/dev/null; };;
                 offline) stuck=$((stuck+1)); [ $stuck = 20 ] && { log "A6L_VOLTE4_NOTE offline for 20 s -> remoteproc start"; echo start > "$M/state" 2>/dev/null; };; esac
    nap 1; t=$((t+1)); done
  log "A6L_VOLTE4_MODEM state=$(mstate) after ${t}s"
  [ "$(mstate)" = running ] || return 1
  for n in rmtfs tqftpserv diag-router; do p=$(pids_named $n | head -n 1); log "A6L_VOLTE4_DAEMON $n pid=${p:-DEAD}"
    [ -z "$p" ] && case $n in diag-router) restart_diag;; *) log "A6L_VOLTE4_FAIL_DAEMON $n died: stop here, reboot the phone (the RAM EFS copy is discarded)"; return 1;; esac; done
  t=0; while [ $t -lt 90 ]; do "$QMICLI" -d $DEV --pdc-noop > /dev/null 2>&1 && break; nap 3; t=$((t+3)); done
  log "A6L_VOLTE4_PDC_BACK after ${t}s"
  c0=$(klog | grep -c "fatal error received"); t=0
  while [ $t -lt $WATCH_AFTER ]; do nap 5; t=$((t+5)); done
  c1=$(klog | grep -c "fatal error received")
  log "A6L_VOLTE4_WATCH ${WATCH_AFTER}s after restart: new_fatal=$((c1 - c0)) state=$(mstate)"
  [ "$(mstate)" = running ] && [ $c1 = $c0 ]
}
nr_watch() { # volte5: no SSR happened. rc 2 = modem running, daemons alive, no new fatal for WATCH_AFTER s; rc 1 = not clean
  c0=$(klog | grep -c "fatal error received"); t=0
  while [ $t -lt $WATCH_AFTER ]; do nap 5; t=$((t+5)); done
  c1=$(klog | grep -c "fatal error received")
  for n in rmtfs tqftpserv; do p=$(pids_named $n | head -n 1); log "A6L_VOLTE4_DAEMON $n pid=${p:-DEAD}"; [ -n "$p" ] || return 1; done
  log "A6L_VOLTE4_WATCH ${WATCH_AFTER}s (no restart): new_fatal=$((c1 - c0)) state=$(mstate)"
  [ "$(mstate)" = running ] && [ $c1 = $c0 ] && return 2
  return 1
}
do_activate() { # $1 id  $2 label
  kmsg "$MARK"; efs_sha pre-$2
  qrun 60 "$L/activate-$2.raw" --pdc-activate-config=software,$1
  grep -q "error" "$L/activate-$2.raw" && log "A6L_VOLTE4_NOTE qmicli reported an error (see above)"
  wait_modem_back; WB=$?
  pdc_state post-$2; efs_sha post-$2
  return $WB
}

log "A6L_VOLTE4_START mode=$MODE $(date) modem=$(mstate) log=$LOG"
case "$MODE" in
ro)
  gates ro; step "ro: PDC list"
  pdc_state ro
  [ -n "$ACTIVE_ID" ] || fail "no active SW config read (qmicli total=${Q_TOTAL:-?}); PDC reads failed"
  log "A6L_VOLTE4_RO_PASS active='$ACTIVE_DESC' orange_loaded=$([ -n "$ORANGE_ID" ] && echo yes || echo no)";;
load)
  gates write; step "load: pre-state"
  pdc_state pre-load
  [ -n "$ACTIVE_ID" ] || fail "PDC state unreadable before load; not loading"
  if [ -n "$ORANGE_ID" ]; then log "A6L_VOLTE4_LOAD_SKIP '$ORANGE_DESC' already loaded id=$ORANGE_ID"
  else
    efs_sha pre-load; step "load: qmicli --pdc-load-config"
    qrun 180 "$L/load.raw" --pdc-load-config="$MBN"
    grep -q "Finished loading" "$L/load.raw" || fail "load did not finish (qmicli rc=$QRC; see $L/load.raw)"
    nap 2; pdc_state post-load; efs_sha post-load
    [ -n "$ORANGE_ID" ] || fail "loaded but '$ORANGE_DESC' not listed"
    [ "$ORANGE_ID" = "$PIN_SHA1" ] || log "A6L_VOLTE4_NOTE orange id $ORANGE_ID != sha1 $PIN_SHA1 (modem-assigned id; fine)"
    log "A6L_VOLTE4_LOAD_PASS id=$ORANGE_ID active_still='$ACTIVE_DESC'"
  fi;;
activate)
  gates write; step "activate: pre-state"
  pdc_state pre-act
  [ -n "$ORANGE_ID" ] || fail "'$ORANGE_DESC' is not loaded: run MODE=load first"
  if [ "$ACTIVE_ID" = "$ORANGE_ID" ]; then log "A6L_VOLTE4_ACTIVATE_SKIP already active id=$ORANGE_ID"
  else
    step "activate: select+activate $ORANGE_ID (modem will restart)"
    do_activate "$ORANGE_ID" orange; rc=$?
    [ "$ACTIVE_ID" = "$ORANGE_ID" ] || fail "after activation active='${ACTIVE_DESC:-?}' id=${ACTIVE_ID:-none} (pending=${PENDING_ID:-none})"
    case $rc in
      0) log "A6L_VOLTE4_ACTIVATE_PASS active='$ACTIVE_DESC' id=$ACTIVE_ID restart=yes";;
      2) log "A6L_VOLTE4_ACTIVATE_PASS active='$ACTIVE_DESC' id=$ACTIVE_ID restart=no (applied in place; modem stable ${WATCH_AFTER}s)";;
      *) fail "Orange is active but the modem did not come back cleanly (see A6L_VOLTE4_MODEM/WATCH/DAEMON lines)";;
    esac
  fi;;
verify)
  gates ro; step "verify: PDC"
  pdc_state verify
  [ "$ACTIVE_DESC" = "$ORANGE_DESC" ] || fail "active is '${ACTIVE_DESC:-?}', not $ORANGE_DESC"
  log "A6L_VOLTE4_VERIFY_PASS active='$ACTIVE_DESC' id=$ACTIVE_ID"
  grep "IMS_SVC" "$L/probe-verify.txt" 2>/dev/null | sed 's/^/  /' | tee -a "$LOG"
  if [ "${NO_VOLTE2:-0}" = 1 ]; then log "A6L_VOLTE4_NOTE NO_VOLTE2=1: volte2 not chained"
  else
    [ "${A6L_RF_APPROVED:-0}" = 1 ] || fail "volte2 chain needs A6L_RF_APPROVED=1 (or NO_VOLTE2=1)"
    [ -f "$V2/volte2-test.sh" ] || fail "no $V2/volte2-test.sh"
    step "verify: chain volte2 (KEEP_ONLINE=1)"
    KEEP_ONLINE=1 SKIP_RADIO=1 sh "$V2/volte2-test.sh" 2>&1 | grep -v "^WARNING: linker" | mask | tee "$L/volte2.txt" | tee -a "$LOG"
    r=$(grep "A6L_VOLTE2_RESULT" "$L/volte2.txt" | tail -n 1)
    log "A6L_VOLTE4_VOLTE2_RESULT ${r#A6L_VOLTE2_RESULT }"
  fi;;
revert)
  gates write; step "revert: pre-state"
  pdc_state pre-rev
  tgt=${REVERT_ID:-$ROW_ID}
  if [ "$ACTIVE_ID" = "$tgt" ]; then log "A6L_VOLTE4_REVERT_PASS already active id=$tgt ('$ACTIVE_DESC')"
  else
    [ "$tgt" != "$ROW_ID" ] || [ $ROW_SEEN = 1 ] || fail "ROW id not listed; reboot instead (RAM EFS is discarded)"
    do_activate "$tgt" revert; rc=$?
    [ "$ACTIVE_ID" = "$tgt" ] || fail "revert not active (active=${ACTIVE_ID:-none}): reboot the phone to restore the original EFS"
    case $rc in 0) rs=yes;; 2) rs=no;; *) rs=unclean; log "A6L_VOLTE4_NOTE modem restart not clean; a reboot is the clean reset";; esac
    log "A6L_VOLTE4_REVERT_PASS active='$ACTIVE_DESC' id=$ACTIVE_ID restart=$rs"
  fi;;
*) log "A6L_VOLTE4_FAIL unknown MODE=$MODE (ro|load|activate|verify|revert)"; exit 2;;
esac
log "A6L_VOLTE4_DONE mode=$MODE result=OK log=$LOG"
