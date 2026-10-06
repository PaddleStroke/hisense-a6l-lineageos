#!/system/bin/sh
# ATTENDED ONLY, V74 diagnostic recovery (V71 controls image). audio4 / audfix (24 Sep 2026): media playback fix
# (q6routing per-direction sessions) + call gains. Same module stack as v75/kvoice, only q6routing.ko is new.
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/audio5 MODE=ovl|load|tone|media|call|capture|off [GAIN=0|3|6] [SECS=n] sh /tmp/audio4/run.sh
#   ovl   : FRESH BOOT, BEFORE the ADSP bundle: voice/links runtime DT overlay (same as kvoice MODE=ovl).
#   load  : after the ADSP bundle: all audio modules incl. the PATCHED q6routing.ko. NO SOUND. PASS = A6L_AU4_LOAD_PASS.
#   tone  : headset NOT WORN. MultiMedia1 routed RX+TX (the case that failed), headset at raw 54 (-30 dB digital),
#           1 kHz -30 dBFS tone left / right / both. PASS = tinyplay rc 0 and NO "q6adm ... error" / "DSP returned error".
#           ROUTES=rx sets only the RX route (the zero-build check: also works with the OLD kvoice/audio3 q6routing.ko).
#   media : headset at raw 84 (0 dB digital = stock level), same -30 dBFS tones, then FULL DUPLEX: 4 s headset-mic capture
#           on MultiMedia2 (pcmC0D1c) while the stereo tone plays on MultiMedia1 (pcmC0D0p). PASS = both rc 0, bytes > 0,
#           no ADM error. Then an INFORMATIVE try of duplex on MultiMedia1 alone (D0p + D0c; mainline q6asm-dai shares one
#           ASM session per FE, so this may fail: result only decides the ROM HAL capture device, it is not a PASS criterion).
#   call  : RF: needs an ACTIVE call placed by the ril tooling. Headset at 0 dB + GAIN (0|3|6 dB -> raw 84|87|90, cap 90).
#           Holds VoiceMMode1 for SECS (default 60) s. ASK PIERRE: far end audible/comfortable? far end hears him?
#   off   : clear voice routes + everything silent.
#   capture (audio5, misc2 25 Sep 2026): CAPTURE ONLY, nothing is played (safe, headset may be worn). Needs MODE=ovl + load
#           done in this boot (load uses the audio5 modules: q6adm.ko with endpoint_id_2 = 0xFFFF like stock).
#           Runs up to 4 variants and prints EVERYTHING (tinycap output, DPCM state, DAPM widgets, pcm status, klog):
#             A: MM2 (pcmC0D1c) 48k/2ch  B: MM2 48k/1ch  C: MM1 (pcmC0D0c, TX route only) 48k/2ch  D: MM2 16k/1ch
#           MIC=headset|main (default headset), SECS (default 4), ONLY=A|B|C|D (one variant). PASS per variant =
#           > 50 % of the expected bytes and non-zero samples: A6L_AU5_CAP_PASS <v> ... / A6L_AU5_CAP_FAIL <v> ...
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
# toybox sha256sum has no --ignore-missing: check every listed file by hand (all listed files must be shipped)
hashchk() { ( cd "$D" || exit 1; n=0; while read -r h f; do [ -n "$f" ] || continue; f=${f#\*}
    [ -f "$f" ] || { echo "A6L_HASH missing $f"; exit 1; }
    set -- $(sha256sum "$f"); [ "$1" = "$h" ] || { echo "A6L_HASH mismatch $f"; exit 1; }; n=$((n+1)); done < SHA256SUMS
    [ $n -gt 0 ] || exit 1; echo "A6L_HASH_OK $n files" ); }
hashchk || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_AU4_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\$p"; }
MODE=${MODE:-load}; GAIN=${GAIN:-0}; SECS=${SECS:-}; MX=$D/mixer3; T=$D/bin; OUT=${OUT:-/tmp/audio5-out}; mkdir -p "$OUT"
CAP=90   # hard cap raw 90 = +6 dB digital
case "$MODE" in tone) RAW=54;; media) RAW=84;; call) case "$GAIN" in 0) RAW=84;; 3) RAW=87;; 6) RAW=90;; *) echo "A6L_HW_FAIL GAIN must be 0|3|6"; exit 7;; esac;; *) RAW=0;; esac
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
echo "A6L_AU4 adsp=$(adsp_rproc) state=$(adsp_state)"
if [ "$MODE" = ovl ]; then
  [ -n "$(voice_dt)" ] && { echo "A6L_AU4_OVL already applied: $(voice_dt)"; exit 0; }
  case "$(adsp_state)" in running) echo "A6L_HW_FAIL ADSP already running: the overlay must go in BEFORE the adsp bundle. Reboot to recovery."; exit 5;; esac
  grep -q "^snd_soc_sm8250 " /proc/modules && { echo "A6L_HW_FAIL card driver already loaded: reboot to recovery"; exit 5; }
  insmod "$D/extra/a6l_voice_ovl.ko" || { echo "A6L_HW_FAIL overlay insmod"; klog | tail -n 10; exit 4; }
  klog | grep A6L_VOICE_OVL
  [ "$(voice_dt)" = kvoice-onbase-v75 ] && echo "A6L_AU4_OVL_PASS now run the ADSP bundle, then MODE=load" || { echo "A6L_AU4_OVL_FAIL"; exit 6; }
  exit 0
