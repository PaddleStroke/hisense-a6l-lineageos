#!/system/bin/sh
# ATTENDED ONLY, V74 diagnostic recovery (V71 controls image). v75/speaker (sens agent, 26 Sep 2026): FIRST loudspeaker test.
# NXP TFA9894 smart amp (I2C 0x34 on blsp_i2c6, reset gpio76, irq gpio77, stock container tfa98xx.cnt) on TERTIARY_MI2S_RX.
# SUPERSET of v75/audio6: same modules (patched q6routing/q6asm-dai/q6adm), but the runtime DT overlay is the ROM's
# a6l-voice-speaker-onbase-v75 (voice + speaker links; the audio6/kvoice overlay is a6l-voice-onbase-v75). Only ONE of the two
# overlays can go in per boot. After MODE=ovl + ADSP + MODE=load here, every v75/audio6 mode (capmain, caphs, tone, call) still
# works from /tmp/audio6 (same pcm numbering MM1=0, MM2=1, VoiceMMode1=2; the card is already bound so audio6 skips its load).
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/speaker MODE=<mode> sh /tmp/speaker/run.sh
#   ovl   : FRESH BOOT, BEFORE the ADSP bundle, INSTEAD of audio6 MODE=ovl.                  PASS = A6L_SPK_OVL_PASS
#   load  : after the ADSP bundle: tfa98xx.cnt, patched LPI pinctrl (ter_mi2s), audio modules, snd-soc-tfa98xx BEFORE
#           snd-soc-sm8250. NO SOUND. Prints the TFA probe result.                         PASS = A6L_SPK_LOAD_PASS
#   diag  : NO SOUND. I2C/probe, TFA registers, debugfs, controls, DAPM, LPI pins 4-7, L13A.
#   spk   : LEVEL=40 (default) plays 2 s 1 kHz at -40 dBFS on MultiMedia1 -> TERT_MI2S_RX -> TFA9894, registers sampled
#           DURING playback. LEVEL=20 = -20 dBFS (ONLY after Pierre confirmed -40 was fine or inaudible). Nothing else is
#           accepted. HEADSET UNPLUGGED.                                                      PASS = A6L_SPK_TONE_PASS + Pierre
#   off   : speaker route off.
# Safety: the stock container is used unchanged; the "... Calibration" control, debugfs calibrate/R and any register write are
# NEVER touched (factory calibration lives in the amp MTP). Hard cap: files other than the two tones are refused, and each
# tone's peak is re-measured with a6l_wavlevel before playing (must be <= -19.5 dBFS).
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
hashchk() { ( cd "$D" || exit 1; n=0; while read -r h f; do [ -n "$f" ] || continue; f=${f#\*}
    [ -f "$f" ] || { echo "A6L_HASH missing $f"; exit 1; }
    set -- $(sha256sum "$f"); [ "$1" = "$h" ] || { echo "A6L_HASH mismatch $f"; exit 1; }; n=$((n+1)); done < SHA256SUMS
    [ $n -gt 0 ] || exit 1; echo "A6L_HASH_OK $n files" ); }
hashchk || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_SPK_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\$p"; }
MODE=${MODE:-diag}; LEVEL=${LEVEL:-40}; MX=$D/mixer; T=$D/bin; OUT=${OUT:-/tmp/speaker-out}; mkdir -p "$OUT"
chmod 755 "$T"/* 2>/dev/null
adsp_rproc() { for r in /sys/class/remoteproc/remoteproc*; do [ "$(cat "$r/name" 2>/dev/null)" = adsp ] && { echo "$r"; return 0; }; done
  for r in /sys/class/remoteproc/remoteproc*; do case "$(readlink -f "$r/device" 2>/dev/null)" in *15700000*) echo "$r"; return 0;; esac; done; return 1; }
adsp_state() { r=$(adsp_rproc) || return 0; cat "$r/state" 2>/dev/null; }
voice_dt() { tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-voice 2>/dev/null; }
card() { sed -n 's/^ *\([0-9]*\) \[.*\]: .*Hisense A6L.*/\1/p' /proc/asound/cards | head -n 1; }
mknodes() { mkdir -p /dev/snd; for s in /sys/class/sound/*; do n=${s##*/}; [ -r "$s/dev" ] || continue; mm=$(cat "$s/dev")
  if [ -e "/dev/snd/$n" ]; then cur=$(ls -l "/dev/snd/$n" | sed -n 's/.* \([0-9]*\), *\([0-9]*\) .*/\1:\2/p'); [ "$cur" = "$mm" ] && continue; rm -f "/dev/snd/$n"; fi
  mknod "/dev/snd/$n" c "${mm%%:*}" "${mm##*:}"; done; }
