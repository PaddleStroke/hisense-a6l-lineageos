#!/system/bin/sh
# ATTENDED ONLY, V74 diagnostic recovery (V71 controls image). kvoice (24 Sep 2026): q6voice call-audio bring-up.
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/kvoice MODE=ovl|load|session|call|off [CVD=0|1] [LEVEL=1|2|3] [SECS=n] sh /tmp/kvoice/run.sh
#   ovl     : FRESH BOOT, BEFORE the ADSP bundle: insmod extra/a6l_voice_ovl.ko = runtime DT overlay (APR q6mvm/q6cvs/q6cvp +
#             links MultiMedia1/MultiMedia2/VoiceMMode1). Refuses if the ADSP (remoteproc named "adsp") is already running.
#   load    : after the ADSP bundle (adsp running): audio3 modules (patched q6asm-dai) + q6voice modules + machine driver.
#             PASS = card up, pcm "VoiceMMode1" = device 2, MultiMedia1 = device 0, the voice controls exist. NO SOUND.
#   session : NO RF / NO SOUND (analog muted). Sets the DSP voice routes and holds VoiceMMode1 open for SECS (default 5) s:
#             exercises MVM passive session, CVD version query, CVP create/enable, attach, MVM start, then stop/destroy.
#             PASS = A6L_Q6VOICED OPEN + CLOSED and no q6voice/q6cvp/q6mvm error in klog. CVD=0|1 forces the init sequence.
#   call    : RF, needs Pierre + SIM + an ACTIVE call placed by the ril tooling (the modem must already be in a call).
#             Headset (worn only after the first check) at LEVEL (default 1 = -30 dB, cap 3 = -18 dB) + headset mic, then
#             holds VoiceMMode1 for SECS (default 60) s. Ctrl-C ends early. ASK PIERRE: hears the far end? far end hears him?
#   off     : clear voice routes + everything silent.
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
# toybox sha256sum has no --ignore-missing: check every listed file by hand (all listed files must be shipped)
hashchk() { ( cd "$D" || exit 1; n=0; while read -r h f; do [ -n "$f" ] || continue; f=${f#\*}
    [ -f "$f" ] || { echo "A6L_HASH missing $f"; exit 1; }
    set -- $(sha256sum "$f"); [ "$1" = "$h" ] || { echo "A6L_HASH mismatch $f"; exit 1; }; n=$((n+1)); done < SHA256SUMS
    [ $n -gt 0 ] || exit 1; echo "A6L_HASH_OK $n files" ); }
hashchk || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_KV_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\$p"; }
MODE=${MODE:-load}; LEVEL=${LEVEL:-1}; SECS=${SECS:-}; MX=$D/mixer3; T=$D/bin; OUT=${OUT:-/tmp/kvoice-out}; mkdir -p "$OUT"
case "$LEVEL" in 1) RAW=54;; 2) RAW=60;; 3) RAW=66;; *) echo "A6L_HW_FAIL LEVEL must be 1..3"; exit 7;; esac
chmod 755 "$T"/* 2>/dev/null
# the ADSP is the remoteproc whose name is "adsp" (remoteproc0 is the modem when the radio bundle ran first)
adsp_rproc() { for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat "$r/name" 2>/dev/null)" = adsp ] && { echo "$r"; return 0; }; done
  for r in /sys/class/remoteproc/remoteproc*; do case "$(readlink -f "$r/device" 2>/dev/null)" in *15700000*) echo "$r"; return 0;; esac; done; return 1; }
adsp_state() { r=$(adsp_rproc) || return 0; cat "$r/state" 2>/dev/null; }
voice_dt() { tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-voice 2>/dev/null; }
card() { sed -n 's/^ *\([0-9]*\) \[.*\]: .*Hisense A6L.*/\1/p' /proc/asound/cards | head -n 1; }
mknodes() { mkdir -p /dev/snd; for s in /sys/class/sound/*; do n=${s##*/}; [ -r "$s/dev" ] || continue; mm=$(cat "$s/dev")
  if [ -e "/dev/snd/$n" ]; then cur=$(ls -l "/dev/snd/$n" | sed -n 's/.* \([0-9]*\), *\([0-9]*\) .*/\1:\2/p'); [ "$cur" = "$mm" ] && continue; rm -f "/dev/snd/$n"; fi
  mknod "/dev/snd/$n" c "${mm%%:*}" "${mm##*:}"; done; }
load() { while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules && continue
  insmod "$D/modules/$ko" || { echo "A6L_HW_FAIL insmod $ko"; klog | tail -n 15; exit 4; }; done < "$D/modules/order.txt"; }
if [ "$MODE" = ovl ]; then
  [ -n "$(voice_dt)" ] && { echo "A6L_KV_OVL already applied: $(voice_dt)"; exit 0; }
  case "$(adsp_state)" in running) echo "A6L_HW_FAIL ADSP already running: the overlay must go in BEFORE the adsp bundle. Reboot to recovery."; exit 5;; esac
  grep -q "^snd_soc_sm8250 " /proc/modules && { echo "A6L_HW_FAIL card driver already loaded: reboot to recovery"; exit 5; }
  insmod "$D/extra/a6l_voice_ovl.ko" || { echo "A6L_HW_FAIL overlay insmod"; klog | tail -n 10; exit 4; }
  klog | grep A6L_VOICE_OVL
  [ "$(voice_dt)" = kvoice-onbase-v75 ] && echo "A6L_KV_OVL_PASS now run the ADSP bundle, then MODE=load" || { echo "A6L_KV_OVL_FAIL"; exit 6; }
  ls /proc/device-tree/soc@0/remoteproc@15700000/glink-edge/apr/ | tr '\n' ' '; echo
  exit 0
