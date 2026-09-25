# v75 audio3: DPCM FE fix check (patched q6asm-dai.ko) + headset / mic / speaker tests + a6l-audio-route check. ATTENDED ONLY.
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/audio3 MODE=map|headset|mic|speaker|route|off [LEVEL=1|2|3] sh /tmp/audio3/run.sh
#   map     : NO SOUND. Loads the audio modules with the PATCHED q6asm-dai.ko (fresh boot needed if the old one is loaded),
#             sets only the MultiMedia1 routes (LPI_MI2S_RX_0 Audio Mixer MultiMedia1, MultiMedia1 Mixer LPI_MI2S_TX_3), all
#             gains muted, and probes all 4 PCM nodes. PASS = pcmC0D0p + pcmC0D0c open, pcmC0D1p + pcmC0D1c EINVAL.
#   headset : wired headset plugged, NOT worn for the first run: left 2 s, right 2 s, both 3 s (tone -30 dBFS, pcmC0D0p)
#   mic     : 5 s headset mic, then 5 s main mic, both on pcmC0D0c (MultiMedia1 capture) -> $OUT/cap-*.wav
#   speaker : only with a speaker DT (TFA9894 + TERT_MI2S) and snd-soc-tfa98xx: 2 s tone at -40 dBFS on pcmC0D0p
#   route   : runs the ROM routing daemon once (bin/a6l-audio-route -1 with mixer3/mixer_paths_a6l.xml, levels rewritten to
#             the LEVEL cap) and dumps the resulting controls; with PLAY=1 also plays the -30 dBFS stereo tone through it.
#   off     : everything silent (mixer3/common-off.txt)
# Hearing safety: RAW digital volume, dB = raw - 84. LEVEL 1 = 54 (-30 dB, default), 2 = 60 (-24 dB), 3 = 66 (-18 dB, hard cap).
# Every "Digital * Volume" write is refused above raw 66 and read back; a read-back above 66 mutes (raw 0) and aborts.
MODE=${MODE:-map}; LEVEL=${LEVEL:-1}; MX=$D/mixer3; OUT=${OUT:-/tmp/audio3-out}; mkdir -p "$OUT"
case "$LEVEL" in 1) RAW=54;; 2) RAW=60;; 3) RAW=66;; *) echo "A6L_HW_FAIL LEVEL must be 1..3"; exit 7;; esac
T="$D/bin"; chmod 755 "$T"/* 2>/dev/null
mknodes() { mkdir -p /dev/snd; for s in /sys/class/sound/*; do n=${s##*/}; [ -r "$s/dev" ] || continue; mm=$(cat "$s/dev")
  if [ -e "/dev/snd/$n" ]; then cur=$(ls -l "/dev/snd/$n" | sed -n 's/.* \([0-9]*\), *\([0-9]*\) .*/\1:\2/p'); [ "$cur" = "$mm" ] && continue; rm -f "/dev/snd/$n"; fi
  mknod "/dev/snd/$n" c "${mm%%:*}" "${mm##*:}"; done; }
card() { sed -n 's/^ *\([0-9]*\) \[.*\]: .*Hisense A6L.*/\1/p' /proc/asound/cards | head -n 1; }
if grep -q "^q6asm_dai " /proc/modules; then
  grep -q "q6asm_dai_of_xlate_dai_name" /proc/kallsyms || { echo "A6L_HW_FAIL the UNPATCHED q6asm-dai is loaded (earlier audio bundle in this boot): reboot to recovery, rerun"; exit 12; }
fi
if [ -z "$(card)" ]; then
  [ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  DTA=$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-audio 2>/dev/null)
  case "$DTA" in *speaker*) # speaker DT: patched LPI pinctrl (ter_mi2s functions) BEFORE load() (which then skips the stock one),
    # the stock TFA container, and the TFA driver (optional build: stub DAI if the amp fails, the card binds either way)
    fw; insmod "$D/modules/pinctrl-lpass-lpi.ko" 2>/dev/null; insmod "$D/extra/pinctrl-sdm660-lpass-lpi.ko" || echo "A6L_WARN patched LPI insmod failed";;
  esac
  load
  case "$DTA" in *speaker*) insmod "$D/extra/snd-soc-tfa98xx.ko" || echo "A6L_WARN tfa98xx insmod failed";; esac
  sleep 8