loaded() { grep -q "^$(echo "${1%.ko}" | tr - _) " /proc/modules; }
ins() { loaded "$1" && return 0; insmod "$2" || { echo "A6L_HW_FAIL insmod $1"; klog | tail -n 15; exit 4; }; }
echo "A6L_SPK adsp=$(adsp_rproc) state=$(adsp_state) voice_dt=$(voice_dt)"

# ---------------------------------------------------------------- ovl
if [ "$MODE" = ovl ]; then
  case "$(voice_dt)" in
    kvoice-speaker-onbase-v75) echo "A6L_SPK_OVL already applied"; echo A6L_SPK_OVL_PASS; exit 0;;
    ?*) echo "A6L_HW_FAIL another voice overlay is applied ($(voice_dt), audio6/kvoice): the speaker overlay needs a fresh boot (run speaker ovl INSTEAD of audio6 ovl)"; exit 5;; esac
  case "$(adsp_state)" in running) echo "A6L_HW_FAIL ADSP already running: the overlay must go in BEFORE the adsp bundle. Reboot to recovery."; exit 5;; esac
  loaded snd-soc-sm8250.ko && { echo "A6L_HW_FAIL card driver already loaded: reboot to recovery"; exit 5; }
  insmod "$D/extra/a6l_spk_ovl.ko" || { echo "A6L_HW_FAIL overlay insmod"; klog | tail -n 10; exit 4; }
  klog | grep A6L_VOICE_OVL
  [ "$(voice_dt)" = kvoice-speaker-onbase-v75 ] || { echo "A6L_SPK_OVL_FAIL chosen/hisense,a6l-voice='$(voice_dt)'"; exit 6; }
  [ -d /proc/device-tree/soc@0/i2c@c1b6000/audio-amplifier@34 ] && echo "  TFA node: $(tr -d '\0' < /proc/device-tree/soc@0/i2c@c1b6000/audio-amplifier@34/compatible) status=$(tr -d '\0' < /proc/device-tree/soc@0/i2c@c1b6000/status 2>/dev/null)" \
    || { n=$(ls -d /proc/device-tree/soc@0/*/audio-amplifier@34 2>/dev/null | head -n 1); echo "  TFA node: ${n:-NOT FOUND}"; }
  echo "A6L_SPK_OVL_PASS now run the ADSP bundle (and sens MODE=pre/load if wanted), then MODE=load"; exit 0
fi

[ "$(voice_dt)" = kvoice-speaker-onbase-v75 ] || { echo "A6L_HW_FAIL speaker overlay not applied (voice_dt='$(voice_dt)'); MODE=ovl in a fresh boot before the ADSP"; exit 6; }
if grep -q "^q6routing " /proc/modules; then grep -q " msm_routing_put_audio_mixer_tx" /proc/kallsyms || { echo "A6L_HW_FAIL the UNPATCHED q6routing is loaded: reboot to recovery"; exit 12; }; fi

# TFA state helpers (only touch the amp's sysfs when the REAL driver probed: the stub has no driver data)
TD=""; for d in /sys/bus/i2c/devices/*-0034; do [ -e "$d" ] && TD=$d; done
tfa_real() { dmesg | grep -q "TFA9894 detected" && ! dmesg | grep -q "A6L_TFA_STUB" && [ -n "$TD" ] && [ -e "$TD/rw" ]; }
rreg() { dd if="$T/regbytes" of="$TD/reg" conv=notrunc bs=1 skip=$1 count=1 2>/dev/null; dd if="$TD/rw" bs=2 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n'; }
regs() {  # $1 = label. 0x00 SYS_CTRL0 (b0 PWDN, b3 AMPE) 0x03 REV 0x10 STATUS0 (b0 VDDS b1 PLLS b2 OTDS b3 OVDS b4 UVDS b5 OCDS
          # b6 CLKS b8 NOCLK b11 SWS b12 AMPS) 0x11 (b10 TDMERR) 0x12 (b6 MANOPER) 0x14 (b3-6 MANSTATE)
  tfa_real || { echo "  TFA_REGS $1: skipped (no real TFA probe)"; return; }
  l=""; for r in 0 1 2 3 4 16 17 18 19 20; do l="$l $(printf %02x $r)=$(rreg $r)"; done; echo "  TFA_REGS $1:$l"
  s=$(rreg 16); [ -n "$s" ] || return; v=$((0x$s)); m=$(rreg 20); ms=$(( (0x${m:-0} >> 3) & 15 ))
  echo "  TFA_STATUS $1: VDDS=$((v&1)) PLLS=$((v>>1&1)) OTDS=$((v>>2&1)) OVDS=$((v>>3&1)) UVDS=$((v>>4&1)) OCDS=$((v>>5&1)) CLKS=$((v>>6&1)) NOCLK=$((v>>8&1)) SWS=$((v>>11&1)) AMPS=$((v>>12&1)) MANSTATE=$ms"
  LASTSTAT=$v; }
tfa_probe_report() {
  echo "  -- TFA probe (klog):"; dmesg | grep -iE "tfa|0034|i2c.*c1b6000|blsp_i2c6|lpass-lpi|ter_mi2s" | grep -v "$MARK" | tail -n 20 | sed 's/^/  KLOG: /'
  echo "  I2C dev: ${TD:-none at 0x34} name=$(cat "$TD/name" 2>/dev/null) driver=$(basename "$(readlink "$TD/driver" 2>/dev/null)" 2>/dev/null)"
  if dmesg | grep -q "A6L_TFA_STUB"; then echo "A6L_SPK_STUB amp not usable: $(dmesg | grep A6L_TFA_STUB | tail -n 1 | sed 's/.*A6L_TFA_STUB/A6L_TFA_STUB/')"
  elif dmesg | grep -q "TFA9894 detected"; then echo "A6L_SPK_TFA_DETECTED TFA9894 (real driver)"
  else echo "A6L_SPK_TFA_UNKNOWN no 'TFA9894 detected' and no stub line"; fi
  for r in /sys/class/regulator/regulator.*; do case "$(cat $r/name 2>/dev/null)" in *l13*|*L13*) echo "  REG $(cat $r/name) state=$(cat $r/state 2>/dev/null) uV=$(cat $r/microvolts 2>/dev/null) users=$(cat $r/num_users 2>/dev/null)";; esac; done
  mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
  DBG=$(ls -d /sys/kernel/debug/tfa98xx* /sys/kernel/debug/*-34 2>/dev/null | head -n 1)
  if [ -n "$DBG" ] && tfa_real; then for f in version fw-state dsp-state MTPEX OTC TEMP; do echo "  DBGFS ${DBG##*/}/$f: $(head -c 120 "$DBG/$f" 2>&1 | tr '\n' ' ')"; done; fi
  echo "  ASOC components: $(grep -iE 'tfa|q6|msm8916|lpi' /sys/kernel/debug/asoc/components 2>/dev/null | tr '\n' ' ')"
  for pc in /sys/kernel/debug/pinctrl/*; do case "$pc" in *15070000*|*lpi*) grep -E "^pin [4-7] " "$pc/pinmux-pins" 2>/dev/null | sed 's/^/  LPIPIN /';; esac; done; }

# ---------------------------------------------------------------- load
if [ -z "$(card)" ]; then
  [ "$MODE" = load ] || { echo "A6L_HW_FAIL no card yet: run MODE=load first"; exit 8; }
  [ "$(adsp_state)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  if loaded q6asm-dai.ko; then grep -q "q6asm_dai_of_xlate_dai_name" /proc/kallsyms || { echo "A6L_HW_FAIL unpatched q6asm-dai loaded: reboot"; exit 12; }; fi
  # 1. container: /lib/firmware (recovery ramdisk) AND the firmware_class search path (belt and braces)
  mkdir -p /lib/firmware && cp "$D/firmware/tfa98xx.cnt" /lib/firmware/ 2>/dev/null || echo "  note: /lib/firmware not writable"
  [ -w /sys/module/firmware_class/parameters/path ] && [ -z "$(cat /sys/module/firmware_class/parameters/path)" ] && echo -n "$D/firmware" > /sys/module/firmware_class/parameters/path
  echo "  firmware_class.path='$(cat /sys/module/firmware_class/parameters/path 2>/dev/null)'"
  # 2. patched LPI pinctrl (ter_mi2s functions for gpio4-7) BEFORE the stock one in order.txt (load then skips it)
  if ! loaded pinctrl-sdm660-lpass-lpi.ko; then ins pinctrl-lpass-lpi.ko "$D/modules/pinctrl-lpass-lpi.ko"; ins pinctrl-sdm660-lpass-lpi.ko "$D/extra/pinctrl-sdm660-lpass-lpi.ko"; fi
  # 3. all audio6 modules; the TFA driver right before the machine driver (the card waits for every dai-link codec)
  while read -r ko; do [ -n "$ko" ] || continue
    [ "$ko" = snd-soc-sm8250.ko ] && { ins snd-soc-tfa98xx.ko "$D/extra/snd-soc-tfa98xx.ko"; sleep 3; }
    ins "$ko" "$D/modules/$ko"; done < "$D/modules/order.txt"
  sleep 8
fi
grep -q " msm_routing_put_audio_mixer_tx" /proc/kallsyms && echo "A6L_Q6ROUTING_PATCHED yes" || { echo "A6L_HW_FAIL patched q6routing not active"; exit 12; }
C=$(card); [ -n "$C" ] || { echo "A6L_HW_FAIL no Hisense A6L card"; cat /proc/asound/cards; klog | grep -iE "snd|asoc|q6|tfa|voice|apr|defer" | tail -n 25; tfa_probe_report; exit 8; }
mknodes; MIX="$T/tinymix -D $C"
has() { o=$($MIX -- "$1" 2>&1 | head -n 1); case "$o" in "$1: "*|"$1:") return 0;; *) return 1;; esac; }
apply() { f="$MX/$1"; ok=0; bad=0
  while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac
    case "$n" in *Calibration*|*Stop*|*"Playback Volume"*) echo "A6L_HW_FAIL refusing $n (TFA controls are never written except the profile)"; exit 9;;
      *"Digital Volume") [ "$v" -le 84 ] 2>/dev/null || { echo "A6L_HW_FAIL refusing $n=$v (cap 84 = 0 dB)"; exit 9; };; esac
    if ! has "$n"; then echo "  MISSING '$n'"; bad=$((bad+1)); continue; fi
    if $MIX -- "$n" "$v" > /dev/null 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "  SET_FAIL '$n' = $v"; fi
  done < "$f"; echo "A6L_MIXER $1 ok=$ok fail=$bad"; }
pcmdev() { sed -n "s/^0*$C-\([0-9][0-9]*\): $1.*/\1/p" /proc/asound/pcm | head -n 1 | sed 's/^0*\([0-9]\)/\1/'; }
PB=$(pcmdev "MultiMedia1 "); VDEV=$(pcmdev "VoiceMMode1 ")
TFACTL=$($MIX 2>/dev/null | grep -E "Profile|Stop|Calibration|TFA|tfa" | head -n 12)
PROF=$(echo "$TFACTL" | sed -n 's/^[0-9]*[[:space:]]*ENUM[[:space:]]*[0-9]*[[:space:]]*\(.* Profile\)[[:space:]]*.*/\1/p' | head -n 1)
[ -n "$PROF" ] || PROF=$(echo "$TFACTL" | grep -o "[A-Za-z0-9_]* Profile" | head -n 1)
echo "A6L_SPK card=$C mm1_dev=${PB:-none} voice_dev=${VDEV:-none} mode=$MODE tfa_profile_ctl='${PROF:-none}'"

case "$MODE" in
load)
  tfa_probe_report
  # fixes-20260927: reasons carry the A6L_SPK_ prefix (the attended one-liners grep for it); only the speaker link itself
  # is fatal, a missing headset route / other pcm numbering is a warning (audio6 checks the headset path itself)
  ok=1; [ "$PB" = 0 ] || echo "A6L_SPK_WARN MultiMedia1 is pcm ${PB:-none}, not 0 (tinyplay below uses the detected one)"
  [ "$VDEV" = 2 ] || echo "A6L_SPK_WARN voice pcm is ${VDEV:-none}, not 2"
  [ -n "$PB" ] || { echo "A6L_SPK_LOAD_WHY no MultiMedia1 pcm"; ok=0; }
  has "TERT_MI2S_RX Audio Mixer MultiMedia1" || { echo "A6L_SPK_LOAD_WHY MISSING 'TERT_MI2S_RX Audio Mixer MultiMedia1' (speaker link not in the card)"; ok=0; }
  has "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" || echo "A6L_SPK_WARN MISSING headset route 'LPI_MI2S_RX_0 Audio Mixer MultiMedia1'"
  dmesg | grep -q "A6L_TFA_STUB" && { echo "A6L_SPK_LOAD_WHY TFA stub DAI (amp not usable)"; ok=0; }
  echo "  TFA controls:"; echo "$TFACTL" | sed 's/^/    /'
  apply common-off.txt > /dev/null; regs idle
  [ $ok = 1 ] && echo "A6L_SPK_LOAD_PASS (headset/mic tests of v75/audio6 can now run from /tmp/audio6)" || echo "A6L_SPK_LOAD_FAIL";;