fi
[ -n "$(voice_dt)" ] || { echo "A6L_HW_FAIL no voice DT (run MODE=ovl first, in a fresh boot, before the ADSP bundle)"; exit 6; }
if grep -q "^q6routing " /proc/modules; then grep -q " msm_routing_put_audio_mixer_tx" /proc/kallsyms || { echo "A6L_HW_FAIL the UNPATCHED q6routing is loaded (earlier audio bundle in this boot): reboot to recovery"; exit 12; }; fi
if [ -z "$(card)" ]; then
  [ "$(adsp_state)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  if grep -q "^q6asm_dai " /proc/modules; then grep -q "q6asm_dai_of_xlate_dai_name" /proc/kallsyms || { echo "A6L_HW_FAIL unpatched q6asm-dai loaded: reboot"; exit 12; }; fi
  load; sleep 8
fi
grep -q " msm_routing_put_audio_mixer_tx" /proc/kallsyms && echo "A6L_Q6ROUTING_PATCHED yes" || { echo "A6L_HW_FAIL patched q6routing not active"; exit 12; }
C=$(card); [ -n "$C" ] || { echo "A6L_HW_FAIL no Hisense A6L card"; cat /proc/asound/cards; klog | grep -i "snd\|asoc\|q6\|voice\|apr" | tail -n 25; exit 8; }
mknodes; MIX="$T/tinymix -D $C"
has() { o=$($MIX -- "$1" 2>&1 | head -n 1); case "$o" in "$1: "*|"$1:") return 0;; *) return 1;; esac; }
rawof() { $MIX -- "$1" 2>&1 | head -n 1 | sed -n "s/^.*: *\([0-9][0-9]*\).*/\1/p"; }
mute_all() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume" "Digital RX3 Digital Volume"; do $MIX -- "$n" 0 > /dev/null 2>&1; done; }
capchk() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume"; do g=$(rawof "$n")
    [ -n "$g" ] && [ "$g" -le "$CAP" ] || { echo "A6L_HW_FAIL $n read back '$g' (expected <= $CAP): muting and aborting" >&2; mute_all; exit 10; }; done; }
apply() { f="$MX/$1"; ok=0; bad=0
  while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac; v=$(echo "$v" | sed "s/@RAW@/$RAW/")
    case "$n" in *"Digital Volume") [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le "$CAP" ] || { echo "A6L_HW_FAIL refusing $n=$v (cap $CAP)" >&2; mute_all; exit 9; };; esac
    if ! has "$n"; then echo "  MISSING '$n'"; bad=$((bad+1)); continue; fi
    if $MIX -- "$n" "$v" > /dev/null 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "  SET_FAIL '$n' = $v"; fi
  done < "$f"; capchk; echo "A6L_MIXER $1 ok=$ok fail=$bad"; }
