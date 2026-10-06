#!/usr/bin/env bash
# r5 review round4 F37: host test of the audio HAL in-call volume path. Extracts a6lQ6voicedVolume() from
# audio/patches/0003-a6l-voice-volume.patch (A6L-F37-BEGIN..END) and runs it against the real a6l-q6voiced request
# handling (fake ALSA control). usage: bash audio/tests/test-telephony-volume.sh [workdir] -> A6L_TELEPHONY_VOLUME_TEST PASS
set -eu
S=$(cd "$(dirname "$0")" && pwd); W=${1:-$(mktemp -d)}; mkdir -p "$W"
tr -d '\r' < "$S/../patches/0003-a6l-voice-volume.patch" | sed -n '/^+\/\/ A6L-F37-BEGIN/,/^+\/\/ A6L-F37-END/p' | sed 's/^+//' > "$W/helper.inc"
[ -s "$W/helper.inc" ] || { echo "A6L_TELEPHONY_VOLUME_TEST FAIL (helper not found in patch)"; exit 1; }
gcc -std=gnu11 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-variable -O1 -g -fsanitize=address,undefined -c "$S/q6voiced_shim.c" -o "$W/shim.o"
g++ -std=gnu++17 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -pthread -I"$W" "$S/test_telephony_volume.cpp" "$W/shim.o" -o "$W/t"
"$W/t" "$W/sock" 2>/dev/null | grep -v '^A6L_Q6VOICED'
