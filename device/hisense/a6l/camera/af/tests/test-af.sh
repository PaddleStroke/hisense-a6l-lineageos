#!/bin/bash
# A6L camera phase 3 (AF) offline tests. Host only (gcc/g++/python3), no phone. Prints AF_TESTS_PASS / AF_TESTS_FAIL.
#   1. AF search logic vs simulated focus curves (ASan/UBSan)          -> AF_SEARCH_PASS
#   2. a6l_afsharp / a6l_afotp build -Wall -Wextra -Werror                -> BUILD_PASS
#   3. a6l_afsharp: synthetic texture, box blur 0..4, XRGB8888 + ABGR8888 + RAW10P (+ padded stride):
#      strictly decreasing with blur, exposure-invariant (x0.5) within 10 %, bad args rejected
#   4. a6l_afotp -F: valid AF block -> YAML line, bad checksum / flag 0 / inverted / short -> invalid
#   5. patches 0101-0104 present, Af tuning keys == keys read by af.cpp, yaml parses, run-af.sh sh -n,
#      run-af.sh has no flash/partition writes
#   6. optional (LIBCAMERA_TREE=<src-af>): patches == tree commits (git format-patch diff)
set -u
T=$(cd "$(dirname "$0")" && pwd); AF=$(dirname "$T"); W=$(mktemp -d); trap 'rm -rf $W' EXIT
fails=0; pass() { echo "PASS $*"; }; fail() { echo "FAIL $*"; fails=$((fails+1)); }

# 1
if g++ -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -I$AF/libcamera/src \
     $T/test_af_search.cpp -o $W/tafs 2> $W/tafs.log && $W/tafs > $W/tafs.out 2>&1; then
  pass "af_search: $(tail -1 $W/tafs.out) ($(head -1 $W/tafs.out))"
else cat $W/tafs.log $W/tafs.out 2>/dev/null | tail -20; fail af_search; fi

# 2
ok=1
for t in a6l_afsharp a6l_afotp; do
  gcc -O2 -Wall -Wextra -Werror -fsanitize=address,undefined -o $W/$t $AF/tools/$t.c 2> $W/$t.log || { cat $W/$t.log; ok=0; }
done
[ $ok = 1 ] && pass "BUILD tools" || fail "BUILD tools"

