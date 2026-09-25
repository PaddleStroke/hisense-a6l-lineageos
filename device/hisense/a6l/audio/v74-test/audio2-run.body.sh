# v74 audio2: PCM-open diagnosis + corrected playback/capture tests (real "Digital " control names, RAW volumes). ATTENDED ONLY.
# usage: export PATH=/tmp/bin:$PATH; D=/tmp/audio2 MODE=diag|dump|headset|mic|speaker|off [LEVEL=1|2|3] sh /tmp/audio2/run.sh
#   diag    : NO SOUND. dynamic debug on soc-pcm/soc-dapm/q6asm-dai/q6routing/q6afe-dai/q6afe/q6asm, then for routing OFF and
#             routing ON (LPI_MI2S_RX_0 Audio Mixer MultiMedia1 = 1, all codec gains muted, HPH/EAR switches ZERO):
#             pcmprobe (real errno of open + HW_REFINE + HW_PARAMS, never PREPARE), DAPM widget graph, dpcm_state, APR devices,
#             /proc/asound sub0 status, dmesg since the marker. Everything goes to $OUT/diag.txt and stdout.
#   dump    : controls -> $OUT/tinymix-controls.txt; name check of every mixer2 file (exact "name:" prefix match)
#   headset : wired headset NOT worn for the first run: left 2 s, right 2 s, both 3 s  (tone -30 dBFS)
#   mic     : 5 s headset mic then 5 s main mic -> $OUT/cap-*.wav
#   speaker : ONLY with the V74 DT (TFA9894 + TERT_MI2S link) and snd-soc-tfa98xx loaded: 2 s tone at -40 dBFS
#   off     : mixer2/common-off.txt (+ speaker-off if present)
# Hearing safety: RAW digital volume, dB = raw - 84. LEVEL 1 = 54 (-30 dB, default), 2 = 60 (-24 dB), 3 = 66 (-18 dB, hard cap).
# Every "Digital * Volume" write is refused above raw 66 and read back; a read-back above 66 mutes (raw 0) and aborts.
MODE=${MODE:-diag}; LEVEL=${LEVEL:-1}; MX=${MIXDIR:-$D/mixer2}; OUT=${OUT:-/tmp/audio2-out}; mkdir -p "$OUT"
case "$LEVEL" in 1) RAW=54;; 2) RAW=60;; 3) RAW=66;; *) echo "A6L_HW_FAIL LEVEL must be 1..3"; exit 7;; esac
T="$D/bin"; chmod 755 "$T"/* 2>/dev/null
mknodes() { mkdir -p /dev/snd; for s in /sys/class/sound/*; do n=${s##*/}; [ -r "$s/dev" ] || continue; mm=$(cat "$s/dev")
  if [ -e "/dev/snd/$n" ]; then cur=$(ls -l "/dev/snd/$n" | sed -n 's/.* \([0-9]*\), *\([0-9]*\) .*/\1:\2/p'); [ "$cur" = "$mm" ] && continue; rm -f "/dev/snd/$n"; fi
  mknod "/dev/snd/$n" c "${mm%%:*}" "${mm##*:}"; done; }
card() { sed -n 's/^ *\([0-9]*\) \[.*\]: .*Hisense A6L.*/\1/p' /proc/asound/cards | head -n 1; }
if [ -z "$(card)" ]; then
  [ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo "A6L_HW_FAIL adsp not running (run the adsp bundle first)"; exit 5; }
  load; sleep 8