diag)
  tfa_probe_report; echo "  TFA controls:"; echo "$TFACTL" | sed 's/^/    /'; regs idle
  for n in "TERT_MI2S_RX Audio Mixer MultiMedia1" "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" "Headphone Jack"; do echo "  CTL $($MIX -- "$n" 2>&1 | head -n 1 | cut -c1-100)"; done
  [ -n "$PROF" ] && echo "  CTL $($MIX -- "$PROF" 2>&1 | head -n 1 | cut -c1-160)";;
spk)
  case "$LEVEL" in 40) W=sine1k-m40dBFS-stereo-2s.wav;; 20) W=sine1k-m20dBFS-stereo-2s.wav;; *) echo "A6L_HW_FAIL LEVEL must be 40 or 20"; exit 7;; esac
  lv=$("$T/a6l_wavlevel" "$D/wav/$W" 2>&1); pk=$(echo "$lv" | sed -n 's/.* peak=\(-*[0-9.]*\).*/\1/p' | head -n 1)
  pki=$(echo "${pk:-0}" | sed 's/\..*//'); [ -n "$pk" ] && [ "${pki:-0}" -le -20 ] || { echo "A6L_HW_FAIL tone peak '$pk' dBFS above the -19.5 cap: refusing"; exit 9; }
  echo "  tone $W peak=$pk dBFS"
  case "$($MIX -- "Headphone Jack" 2>&1 | head -n 1)" in *On*|*": 1"*) echo "  WARNING: headset plugged: unplug it (the test must use the loudspeaker only)";; esac
  has "TERT_MI2S_RX Audio Mixer MultiMedia1" || { echo "A6L_SKIP no TERT_MI2S route control"; exit 11; }
  if dmesg | grep -q "A6L_TFA_STUB"; then echo "A6L_SPK_STUB: amp not usable (stub DAI), nothing would be heard; see MODE=diag. Headset tests still valid."; exit 11; fi
  apply common-off.txt > /dev/null; apply speaker.txt
  if [ -n "$PROF" ]; then vals=$($MIX -- "$PROF" 2>&1 | head -n 1); echo "  $vals" | cut -c1-200
    case "$vals" in *music*) $MIX -- "$PROF" music > /dev/null 2>&1 && echo "  $PROF -> music";; *) echo "  $PROF: no 'music' value, left at default";; esac; fi
  regs before
  echo "A6L_SPK_MARK_$LEVEL" > /dev/kmsg
  echo "A6L_PLAY $W in 3 s: Pierre, listen to the LOUDSPEAKER (bottom of the phone)"; sleep 3
  "$T/tinyplay" "$D/wav/$W" -D "$C" -d "$PB" > "$OUT/tinyplay-$LEVEL.log" 2>&1 & pp=$!
  # fixes-20260927: 3 snapshots (0.5/1.0/1.5 s): the amp may still be starting at the first one, OCDS is clear-on-read
  GOOD=""; PROT=0; NSNAP=0; for sn in 1 2 3; do sleep 0.5; LASTSTAT=""; regs "during$sn"; [ -n "$LASTSTAT" ] || continue
    NSNAP=$((NSNAP+1)); v=$LASTSTAT
    [ $((v>>6&1)) = 1 ] && [ $((v>>8&1)) = 0 ] && [ $((v>>12&1)) = 1 ] && GOOD=$sn
    { [ $((v>>2&1)) = 1 ] || [ $((v>>4&1)) = 1 ] || [ $((v>>5&1)) = 1 ]; } && PROT=$((PROT+1)); done
  DAD=$(ls -d /sys/kernel/debug/asoc/* 2>/dev/null | grep -i a6l | head -n 1)
  [ -n "$DAD" ] && find "$DAD" -path '*dapm*' -type f 2>/dev/null | grep -iE "/(TERT_MI2S_RX|Tertiary MI2S Playback|TERT_MI2S_RX Audio Mixer|MM_DL1|MultiMedia1 Playback|AIF Playback|OUTL|OUTR|SPK|Speaker|bias_level)" | head -n 15 |
    while read -r w; do echo "  DAPM ${w#$DAD/}: $(head -n 1 "$w")"; done
  wait $pp; rc=$?; tail -n 2 "$OUT/tinyplay-$LEVEL.log"; echo "A6L_PLAY_RC $rc"
  regs after
  dmesg | sed -n "/A6L_SPK_MARK_$LEVEL/,\$p" | grep -iE "tfa|q6afe|q6adm|q6asm|tert|mi2s|error|fail|clock|NOCLK" | head -n 25 | sed 's/^/  KLOG: /'
  apply common-off.txt > /dev/null
  # verdict (fixes-20260927): FAIL only for a real error (stream error, or a protection flag in 2+ snapshots). A missing or
  # not-yet-running snapshot is not a failure: the stream ran, the ear decides (27 Sep: tone heard, check said FAIL).
  if [ $rc != 0 ]; then echo "A6L_SPK_TONE_FAIL level=-$LEVEL dBFS: tinyplay rc=$rc (stream error, see KLOG above)"
  elif [ $PROT -ge 2 ]; then echo "A6L_SPK_TONE_FAIL level=-$LEVEL dBFS: protection flag (OTDS/UVDS/OCDS) in $PROT of $NSNAP snapshots"
  elif [ -n "$GOOD" ]; then echo "A6L_SPK_TONE_PASS level=-$LEVEL dBFS (stream ran; amp clocked and enabled in snapshot $GOOD of $NSNAP)"
  else echo "A6L_SPK_TONE_PLAYED level=-$LEVEL dBFS (stream ran rc=0; amp state not confirmed: $NSNAP register snapshots, none with CLKS=1 NOCLK=0 AMPS=1): PASS if Pierre heard it"; fi
  echo "ASK PIERRE: heard a 1 kHz tone from the LOUDSPEAKER (not the earpiece, not the headset)? faint/ok/loud/distorted/nothing";;
off) apply common-off.txt;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
echo "A6L_SPK_DONE mode=$MODE"