pcmdev() { sed -n "s/^0*$C-\([0-9][0-9]*\): $1.*/\1/p" /proc/asound/pcm | head -n 1 | sed 's/^0*\([0-9]\)/\1/'; }
VDEV=$(pcmdev "VoiceMMode1 "); PB=$(pcmdev "MultiMedia1 ")
echo "A6L_AU4 card=$C mm1_dev=${PB:-none} voice_dev=${VDEV:-none} mode=$MODE raw=$RAW ($((RAW-84)) dB)"
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
adm_err() { klog | grep -iE "q6adm.*error|DSP returned error|Routing not setup|A6L_Q6ROUTING|q6asm.*(error|fail)|no backend DAIs|AFE enable|returned error|Audio Client already" | tail -n 8; }
routes_show() { for n in "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" "MultiMedia1 Mixer LPI_MI2S_TX_3"; do echo "  $($MIX -- "$n" 2>&1 | head -n 1 | cut -c1-90)"; done; }
play() { echo "A6L_PLAY $1 in 3 s (raw $RAW = $((RAW-84)) dB digital)"; sleep 3
  "$T/tinyplay" "$D/wav/$1" -D "$C" -d "$PB" > "$OUT/tinyplay-$1.log" 2>&1; rc=$?
  tail -n 2 "$OUT/tinyplay-$1.log"; echo "A6L_PLAY_RC $1 $rc"; [ $rc = 0 ] || FAILS=$((FAILS+1)); }
verdict() { e=$(adm_err); [ -n "$e" ] && { echo "$e"; FAILS=$((FAILS+1)); }
  [ "$FAILS" = 0 ] && echo "A6L_AU4_${1}_PASS" || echo "A6L_AU4_${1}_FAIL fails=$FAILS (see above)"; }
FAILS=0
case "$MODE" in
load)
  ok=1; [ "$PB" = 0 ] || { echo "  MultiMedia1 not device 0"; ok=0; }; [ "$VDEV" = 2 ] || { echo "  voice pcm not device 2"; ok=0; }
  for n in "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" "MultiMedia1 Mixer LPI_MI2S_TX_3" "LPI_MI2S_RX_0 Voice Mixer VoiceMMode1"; do has "$n" || { echo "  MISSING '$n'"; ok=0; }; done
  # per-direction check: RX and TX route of MultiMedia1 both read back 1 (the upstream module shows only the last one)
  apply common-off.txt > /dev/null; mute_all; apply fe-routes.txt; routes_show
  case "$($MIX -- "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" | head -n 1)" in *On*|*": 1"*) ;; *) echo "  RX route not kept"; ok=0;; esac
  case "$($MIX -- "MultiMedia1 Mixer LPI_MI2S_TX_3" | head -n 1)" in *On*|*": 1"*) ;; *) echo "  TX route not kept"; ok=0;; esac
  "$T/pcmprobe" /dev/snd/pcmC${C}D0p hw; "$T/pcmprobe" /dev/snd/pcmC${C}D0c hw
  apply common-off.txt > /dev/null
  [ $ok = 1 ] && echo "A6L_AU4_LOAD_PASS" || echo "A6L_AU4_LOAD_FAIL";;
tone)
  echo "A6L_AU4_TONE: headset plugged, NOT WORN (hold an earbud near the ear)."
  if [ "${ROUTES:-both}" = rx ]; then apply common-off.txt > /dev/null; apply rx-only.txt; else apply common-off.txt > /dev/null; apply fe-routes.txt; fi
  routes_show; apply headset.txt
  for w in sine1k-m30dBFS-left-2s.wav sine1k-m30dBFS-right-2s.wav sine1k-m30dBFS-stereo-3s.wav; do play $w; done
  apply common-off.txt > /dev/null; verdict TONE
  echo "ASK PIERRE: faint tone left only, then right only, then both? (y/n each)";;