fi
C=$(card); [ -n "$C" ] || { echo A6L_HW_FAIL no Hisense A6L card; cat /proc/asound/cards; klog | grep -i "snd\|asoc\|q6\|codec" | tail -n 20; exit 8; }
mknodes
pcmdev() { sed -n "s/^0*$C-0*\([0-9][0-9]*\): *$1[ (].*/\1/p" /proc/asound/pcm | head -n 1; }
PB=$(pcmdev MultiMedia1); CP=$(pcmdev MultiMedia2); [ -n "$CP" ] || CP=$PB
echo "A6L_AUDIO2 card=$C playback_dev=${PB:-?} capture_dev=${CP:-?} mode=$MODE"
MIX="$T/tinymix -D $C"
# control exists <=> "tinymix -- <name>" prints "<name>: ..." (errors like "Invalid mixer control: <name>" also contain the name!)
has() { o=$($MIX -- "$1" 2>&1 | head -n 1); case "$o" in "$1: "*|"$1:") return 0;; *) return 1;; esac; }
rawof() { $MIX -- "$1" 2>&1 | head -n 1 | sed -n "s/^.*: *\([0-9][0-9]*\).*/\1/p"; }
mute_all() { for n in "Digital RX1 Digital Volume" "Digital RX2 Digital Volume" "Digital RX3 Digital Volume"; do $MIX -- "$n" 0 > /dev/null 2>&1; done; }
apply() { f="$MX/$1"; ok=0; bad=0
  while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac; v=$(echo "$v" | sed "s/@RAW@/$RAW/")
    case "$n" in *"Digital Volume") [ "$v" -ge 0 ] 2>/dev/null && [ "$v" -le 66 ] || { echo "A6L_HW_FAIL refusing $n=$v (cap raw 66 = -18 dB)"; mute_all; exit 9; };; esac
    if ! has "$n"; then echo "  MISSING '$n'"; bad=$((bad+1)); continue; fi
    if $MIX -- "$n" "$v" > /dev/null 2>&1; then ok=$((ok+1)); else bad=$((bad+1)); echo "  SET_FAIL '$n' = $v"; fi
    echo "  $n := $v  -> $($MIX -- "$n" 2>&1 | head -n 1 | cut -c1-100)"
    case "$n" in *"Digital Volume") g=$(rawof "$n")
      [ -n "$g" ] && [ "$g" -le 66 ] || { echo "A6L_HW_FAIL $n read back '$g' (expected <= 66): muting and aborting"; mute_all; exit 10; };; esac
  done < "$f"; echo "A6L_MIXER $1 ok=$ok fail=$bad"; }
AD=""; for a in /sys/kernel/debug/asoc/*; do [ -d "$a/dapm" ] || [ -d "$a/Internal MI2S Playback" ] && AD=$a; done
dapmw() { for w in "$@"; do f=""; for c in "$AD"/dapm/"$w" "$AD"/*/dapm/"$w"; do [ -r "$c" ] && { f=$c; break; }; done
  if [ -n "$f" ]; then echo "--- dapm [$w] ($f)"; cat "$f"; else echo "--- dapm [$w] NOT FOUND"; fi; done; }
play() { echo "A6L_PLAY $1 in 3 s (raw $RAW = $((RAW-84)) dB)"; sleep 3
  "$T/tinyplay" "$D/wav/$1" -D "$C" -d "$PB" > "$OUT/tinyplay-$1.log" 2>&1 & p=$!
  sleep 1; dapmw "HPHL PA" "HPHR PA" "EAR PA" 2>/dev/null | grep -i "^---\|power\|On\|Off" | head -n 12
  wait $p; rc=$?; tail -n 3 "$OUT/tinyplay-$1.log"; echo "A6L_PLAY_RC $1 $rc"
  [ $rc = 0 ] || "$T/pcmprobe" /dev/snd/pcmC${C}D${PB}p; }