fi
grep -q "q6asm_dai_of_xlate_dai_name" /proc/kallsyms && echo "A6L_Q6ASM_PATCHED yes" || { echo "A6L_HW_FAIL patched q6asm-dai not active"; exit 12; }
C=$(card); [ -n "$C" ] || { echo A6L_HW_FAIL no Hisense A6L card; cat /proc/asound/cards; klog | grep -i "snd\|asoc\|q6\|codec\|tfa" | tail -n 20; exit 8; }
mknodes; PB=0; CP=0
echo "A6L_AUDIO3 card=$C fe=MultiMedia1 dev=0 mode=$MODE dt_audio=$(tr -d '\0' < /proc/device-tree/chosen/hisense,a6l-audio 2>/dev/null)"
MIX="$T/tinymix -D $C"
has() { o=$($MIX -- "$1" 2>&1 | head -n 1); case "$o" in "$1: "*|"$1:") return 0;; *) return 1;; esac; }
rawof() { $MIX -- "$1" 2>&1 | head -n 1 | sed -n "s/^.*: *\([0-9][0-9]*\).*/\1/p"; }
mute_all() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume" "Digital RX3 Digital Volume"; do $MIX -- "$n" 0 > /dev/null 2>&1; done; }
capchk() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume"; do g=$(rawof "$n")
    [ -n "$g" ] && [ "$g" -le 66 ] || { echo "A6L_HW_FAIL $n read back '$g' (expected <= 66): muting and aborting" >&2; mute_all; exit 10; }; done; }
apply() { f="$MX/$1"; ok=0; bad=0
  while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac; v=$(echo "$v" | sed "s/@RAW@/$RAW/")
    case "$n" in *"Digital Volume") [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 66 ] || { echo "A6L_HW_FAIL refusing $n=$v (cap raw 66 = -18 dB)" >&2; mute_all; exit 9; };; esac
    if ! has "$n"; then echo "  MISSING '$n'"; bad=$((bad+1)); continue; fi
    if $MIX -- "$n" "$v" > /dev/null 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "  SET_FAIL '$n' = $v"; fi
    echo "  $n := $v  -> $($MIX -- "$n" 2>&1 | head -n 1 | cut -c1-100)"
  done < "$f"; capchk; echo "A6L_MIXER $1 ok=$ok fail=$bad"; }
probe4() { for d in 0p 0c 1p 1c; do "$T/pcmprobe" "/dev/snd/pcmC${C}D$d" hw; done; }
play() { echo "A6L_PLAY $1 in 3 s (raw $RAW = $((RAW-84)) dB)"; sleep 3
  "$T/tinyplay" "$D/wav/$1" -D "$C" -d "$PB" > "$OUT/tinyplay-$1.log" 2>&1 & p=$!
  sleep 1; for w in "HPHL PA" "HPHR PA"; do f=$(ls -d /sys/kernel/debug/asoc/*/*/dapm/"$w" 2>/dev/null | head -n 1); [ -n "$f" ] && head -n 1 "$f"; done
  wait $p; rc=$?; tail -n 3 "$OUT/tinyplay-$1.log"; echo "A6L_PLAY_RC $1 $rc"
  [ $rc = 0 ] || "$T/pcmprobe" /dev/snd/pcmC${C}D${PB}p; }
mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug 2>/dev/null
case "$MODE" in
map)
  apply common-off.txt > /dev/null
  echo "== routing OFF"; probe4
  mute_all; apply fe-routes.txt
  echo "== routing ON (MultiMedia1 only)"; probe4 | tee "$OUT/map.txt"
  apply common-off.txt > /dev/null
  p0=$(grep -c "pcmC${C}D0[pc] open OK" "$OUT/map.txt"); e1=$(grep -c "pcmC${C}D1[pc] open errno=22" "$OUT/map.txt")
  [ "$p0" = 2 ] && [ "$e1" = 2 ] && echo "A6L_MAP_PASS MultiMedia1 = pcmC${C}D0 (playback+capture)" || echo "A6L_MAP_FAIL D0-open-ok=$p0 D1-einval=$e1 (see above)";;
