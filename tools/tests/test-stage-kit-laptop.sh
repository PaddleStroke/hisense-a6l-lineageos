#!/usr/bin/env bash
# Host test for tools/stage-rom-v2-kit-laptop.sh (bug hunt round2 install-tools, 29 Sep 2026). No network, no laptop, no phone:
# ssh/scp are replaced by local fakes working in a scratch "laptop home". PREFIX=1 runs the pre-fix remote logic (embedded
# copy, same fakes) to show the capture loss.   usage: bash tools/tests/test-stage-kit-laptop.sh
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd); S=$HERE/stage-rom-v2-kit-laptop.sh
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
fail=0; ok() { echo "PASS $1"; }; bad() { echo "FAIL $1"; fail=1; }
cat > $T/fakessh <<'X'
#!/usr/bin/env bash
shift   # host
HOME=$FAKEHOME bash -c "$1"
X
cat > $T/fakescp <<'X'
#!/usr/bin/env bash
src=$3; dst=${4#a6l-laptop:}; cp -r "$src" "$FAKEHOME/$dst" || exit 1
[ -n "${CORRUPT:-}" ] && echo x >> "$FAKEHOME/$dst/images/system.erofs"; exit 0
X
chmod +x $T/fakessh $T/fakescp
mkkit() { local k=$T/base/kit-$1/rom-v2; rm -rf $T/base/kit-$1; mkdir -p $k/images
  echo "$1 system" > $k/images/system.erofs; echo tool > $k/Write-LaptopRomV1.py
  ( cd $k/images && sha256sum system.erofs > SHA256SUMS ); }
if [ "${PREFIX:-0}" = 1 ]; then   # the pre-fix remote logic (29 Sep 2026 copy), same fakes
  cat > $T/old.sh <<'X'
set -uo pipefail
TAG=$1; N=${2:-rom-v2}; K=$A6L_KIT_BASE/kit-$TAG/rom-v2; SSH=$A6L_KIT_SSH; SCP=$A6L_KIT_SCP
[ -f $K/images/SHA256SUMS ] || { echo "KIT_MISSING $K"; exit 1; }
WS=$A6L_KIT_WS/rom-v2-kit-$TAG; rm -rf $WS; mkdir -p $WS; cp -r $K/. $WS/
( cd $K && find . -type f ! -name KIT-SHA256SUMS | sort | xargs sha256sum ) > $WS/KIT-SHA256SUMS
$SSH a6l-laptop "rm -rf ~/A6L-usb-20260915/$N.new" < /dev/null
W=$WS
$SCP -r -q "$W" "a6l-laptop:A6L-usb-20260915/$N.new" < /dev/null || { echo "SCP_FAIL"; exit 2; }
$SSH a6l-laptop "cd ~/A6L-usb-20260915/$N.new && sha256sum -c --quiet KIT-SHA256SUMS && (cd images && sha256sum -c --quiet SHA256SUMS) && echo LAPTOP_SHA_OK && cd .. && rm -rf $N.prev && { [ -d $N ] && mv $N $N.prev; true; } && mv $N.new $N && ls -la $N $N/images" < /dev/null | tr -d '\r'
rm -rf $WS
echo "== $(date) STAGE_KIT_DONE $TAG -> ~/A6L-usb-20260915/$N"
X
  S=$T/old.sh
fi
export FAKEHOME=$T/home A6L_KIT_SSH=$T/fakessh A6L_KIT_SCP=$T/fakescp A6L_KIT_WSLPATH=echo A6L_KIT_BASE=$T/base A6L_KIT_WS=$T/ws
mkdir -p $FAKEHOME/A6L-usb-20260915
L=$FAKEHOME/A6L-usb-20260915
run() { bash $S "$@" > $T/out 2>&1; echo $?; }
# 1 fresh stage
mkkit r5; rc=$(run r5 rom-r5)
[ $rc = 0 ] && grep -q STAGE_KIT_DONE $T/out && grep -q "r5 system" $L/rom-r5/images/system.erofs && ok fresh || { bad fresh; cat $T/out; }
# 2 install happened: capture inside the kit dir; re-stage (twice) must not move or delete it
mkdir -p $L/rom-r5/capture-rom-v1-install/edl; echo stock > $L/rom-r5/capture-rom-v1-install/edl/system.bin
mkkit r5; rc1=$(run r5 rom-r5); rc2=$(run r5 rom-r5)
if [ -f $L/rom-r5/capture-rom-v1-install/edl/system.bin ]; then ok capture_kept_in_place
elif [ -f $L/rom-r5.prev/capture-rom-v1-install/edl/system.bin ]; then bad "capture_moved_to_prev (restore tool no longer finds it)"
else bad "capture_DELETED (rc $rc1 $rc2)"; fi
[ "$rc1" = 4 ] && grep -q KIT_HAS_CAPTURE $T/out && ! grep -q STAGE_KIT_DONE $T/out && ok refused_rc4 || bad "refused_rc4 ($rc1/$rc2)"
# 3 capture in .prev (earlier re-stage moved it) -> refused, .prev kept
mv $L/rom-r5 $L/rom-r5.prev 2>/dev/null; mkdir -p $L/rom-r5; mkkit r5; rc=$(run r5 rom-r5)
[ -f $L/rom-r5.prev/capture-rom-v1-install/edl/system.bin ] && [ "$rc" = 4 ] && ok prev_capture_refused || bad "prev_capture_refused rc=$rc"
# 4 re-stage of a capture-free dir works and keeps the old kit as .prev
mkkit r6; run r6 rom-r6 >/dev/null; mkkit r6; echo v2 >> $T/base/kit-r6/rom-v2/Write-LaptopRomV1.py; rc=$(run r6 rom-r6)
[ "$rc" = 0 ] && grep -q v2 $L/rom-r6/Write-LaptopRomV1.py && [ -d $L/rom-r6.prev ] && ok restage_no_capture || bad "restage_no_capture rc=$rc"
# 5 transfer corruption: laptop sha check fails -> exit 3, no STAGE_KIT_DONE, old kit untouched
mkkit r7; run r7 rom-r7 >/dev/null; before=$(cat $L/rom-r7/images/system.erofs); mkkit r7; echo new >> $T/base/kit-r7/rom-v2/images/system.erofs
( cd $T/base/kit-r7/rom-v2/images && sha256sum system.erofs > SHA256SUMS ); rc=$(CORRUPT=1 run r7 rom-r7)
[ "$rc" = 3 ] && ! grep -q STAGE_KIT_DONE $T/out && [ "$(cat $L/rom-r7/images/system.erofs)" = "$before" ] && ok corrupt_copy_fails || { bad "corrupt_copy_fails rc=$rc"; tail -3 $T/out; }
# 6 bad dir names
rc=$(run r5 ../x); [ "$rc" = 2 ] && ok bad_name || bad "bad_name rc=$rc"
echo "STAGE_KIT_LAPTOP_TEST $([ $fail = 0 ] && echo PASS || echo FAIL)"; exit $fail