diag_once() { # $1 = label
  echo "===== diag: $1"
  for d in "$PB:p" "$PB:c" "$CP:p" "$CP:c"; do "$T/pcmprobe" "/dev/snd/pcmC${C}D${d%%:*}${d##*:}" hw; done
  for s in /proc/asound/card$C/pcm${PB}p/sub0 /proc/asound/card$C/pcm${PB}c/sub0; do echo "--- $s"; for f in info status hw_params; do echo "[$f] $(tr '\n' ' ' < $s/$f 2>&1 | cut -c1-300)"; done; done
  for fe in "$AD"/MultiMedia1 "$AD"/MultiMedia2; do [ -r "$fe/dpcm_state" ] && { echo "--- $fe/dpcm_state"; cat "$fe/dpcm_state"; }; done
  dapmw "MultiMedia1 Playback" "MM_DL1" "LPI_MI2S_RX_0 Audio Mixer" "LPI_MI2S_RX_0" "LPI RX0 MI2S Playback" \
        "Digital I2S RX1" "Digital RX1 MIX1" "HPHL PA" "LPI_MI2S_TX_3" "MultiMedia2 Mixer" "MM_UL2" "MultiMedia2 Capture"
}
case "$MODE" in
diag)
  mount | grep -q debugfs || mount -t debugfs none /sys/kernel/debug
  [ -n "$AD" ] || { for a in /sys/kernel/debug/asoc/*; do [ -d "$a" ] && ls "$a" | grep -q "MultiMedia1" && AD=$a; done; }
  M=A6L_AUDIO2_DIAG_$$; echo "$M" > /dev/kmsg
  DC=/sys/kernel/debug/dynamic_debug/control
  if [ -w $DC ]; then for f in soc-pcm.c soc-dapm.c soc-core.c q6asm-dai.c q6routing.c q6afe-dai.c q6afe.c q6asm.c q6adm.c apr.c pcm_native.c; do echo "file $f +p" > $DC 2>/dev/null && echo "dyndbg on $f"; done
  else echo "A6L_WARN no dynamic_debug control (debugfs?)"; fi
  {
  echo "== cards/pcm"; cat /proc/asound/cards /proc/asound/pcm; ls -l /dev/snd; for s in /sys/class/sound/*; do echo "${s##*/} $(cat $s/dev 2>/dev/null)"; done
  echo "== APR / audio services"; ls /sys/bus/apr/devices/ 2>&1; for d in /sys/bus/apr/devices/*; do echo "$d -> $(basename $(readlink -f $d/driver 2>/dev/null) 2>/dev/null)"; done
  ls /sys/bus/platform/drivers/ | grep -i "q6\|qcom-q6\|lpass\|sm8250\|snd" | tr '\n' ' '; echo
  echo "== asoc debugfs ($AD)"; ls /sys/kernel/debug/asoc/ 2>&1; cat /sys/kernel/debug/asoc/components 2>/dev/null | head -n 40; cat /sys/kernel/debug/asoc/dais 2>/dev/null | head -n 60
  echo "== openers of /dev/snd"; for p in /proc/[0-9]*; do ls -l $p/fd 2>/dev/null | grep -q "/dev/snd/pcm" && echo "pid ${p#/proc/} $(cat $p/cmdline | tr '\0' ' ')"; done
  apply common-off.txt > /dev/null
  diag_once "routing OFF (expected: open fails, dyndbg 'no backend DAIs enabled for MultiMedia1')"
  mute_all; $MIX -- "LPI_MI2S_RX_0 Audio Mixer MultiMedia1" 1 > /dev/null; $MIX -- "MultiMedia2 Mixer LPI_MI2S_TX_3" 1 > /dev/null
  echo "route now: $($MIX -- 'LPI_MI2S_RX_0 Audio Mixer MultiMedia1' | head -n 1) / $($MIX -- 'MultiMedia2 Mixer LPI_MI2S_TX_3' | head -n 1) / RX1 vol raw $(rawof 'Digital RX1 Digital Volume') HPHL $($MIX -- HPHL | head -n 1)"
  diag_once "routing ON, all gains muted, HPH/EAR switches ZERO (no audible path)"
  apply common-off.txt > /dev/null
  echo "== dmesg since marker"; dmesg | sed -n "/$M/,\$p" | grep -v "dynamic_debug\|dyndbg" | tail -n 150
  } 2>&1 | tee "$OUT/diag.txt"
  [ -w $DC ] && for f in soc-pcm.c soc-dapm.c soc-core.c q6asm-dai.c q6routing.c q6afe-dai.c q6afe.c q6asm.c q6adm.c apr.c pcm_native.c; do echo "file $f -p" > $DC 2>/dev/null; done
  echo "A6L_DIAG_SUMMARY $(grep -c 'open OK' "$OUT/diag.txt") opens OK, errnos: $(grep -o 'open errno=[0-9]*' "$OUT/diag.txt" | sort | uniq -c | tr '\n' ' ')";;
dump)
  $MIX > "$OUT/tinymix-controls.txt" 2>&1; echo "controls: $(wc -l < "$OUT/tinymix-controls.txt") lines"
  miss=0; for f in "$MX"/*.txt; do while IFS='|' read -r n v; do case "$n" in ''|'#'*) continue;; esac
      has "$n" || { echo "MISSING ${f##*/}: $n"; miss=$((miss+1)); }; done < "$f"; done
  echo "A6L_MIXER_NAMES_MISSING=$miss (speaker*.txt names need the V74 DT)";;