# 3
python3 - "$W" <<'PY' || fail "synthetic image generation"
import random, struct, sys
w, h = 256, 192
W = sys.argv[1]
random.seed(7)
# texture: random rectangles + fine checker (focus-sensitive)
img = [[0.0]*w for _ in range(h)]
for y in range(h):
    for x in range(w):
        img[y][x] = 90 + 60 * (((x // 3) + (y // 3)) % 2)
for _ in range(60):
    x0, y0 = random.randrange(w), random.randrange(h)
    x1, y1 = min(w, x0 + random.randrange(4, 40)), min(h, y0 + random.randrange(4, 40))
    v = random.randrange(20, 235)
    for y in range(y0, y1):
        for x in range(x0, x1):
            img[y][x] = v
def blur(a, r):
    if r == 0: return a
    out = [[0.0]*w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            s = n = 0
            for dy in range(-r, r+1):
                yy = min(h-1, max(0, y+dy))
                for dx in range(-r, r+1):
                    xx = min(w-1, max(0, x+dx)); s += a[yy][xx]; n += 1
            out[y][x] = s / n
    return out
for r in range(5):
    for scale, tag in ((1.0, ''), (0.5, 'dim')):
        b = blur(img, r)
        px = [[max(0, min(255, int(v * scale + 0.5))) for v in row] for row in b]
        with open(f'{W}/x{tag}{r}.bin', 'wb') as f:          # XRGB8888 little endian: B G R X, G = byte 1
            for row in px:
                f.write(b''.join(struct.pack('BBBB', v, v, v, 255) for v in row))
        with open(f'{W}/a{tag}{r}.bin', 'wb') as f:          # ABGR8888 + 64 bytes stride padding
            for row in px:
                f.write(b''.join(struct.pack('BBBB', v, v, v, 255) for v in row) + b'\0' * 64)
        with open(f'{W}/r{tag}{r}.bin', 'wb') as f:          # RAW10 CSI-2 packed, all 4 Bayer sites = v
            for row in px:
                out = bytearray()
                for g in range(0, w, 4):
                    q = [v * 4 for v in row[g:g+4]]
                    out += bytes([q[0] >> 2, q[1] >> 2, q[2] >> 2, q[3] >> 2,
                                  (q[0] & 3) | (q[1] & 3) << 2 | (q[2] & 3) << 4 | (q[3] & 3) << 6])
                f.write(bytes(out))
print("generated")
PY
sharp() { $W/a6l_afsharp "$@" | sed 's/.* sharp=\([0-9.]*\).*/\1/'; }
for spec in "x XRGB8888 0" "a ABGR8888 1088" "r RAW10P 0"; do
  set -- $spec; p=$1; f=$2; st=$3; prev=""; mono=1; vals=""
  for r in 0 1 2 3 4; do
    v=$(sharp -f $f -w 256 -h 192 -s $st -p $r $W/$p$r.bin); vals="$vals $v"
    [ -n "$prev" ] && python3 -c "import sys; sys.exit(0 if $v < $prev else 1)" || { [ -n "$prev" ] && mono=0; }
    prev=$v
  done
  [ $mono = 1 ] && pass "afsharp $f monotonic:$vals" || fail "afsharp $f not monotonic:$vals"
  a=$(sharp -f $f -w 256 -h 192 -s $st $W/${p}1.bin); b=$(sharp -f $f -w 256 -h 192 -s $st $W/${p}dim1.bin)
  python3 -c "import sys; a,b=$a,$b; sys.exit(0 if abs(a-b)/a < 0.10 else 1)" \
    && pass "afsharp $f exposure-invariant ($a vs $b)" || fail "afsharp $f exposure ($a vs $b)"
done
$W/a6l_afsharp -f NOPE -w 256 -h 192 $W/x0.bin > /dev/null 2>&1; [ $? = 2 ] && pass "afsharp bad fmt rc=2" || fail "afsharp bad fmt"
$W/a6l_afsharp -f XRGB8888 -w 512 -h 192 $W/x0.bin > /dev/null 2>&1; [ $? = 2 ] && pass "afsharp short file rc=2" || fail "afsharp short"
$W/a6l_afsharp -f XRGB8888 -w 256 -h 192 $W/missing.bin > /dev/null 2>&1; [ $? = 3 ] && pass "afsharp missing rc=3" || fail "afsharp missing"
$W/a6l_afsharp -f RAW10P -w 250 -h 192 $W/r0.bin > /dev/null 2>&1; [ $? = 2 ] && pass "afsharp raw10 w%4 rc=2" || fail "afsharp raw10 w%4"

# 4
python3 - "$W" <<'PY'
import sys
W = sys.argv[1]
def dump(name, flag=1, macro=640, inf=250, bad=False, n=0x0F27):
    b = bytearray(n)
    if n > 0x720:
        b[0x708] = flag
        b[0x709:0x70b] = macro.to_bytes(2, 'big'); b[0x70b:0x70d] = inf.to_bytes(2, 'big')
        for i in range(0x70d, 0x720): b[i] = (i * 7) & 0xff
        b[0x720] = (sum(b[0x709:0x720]) + (1 if bad else 0)) & 0xff
    open(f'{W}/{name}.bin', 'wb').write(b)
dump('otp_ok'); dump('otp_csum', bad=True); dump('otp_flag', flag=0); dump('otp_inv', macro=200, inf=600)
dump('otp_short', n=0x700)
PY
out=$($W/a6l_afotp -F $W/otp_ok.bin); rc=$?
[ $rc = 0 ] && echo "$out" | grep -q "A6L_AF_OTP_YAML infinityDac: 250 macroDac: 640" && pass "afotp valid: $(echo "$out" | head -1)" || fail "afotp valid rc=$rc $out"
for c in csum flag inv short; do
  $W/a6l_afotp -F $W/otp_$c.bin > $W/o.txt; rc=$?
  [ $rc = 1 ] && ! grep -q YAML $W/o.txt && pass "afotp $c -> invalid" || fail "afotp $c rc=$rc $(cat $W/o.txt)"
done
$W/a6l_afotp > /dev/null 2>&1; [ $? = 2 ] && pass "afotp usage rc=2" || fail "afotp usage"
$W/a6l_afotp -b 1 -F $W/otp_ok.bin > /dev/null 2>&1; [ $? = 2 ] && pass "afotp -b and -F exclusive" || fail "afotp -b -F"

# 5
n=$(ls $AF/libcamera/patches/010[1-4]-*.patch 2>/dev/null | wc -l)
[ "$n" = 4 ] && pass "patches 0101-0104 present" || fail "patches: $n of 4"
keys_cpp=$(grep -o 'tuningData\["[a-zA-Z]*"\]' $AF/libcamera/src/af.cpp | sed 's/.*\["\(.*\)"\]/\1/' | sort -u | tr '\n' ' ')
keys_yaml=$(python3 -c "
import yaml
d = yaml.safe_load(open('$AF/libcamera/data/imx576_a6l.yaml'))
af = [a['Af'] for a in d['algorithms'] if isinstance(a, dict) and 'Af' in a][0]
print(' '.join(sorted(af)) + ' ')" 2>&1)
[ "$keys_cpp" = "$keys_yaml" ] && pass "Af tuning keys match ($keys_cpp)" || fail "Af keys cpp=[$keys_cpp] yaml=[$keys_yaml]"
python3 -c "
import yaml,sys
d=yaml.safe_load(open('$AF/libcamera/data/imx576_a6l.yaml'))
names=[list(a)[0] if isinstance(a,dict) else a for a in d['algorithms']]
sys.exit(0 if names[-1]=='Af' and 'Agc' in names else 1)" && pass "imx576_a6l.yaml: Af last, Agc present" || fail "yaml algorithm order"
blk=$(grep -c "Af:" $AF/libcamera/data/af-block.yaml); [ "$blk" = 1 ] && pass "af-block.yaml" || fail "af-block.yaml"
for y in $AF/af-script-auto.yaml $AF/af-script-continuous.yaml; do
  python3 -c "import yaml; d=yaml.safe_load(open('$y')); assert 'frames' in d" && pass "script $(basename $y)" || fail "script $y"
done
sh -n $AF/run-af.sh && pass "run-af.sh sh -n" || fail "run-af.sh syntax"
if grep -nE '(^|[^a-z_])(dd|mkfs|fastboot|flash_image|blkdiscard)( |$)|of=/dev|/dev/block|> */dev/(mmc|sd)' $AF/run-af.sh; then
  fail "run-af.sh writes devices"; else pass "run-af.sh: no flash/partition writes"; fi

# 6
if [ -n "${LIBCAMERA_TREE:-}" ] && [ -d "$LIBCAMERA_TREE/.git" ]; then
  git -C $LIBCAMERA_TREE format-patch -q --no-signature --start-number 101 -o $W/fp HEAD~4
  same=1; for p in $AF/libcamera/patches/010[1-4]-*.patch; do
    q=$W/fp/$(basename $p); diff <(grep -v '^From [0-9a-f]\{40\}\|^Date:' $p) <(grep -v '^From [0-9a-f]\{40\}\|^Date:' $q) > /dev/null || same=0; done
  [ $same = 1 ] && pass "patches == $LIBCAMERA_TREE commits" || fail "patches differ from $LIBCAMERA_TREE"
fi

[ $fails = 0 ] && echo AF_TESTS_PASS || echo "AF_TESTS_FAIL ($fails)"
exit $fails
