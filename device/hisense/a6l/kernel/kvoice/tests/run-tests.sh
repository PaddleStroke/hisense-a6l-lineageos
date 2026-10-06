#!/usr/bin/env bash
# r5 bug hunt round2 audio: q6voice-common across an ADSP restart (host, ASan/UBSan). The source is rebuilt from the
# kernel series patches (full q6voice series creates the file, the ssr patch fixes it). PREFIX=1: without the ssr
# patch (must fail: AddressSanitizer use-after-free).
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); KV=$H/..; B=$(mktemp -d); trap 'rm -rf $B' EXIT
extract() {	# new file $1 from the full series patch
  awk -v f="+++ b/sound/soc/qcom/qdsp6/$1" '$0==f{on=1; getline; next} on&&/^diff --git/{exit} on&&/^\+/{print substr($0,2)} on&&/^ /{print substr($0,2)}' \
    <(tr -d '\r' < $KV/a6l-q6voice-full-7.2.3.patch) > $B/$1
}
mkdir -p $B/sound/soc/qcom/qdsp6
for f in q6voice-common.c q6voice-common.h q6voice.h; do extract $f; cp $B/$f $B/sound/soc/qcom/qdsp6/; done
if [ "${PREFIX:-0}" != 1 ]; then
  (cd $B && awk '/^--- a\/sound\/soc\/qcom\/qdsp6\/q6voice-common.c/{on=1} on' <(tr -d '\r' < $KV/a6l-q6voice-ssr-v75.patch) | patch -s -p1)
  cp $B/sound/soc/qcom/qdsp6/q6voice-common.c $B/
fi
grep -q "struct q6voice_service {" $B/q6voice-common.c
gcc -std=gnu11 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wno-unused-function \
  -I$H/stub -I$B -o $B/test_ssr $H/test_ssr.c
timeout 30 $B/test_ssr