headset) apply common-off.txt > /dev/null; apply headset.txt
  for w in sine1k-m30dBFS-left-2s.wav sine1k-m30dBFS-right-2s.wav sine1k-m30dBFS-stereo-3s.wav; do play $w; done; apply common-off.txt > /dev/null
  echo "ASK PIERRE: left only, then right only, then both? (y/n each)";;
earpiece) echo "A6L_SKIP earpiece is physically broken on this unit";;
mic) apply common-off.txt > /dev/null
  for m in headset-mic handset-mic; do apply $m.txt
    echo "A6L_CAPTURE $m 5 s: speak/tap near the mic now"; "$T/tinycap" "$OUT/cap-$m.wav" -D "$C" -d "$CP" -c 2 -r 48000 -b 16 -T 5 > "$OUT/tinycap-$m.log" 2>&1; rc=$?
    [ $rc = 0 ] || { echo "  stereo failed rc=$rc, retry mono"; "$T/tinycap" "$OUT/cap-$m.wav" -D "$C" -d "$CP" -c 1 -r 48000 -b 16 -T 5 > "$OUT/tinycap-$m.log" 2>&1; rc=$?; }
    tail -n 2 "$OUT/tinycap-$m.log"; echo "A6L_CAPTURE_RC $m $rc bytes=$(wc -c < "$OUT/cap-$m.wav" 2>/dev/null)"
    [ $rc = 0 ] || "$T/pcmprobe" /dev/snd/pcmC${C}D${CP}c
    apply common-off.txt > /dev/null; done;;
speaker)
  has "TERT_MI2S_RX Audio Mixer MultiMedia1" || { echo "A6L_HW_FAIL no TERT_MI2S route control"; exit 11; }
  grep -qi "tfa98xx" /sys/kernel/debug/asoc/components 2>/dev/null || { echo "A6L_HW_FAIL tfa98xx codec not in the card (V74 DT + snd-soc-tfa98xx.ko needed)"; exit 11; }
  apply common-off.txt > /dev/null; apply speaker.txt
  for n in "TFA Profile" "Speaker TFA Profile"; do has "$n" && { $MIX -- "$n" music > /dev/null 2>&1; echo "  $n -> $($MIX -- "$n" | head -n 1 | cut -c1-100)"; }; done
  play sine1k-m40dBFS-stereo-2s.wav; apply speaker-off.txt > /dev/null
  klog | grep -i "tfa\|tert\|q6afe" | tail -n 15
  echo "ASK PIERRE: quiet 1 kHz tone from the loudspeaker? (y/n)";;
off) apply common-off.txt; has "TERT_MI2S_RX Audio Mixer MultiMedia1" && apply speaker-off.txt;;
*) echo "A6L_HW_FAIL unknown MODE"; exit 7;;
esac
klog | grep -i "q6afe\|q6asm\|q6routing\|asoc\|wcd\|error\|fail" | tail -n 12
echo "A6L_AUDIO2_DONE mode=$MODE"