headset) apply common-off.txt > /dev/null; apply fe-routes.txt; apply headset.txt
  for w in sine1k-m30dBFS-left-2s.wav sine1k-m30dBFS-right-2s.wav sine1k-m30dBFS-stereo-3s.wav; do play $w; done; apply common-off.txt > /dev/null
  echo "ASK PIERRE: left only, then right only, then both? (y/n each)";;
earpiece) echo "A6L_SKIP earpiece is physically broken on this unit";;
mic) apply common-off.txt > /dev/null; apply fe-routes.txt > /dev/null
  for m in headset-mic main-mic; do apply $m.txt
    echo "A6L_CAPTURE $m 5 s: speak/tap near the mic now"; "$T/tinycap" "$OUT/cap-$m.wav" -D "$C" -d "$CP" -c 2 -r 48000 -b 16 -T 5 > "$OUT/tinycap-$m.log" 2>&1; rc=$?
    [ $rc = 0 ] || { echo "  stereo failed rc=$rc, retry mono"; "$T/tinycap" "$OUT/cap-$m.wav" -D "$C" -d "$CP" -c 1 -r 48000 -b 16 -T 5 > "$OUT/tinycap-$m.log" 2>&1; rc=$?; }
    tail -n 2 "$OUT/tinycap-$m.log"; echo "A6L_CAPTURE_RC $m $rc bytes=$(wc -c < "$OUT/cap-$m.wav" 2>/dev/null)"
    [ $rc = 0 ] || "$T/pcmprobe" /dev/snd/pcmC${C}D${CP}c
    apply common-off.txt > /dev/null; apply fe-routes.txt > /dev/null; done
  apply common-off.txt > /dev/null; echo "laptop: python3 v75/audio3/wav-level.py audio3-out/cap-*.wav";;
speaker)
  has "TERT_MI2S_RX Audio Mixer MultiMedia1" || { echo "A6L_SKIP no TERT_MI2S route control (DT without the speaker link)"; exit 11; }
  grep -qi "tfa98xx" /sys/kernel/debug/asoc/components 2>/dev/null || { echo "A6L_HW_FAIL tfa98xx codec not in the card"; exit 11; }
  klog | grep -i "tfa" | tail -n 8
  apply common-off.txt > /dev/null; apply fe-routes.txt > /dev/null; apply speaker.txt
  for n in "TFA Profile" "Speaker TFA Profile"; do has "$n" && { $MIX -- "$n" music > /dev/null 2>&1; echo "  $n -> $($MIX -- "$n" | head -n 1 | cut -c1-100)"; }; done
  play sine1k-m40dBFS-stereo-2s.wav; apply common-off.txt > /dev/null
  klog | grep -i "tfa\|tert\|q6afe" | tail -n 15
  echo "ASK PIERRE: quiet 1 kHz tone from the loudspeaker? (y/n)";;
route)
  X=$OUT/mixer_paths_a6l.xml; sed "s/value=\"78\"/value=\"$RAW\"/g" "$MX/mixer_paths_a6l.xml" > "$X"
  grep -q 'value="7[0-9]"\|value="8[0-9]"' "$X" && { echo "A6L_HW_FAIL level rewrite failed"; exit 9; }
  "$T/a6l-audio-route" -1 -x "$X"; rc=$?; echo "A6L_ROUTE_RC $rc"; capchk
  for n in "Headphone Jack" "Mic Jack" "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" "MultiMedia1 Mixer LPI_MI2S_TX_3" "TERT_MI2S_RX Audio Mixer MultiMedia1" \
           "HPHL" "HPHR" "Digital RX1 Digital Volume" "Digital RX2 Digital Volume" "Digital DEC1 MUX" "ADC2 MUX"; do echo "  $($MIX -- "$n" 2>&1 | head -n 1 | cut -c1-100)"; done
  probe4 | grep "open"
  if [ "${PLAY:-0}" = 1 ]; then
    case "$($MIX -- "Headphone Jack" 2>&1 | head -n 1)" in *On*) play sine1k-m30dBFS-stereo-3s.wav;; *) echo "A6L_SKIP PLAY=1 needs the headset plugged (route would be the loudspeaker)";; esac
  fi
  apply common-off.txt > /dev/null;;
off) apply common-off.txt;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
klog | grep -i "q6afe\|q6asm\|q6routing\|asoc\|wcd\|tfa\|error\|fail" | tail -n 12
echo "A6L_AUDIO3_DONE mode=$MODE"