media)
  echo "A6L_AU4_MEDIA: 0 dB digital (stock level) with a -30 dBFS tone. Headset may be worn only if MODE=tone was fine."
  CPD=$(pcmdev "MultiMedia2 "); [ -n "$CPD" ] || { echo "A6L_HW_FAIL no MultiMedia2 pcm"; exit 8; }
  apply common-off.txt > /dev/null; apply rx-only.txt; apply mm2-capture.txt; routes_show; apply headset.txt; apply headset-mic.txt
  for w in sine1k-m30dBFS-left-2s.wav sine1k-m30dBFS-right-2s.wav; do play $w; done
  duplex() { echo "A6L_DUPLEX $1: capture pcmC${C}D${2}c 4 s while playing pcmC${C}D${PB}p"; rm -f "$OUT/duplex-$1.wav"
    "$T/tinycap" "$OUT/duplex-$1.wav" -D "$C" -d "$2" -c 2 -r 48000 -b 16 -T 4 > "$OUT/tinycap-$1.log" 2>&1 & cp=$!
    sleep 1; play sine1k-m30dBFS-stereo-3s.wav; wait $cp; crc=$?; by=$(wc -c < "$OUT/duplex-$1.wav" 2>/dev/null)
    tail -n 2 "$OUT/tinycap-$1.log"; echo "A6L_CAPTURE_RC $1 $crc bytes=${by:-0}"; }
  duplex mm2 "$CPD"; [ "$crc" = 0 ] && [ "${by:-0}" -gt 100000 ] || FAILS=$((FAILS+1))
  verdict MEDIA
  echo "== informative: duplex on MultiMedia1 only (routes RX+TX of MM1)"
  $MIX -- "MultiMedia2 Mixer LPI_MI2S_TX_3" 0 > /dev/null 2>&1; apply fe-routes.txt > /dev/null
  F0=$FAILS; duplex mm1 "$PB"; FAILS=$F0
  [ "$crc" = 0 ] && [ "${by:-0}" -gt 100000 ] && echo "A6L_AU4_MM1_DUPLEX ok" || { echo "A6L_AU4_MM1_DUPLEX no (ROM capture must use MultiMedia2 = device 1)"; adm_err; }
  apply common-off.txt > /dev/null
  echo "ASK PIERRE: tone clearly audible at 0 dB left then right then both (not loud)?  laptop: python3 v75/audio4/wav-level.py audio4-out/duplex-*.wav";;
call)
  [ -n "$VDEV" ] || { echo "A6L_HW_FAIL no VoiceMMode1 pcm"; exit 8; }
  echo "A6L_AU4_CALL: modem must ALREADY be in an active call. Headset raw $RAW ($((RAW-84)) dB). Start with GAIN=0; +3/+6 only if Pierre asks."
  apply common-off.txt > /dev/null; apply headset.txt; apply headset-mic.txt
  sleep 2; "$T/a6l-q6voiced" -c "$C" -d "$VDEV" -r hold "${SECS:-60}" 2>&1 | tee "$OUT/q6voiced-call.log"
  klog | grep -iE "A6L_Q6VOICE|q6voice|q6cvp|q6mvm|q6cvs" | tail -n 15
  apply common-off.txt > /dev/null; "$T/a6l-q6voiced" -c "$C" routes 0 > /dev/null
  echo "ASK PIERRE: far end loud enough / comfortable at GAIN=$GAIN? any distortion? far end hears him?";;