fi
[ -n "$(voice_dt)" ] || { echo "A6L_HW_FAIL no voice DT (run MODE=ovl first, in a fresh boot, before the ADSP bundle)"; exit 6; }
if [ -z "$(card)" ]; then
  [ "$(adsp_state)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  if grep -q "^q6asm_dai " /proc/modules; then grep -q "q6asm_dai_of_xlate_dai_name" /proc/kallsyms || { echo "A6L_HW_FAIL unpatched q6asm-dai loaded: reboot"; exit 12; }; fi
  load; sleep 8
fi
MIXB="$T/tinymix"
C=$(card); [ -n "$C" ] || { echo "A6L_HW_FAIL no Hisense A6L card"; cat /proc/asound/cards; klog | grep -i "snd\|asoc\|q6\|voice\|apr" | tail -n 25; exit 8; }
mknodes; MIX="$MIXB -D $C"
has() { o=$($MIX -- "$1" 2>&1 | head -n 1); case "$o" in "$1: "*|"$1:") return 0;; *) return 1;; esac; }
rawof() { $MIX -- "$1" 2>&1 | head -n 1 | sed -n "s/^.*: *\([0-9][0-9]*\).*/\1/p"; }
mute_all() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume" "Digital RX3 Digital Volume"; do $MIX -- "$n" 0 > /dev/null 2>&1; done; }
capchk() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume"; do g=$(rawof "$n")
    [ -n "$g" ] && [ "$g" -le 66 ] || { echo "A6L_HW_FAIL $n read back '$g' (expected <= 66): muting and aborting" >&2; mute_all; exit 10; }; done; }
apply() { f="$MX/$1"; ok=0; bad=0
  while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac; v=$(echo "$v" | sed "s/@RAW@/$RAW/")
    case "$n" in *"Digital Volume") [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 66 ] || { echo "A6L_HW_FAIL refusing $n=$v" >&2; mute_all; exit 9; };; esac
    if ! has "$n"; then echo "  MISSING '$n'"; bad=$((bad+1)); continue; fi
    if $MIX -- "$n" "$v" > /dev/null 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "  SET_FAIL '$n' = $v"; fi
  done < "$f"; capchk; echo "A6L_MIXER $1 ok=$ok fail=$bad"; }
pcmdev() { sed -n "s/^0*$C-\([0-9][0-9]*\): $1.*/\1/p" /proc/asound/pcm | head -n 1 | sed 's/^0*\([0-9]\)/\1/'; }
VDEV=$(pcmdev "VoiceMMode1 "); MMDEV=$(pcmdev "MultiMedia1 ")
echo "A6L_KV card=$C voice_dev=${VDEV:-none} mm1_dev=${MMDEV:-none} mode=$MODE voice_dt=$(voice_dt) cvd_param=$(cat /sys/module/q6voice/parameters/cvd_mode 2>/dev/null)"
[ -n "${CVD:-}" ] && { echo "$CVD" > /sys/module/q6voice/parameters/cvd_mode && echo "A6L_KV cvd_mode forced to $CVD"; }
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
hold() { "$T/a6l-q6voiced" -c "$C" -d "$VDEV" -r hold "$1" 2>&1 | tee "$OUT/q6voiced-$MODE.log"
  klog | grep -iE "A6L_Q6VOICE|q6voice|q6cvp|q6mvm|q6cvs|apr|q6afe" | tail -n 25 | tee "$OUT/klog-$MODE.txt"
  if grep -q "A6L_Q6VOICED OPEN card" "$OUT/q6voiced-$MODE.log" && grep -q "A6L_Q6VOICED CLOSED" "$OUT/q6voiced-$MODE.log" \
     && ! grep -qiE "failed to|error|timed out|unexpected reply" "$OUT/klog-$MODE.txt"; then echo "A6L_KV_SESSION_PASS"; else echo "A6L_KV_SESSION_FAIL (see above)"; fi; }
case "$MODE" in
load)
  ok=1; [ "$VDEV" = 2 ] || { echo "  voice pcm not device 2"; ok=0; }; [ "$MMDEV" = 0 ] || { echo "  MultiMedia1 not device 0"; ok=0; }
  for n in "LPI_MI2S_RX_0 Voice Mixer VoiceMMode1" "VoiceMMode1 Capture Mixer LPI_MI2S_TX_3" "VoiceMMode1 TX Topology" "LPI_MI2S_RX_0 Audio Mixer MultiMedia1"; do
    has "$n" && echo "  ok '$n'" || { echo "  MISSING '$n'"; ok=0; }; done
  grep -iE "q6voice|mvm|cvp|cvs" /sys/kernel/debug/asoc/components 2>/dev/null | head
  cat /proc/asound/pcm
  [ $ok = 1 ] && echo "A6L_KV_LOAD_PASS" || { echo "A6L_KV_LOAD_FAIL"; klog | grep -iE "voice|mvm|cvp|cvs|asoc|apr" | tail -n 20; };;
session)
  apply common-off.txt > /dev/null; mute_all
  hold "${SECS:-5}";;
call)
  echo "A6L_KV_CALL: the modem must ALREADY be in an active call (ril tooling). Headset at raw $RAW ($((RAW-84)) dB). First run: NOT worn."
  apply common-off.txt > /dev/null; apply headset.txt; apply headset-mic.txt
  sleep 2; hold "${SECS:-60}"; apply common-off.txt > /dev/null; "$T/a6l-q6voiced" -c "$C" routes 0 > /dev/null
  echo "ASK PIERRE: did he hear the far end in the headset? did the far end hear him?";;
off) "$T/a6l-q6voiced" -c "$C" routes 0; apply common-off.txt;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
echo "A6L_KV_DONE mode=$MODE"
