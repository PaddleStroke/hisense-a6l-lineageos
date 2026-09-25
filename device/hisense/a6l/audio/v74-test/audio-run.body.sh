# v74 audio: FIRST PLAYBACK through the pm660l internal codec (earpiece / wired headset) + mic capture. ATTENDED ONLY.
# usage: D=/tmp/audio MODE=dump|earpiece|headset|mic|off [LEVEL=1|2|3] sh run.sh
#   dump     : load stack if needed, write $OUT/tinymix-controls.txt + pcm info, check every control name used here. No sound.
#   earpiece : ~3 s 1 kHz tone on the earpiece (receiver). Phone NOT at the ear for the first run; listen from ~10 cm.
#   headset  : wired headset: left-only 2 s, right-only 2 s, both 3 s. Headphones NOT on the ears for the first run.
#   mic      : 5 s capture from the headset mic (if plugged) then 5 s from the main (handset) mic -> $OUT/*.wav (no sound played)
#   off      : apply mixer/common-off.txt only
# Hearing safety: tone file is -30 dBFS; codec digital gain LEVEL 1=-30 dB (default), 2=-24 dB, 3=-18 dB (hard cap).
# Stock uses 0 dB digital + EAR PA +6 dB, i.e. LEVEL 1 is ~60 dB below stock full scale. Never raise beyond LEVEL 3 without re-review.
# Control names come from the mainline driver sources; if `dump` reports MISSING names: cp -r $D/mixer /tmp/audio-mixer, fix the
# names there and re-run with MIXDIR=/tmp/audio-mixer (the bundle itself stays hash-checked). The -18 dB cap applies to MIXDIR too.
MODE=${MODE:-dump}; LEVEL=${LEVEL:-1}; MX=${MIXDIR:-$D/mixer}; OUT=${OUT:-/tmp/audio-out}; mkdir -p "$OUT"
case "$LEVEL" in 1) VOL=-30;; 2) VOL=-24;; 3) VOL=-18;; *) echo "A6L_HW_FAIL LEVEL must be 1..3"; exit 7;; esac
T="$D/bin"; chmod 755 "$T"/* 2>/dev/null
# device nodes (recovery has no ueventd for sound)
mknodes() { mkdir -p /dev/snd; for s in /sys/class/sound/*; do n=${s##*/}; [ -e "/dev/snd/$n" ] && continue; [ -r "$s/dev" ] || continue; mm=$(cat "$s/dev"); mknod "/dev/snd/$n" c "${mm%%:*}" "${mm##*:}" 2>/dev/null; done; }
card() { sed -n 's/^ *\([0-9]*\) \[.*\]: .*Hisense A6L.*/\1/p;s/^ *\([0-9]*\) \[.*\]: .*A6L.*/\1/p' /proc/asound/cards | head -n 1; }
if [ -z "$(card)" ]; then
  [ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  load; sleep 8
fi
C=$(card); [ -n "$C" ] || { echo A6L_HW_FAIL no Hisense A6L card; cat /proc/asound/cards; klog | grep -i "snd\|asoc\|q6\|codec" | tail -n 20; exit 8; }
mknodes; ls /dev/snd | tr '\n' ' '; echo
# PCM device numbers of the front ends, from /proc/asound/pcm ("CC-DD: MultiMedia1 ...")
pcmdev() { sed -n "s/^0*$C-0*\([0-9][0-9]*\): *$1[ (].*/\1/p" /proc/asound/pcm | head -n 1; }
PB=$(pcmdev MultiMedia1); CP=$(pcmdev MultiMedia2); [ -n "$CP" ] || CP=$PB
echo "A6L_AUDIO card=$C playback_dev=${PB:-?} capture_dev=${CP:-?}"
MIX="$T/tinymix -D $C"
# apply a mixer file: "control|value" lines; @VOL@ -> LEVEL dB. Reads back each control.
apply() { f="$MX/$1"; ok=0; bad=0
  while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac; v=$(echo "$v" | sed "s/@VOL@/$VOL/")
    case "$n" in *"Digital Volume") [ "$v" -le -18 ] 2>/dev/null || { echo "A6L_HW_FAIL refusing $n=$v (cap -18 dB)" >&2; exit 9; };; esac
    # "--" first: tinymix uses getopt_long, a negative value like -30 would otherwise be parsed as an option
    if $MIX -- "$n" "$v" > /dev/null 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "  SET_FAIL '$n' = $v" >&2; fi
    rb=$($MIX -- "$n" 2>&1 | head -n 1); echo "  $n := $v  -> $rb"
    case "$n" in *"Digital Volume") g=$(echo "$rb" | sed -n 's/^[^:]*: *\(-\{0,1\}[0-9][0-9]*\).*/\1/p')
      [ -n "$g" ] && [ "$g" -le -18 ] || { echo "A6L_HW_FAIL $n read back '$rb' (expected <= -18): muting and aborting" >&2; $MIX -- "$n" -84 > /dev/null 2>&1; exit 10; };; esac
  done < "$f"; echo "A6L_MIXER $1 ok=$ok fail=$bad"; }