capture)
  echo "A6L_AU5_CAPTURE: capture only, nothing is played. MIC=${MIC:-headset}"
  DD=/sys/kernel/debug/dynamic_debug/control
  [ -w $DD ] && for q in "file soc-pcm.c +p" "module q6asm +p" "module q6asm_dai +p" "module q6adm +p" "module q6afe +p" \
      "module q6afe_dai +p" "module q6routing +p" "module snd_soc_sm8250 +p"; do echo "$q" > $DD 2>/dev/null; done
  CARDN=$(sed -n "s/^ *$C \[.*\]: *[^ ]* - \(.*\)$/\1/p" /proc/asound/cards | head -n 1)
  AD=$(ls -d /sys/kernel/debug/asoc/* 2>/dev/null | grep -i "a6l" | head -n 1); echo "A6L_AU5 card=$C name='$CARDN' asoc_debugfs=${AD:-none}"
  MM2D=$(pcmdev "MultiMedia2 "); SECS=${SECS:-4}
  nzcount() { od -An -tx1 -v -j 44 -N 96000 "$1" 2>/dev/null | tr -s ' ' '\n' | grep -v '^$' | grep -vc '^00$'; }
  dapm_show() { [ -n "$AD" ] || return; find "$AD" -path '*dapm*' -type f 2>/dev/null | grep -E "/(MM_UL[12]|MultiMedia[12] (Mixer|Capture)|LPI_MI2S_TX_3|LPI TX3 MI2S Capture|AIF1 Capture|I2S TX1|DEC1 MUX|ADC[12]|ADC2 MUX|MIC BIAS [A-Za-z]*[12]|AMIC[12]|PDM_TX|LPASS_PDM_TX|CIC1 MUX|bias_level)\$" |
      while read -r f; do echo "  DAPM ${f#$AD/}: $(head -n 1 "$f")"; done; }
  dpcm_show() { [ -n "$AD" ] || return; for l in MultiMedia1 MultiMedia2; do [ -r "$AD/$l/state" ] && sed "s/^/  DPCM $l: /" "$AD/$l/state" | head -n 20; done; }
  cap() {  # variant fe_dev channels rate route_file
    v=$1; dv=$2; ch=$3; rt=$4; rf=$5
    [ -n "${ONLY:-}" ] && [ "$ONLY" != "$v" ] && return
    echo "== A6L_AU5 variant $v: pcmC${C}D${dv}c ${rt} Hz ${ch} ch, route $rf, mic ${MIC:-headset}, $SECS s"
    apply common-off.txt > /dev/null; mute_all; apply "$rf"
    case "${MIC:-headset}" in main) apply main-mic.txt;; *) apply headset-mic.txt;; esac
    for n in "MultiMedia1 Mixer LPI_MI2S_TX_3" "MultiMedia2 Mixer LPI_MI2S_TX_3" "Digital DEC1 MUX" "Digital CIC1 MUX" "ADC1 Volume" "ADC2 MUX" "ADC2 Volume"; do
      echo "  CTL $($MIX -- "$n" 2>&1 | head -n 1 | cut -c1-100)"; done
    echo "A6L_AU5_MARK_$v" > /dev/kmsg; f="$OUT/cap-$v.wav"; rm -f "$f"
    "$T/tinycap" "$f" -D "$C" -d "$dv" -c "$ch" -r "$rt" -b 16 -T "$SECS" > "$OUT/tinycap-$v.log" 2>&1 & cp=$!
    sleep 2
    echo "  -- during capture:"; for x in status hw_params sw_params; do sed "s/^/  PCM $x: /" /proc/asound/card$C/pcm${dv}c/sub0/$x 2>/dev/null | head -n 12; done
    dpcm_show; dapm_show
    wait $cp; crc=$?; by=$(wc -c < "$f" 2>/dev/null); nz=$(nzcount "$f")
    sed 's/^/  TINYCAP: /' "$OUT/tinycap-$v.log"
    dmesg | sed -n "/A6L_AU5_MARK_$v/,\$p" | grep -iE "asoc|dpcm|backend|BE |q6|afe|adm|asm|apr|routing|lpass|wcd|msm8916|error|fail" | head -n 60 | sed 's/^/  KLOG: /'
    exp=$((rt * ch * 2 * SECS)); echo "  bytes=${by:-0} expected~$exp nonzero_bytes_in_first_96k=${nz:-0} rc=$crc"
    if [ "${by:-0}" -gt $((exp / 2)) ] && [ "${nz:-0}" -gt 100 ]; then echo "A6L_AU5_CAP_PASS $v bytes=$by"; PASSV="$PASSV $v"
    else echo "A6L_AU5_CAP_FAIL $v bytes=${by:-0} nonzero=${nz:-0}"; fi
    apply common-off.txt > /dev/null
  }
  PASSV=""
  [ -n "$MM2D" ] && cap A "$MM2D" 2 48000 mm2-capture.txt
  [ -n "$MM2D" ] && cap B "$MM2D" 1 48000 mm2-capture.txt
  cap C "$PB" 2 48000 mm1-capture.txt
  [ -n "$MM2D" ] && cap D "$MM2D" 1 16000 mm2-capture.txt
  [ -w $DD ] && echo "file soc-pcm.c -p" > $DD 2>/dev/null
  echo "A6L_AU5_CAPTURE_SUMMARY passed:${PASSV:- none}   (pull $OUT: cap-*.wav, python3 v75/audio5/wav-level.py)"
  echo "ASK PIERRE: nothing to listen to; speak / tap near the headset mic (or the bottom mic with MIC=main) during each capture";;
off) "$T/a6l-q6voiced" -c "$C" routes 0; apply common-off.txt;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
echo "A6L_AU4_DONE mode=$MODE"