dapm() { for f in /sys/kernel/debug/asoc/*/*/dapm/"$1" /sys/kernel/debug/asoc/*/dapm/"$1"; do [ -r "$f" ] && { echo "  dapm $1: $(head -n 1 "$f")"; break; }; done; }
play() { echo "A6L_PLAY $1 in 3 s (level $LEVEL = $VOL dB)"; sleep 3
  "$T/tinyplay" "$D/wav/$1" -D "$C" -d "$PB" > "$OUT/tinyplay-$1.log" 2>&1 & p=$!
  sleep 1; for w in "EAR PA" "HPHL PA" "HPHR PA" "RX1 INT" "RX2 INT" "LPI_MI2S_RX_0"; do dapm "$w"; done
  wait $p; rc=$?; cat "$OUT/tinyplay-$1.log" | tail -n 3; echo "A6L_PLAY_RC $1 $rc"; }
case "$MODE" in
dump)
  $MIX > "$OUT/tinymix-controls.txt" 2>&1; echo "controls: $(wc -l < "$OUT/tinymix-controls.txt") lines -> $OUT/tinymix-controls.txt"
  cat /proc/asound/pcm > "$OUT/asound-pcm.txt"; cat "$OUT/asound-pcm.txt"
  for d in $(sed -n "s/^0*$C-0*\([0-9][0-9]*\):.*/\1/p" /proc/asound/pcm); do echo "== pcm $d"; "$T/tinypcminfo" -D "$C" -d "$d" 2>&1 | head -n 30; done > "$OUT/tinypcminfo.txt"
  miss=0; for f in "$MX"/*.txt; do while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac
      o=$($MIX -- "$n" 2>&1); case "$o" in *"$n"*) ;; *) echo "MISSING ${f##*/}: $n ($o)"; miss=$((miss+1));; esac
    done < "$f"; done
  echo "A6L_MIXER_NAMES_MISSING=$miss"; grep -i "Digital Volume\|MUX\|EAR_S\|HPH\|LPI_MI2S\|MultiMedia[12] Mixer" "$OUT/tinymix-controls.txt" | cut -c1-120 | head -n 40
  klog | grep -i "q6\|apr\|asoc\|snd\|codec" | tail -n 10 ;;
earpiece) apply common-off.txt > /dev/null; apply earpiece.txt; play sine1k-m30dBFS-stereo-3s.wav; apply common-off.txt > /dev/null
  echo "ASK PIERRE: tone heard from the earpiece? (y/n, loudness)";;
headset) apply common-off.txt > /dev/null; apply headset.txt
  for w in sine1k-m30dBFS-left-2s.wav sine1k-m30dBFS-right-2s.wav sine1k-m30dBFS-stereo-3s.wav; do play $w; done; apply common-off.txt > /dev/null
  echo "ASK PIERRE: left only, then right only, then both? (y/n each)";;
mic) apply common-off.txt > /dev/null
  for m in headset-mic handset-mic; do apply $m.txt
    echo "A6L_CAPTURE $m 5 s: speak/tap near the mic now"; "$T/tinycap" "$OUT/cap-$m.wav" -D "$C" -d "$CP" -c 2 -r 48000 -b 16 -T 5 > "$OUT/tinycap-$m.log" 2>&1; rc=$?
    [ $rc = 0 ] || { echo "  stereo failed rc=$rc, retry mono"; "$T/tinycap" "$OUT/cap-$m.wav" -D "$C" -d "$CP" -c 1 -r 48000 -b 16 -T 5 > "$OUT/tinycap-$m.log" 2>&1; rc=$?; }
    tail -n 2 "$OUT/tinycap-$m.log"; echo "A6L_CAPTURE_RC $m $rc bytes=$(wc -c < "$OUT/cap-$m.wav" 2>/dev/null)"
    apply common-off.txt > /dev/null; done
  echo "pull $OUT/cap-*.wav to the laptop and check the level (python3 wav-level.py)";;
off) apply common-off.txt;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
klog | grep -i "q6afe\|q6asm\|q6routing\|asoc\|wcd\|error\|fail" | grep -v "Failed to add route" | tail -n 12
echo "A6L_AUDIO_DONE mode=$MODE"
