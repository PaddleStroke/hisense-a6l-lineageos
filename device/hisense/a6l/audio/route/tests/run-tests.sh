#!/bin/bash
# a6l-audio-route host tests (r5 F7 + round6, 29 Sep 2026): daemon (main compiled out) + the REAL libaudioroute of the
# Lineage tree (expat) + a fake tinyalsa card generated from mixer_paths_a6l.xml. usage: bash run-tests.sh [<lineage tree>]
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); L=${1:-/home/a6l/android/a6l-lineage24}
AR=$L/system/media/audio_route; TA=$L/external/tinyalsa/include; W=$(mktemp -d); trap 'rm -rf $W' EXIT
mkdir -p $W/route/tests; cp $H/../a6l_audio_route.c $W/route/; cp $H/route_tests.c $W/route/tests/; cp -r $H/stub $W/route/tests/
cp $H/../../mixer_paths_a6l.xml $W/route/; find $W -type f -exec sed -i 's/\r$//' {} +
F="-std=gnu11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer"
gcc $F -w -I$AR/include -I$TA -I$W/route/tests/stub -c $AR/audio_route.c -o $W/audio_route.o
gcc $F -Wall -Wextra -Werror -Wno-unused-function -I$AR/include -I$TA -I$W/route/tests/stub -c $W/route/tests/route_tests.c -o $W/t.o
gcc $F $W/t.o $W/audio_route.o -lexpat -o $W/t
# r5 review F62: the exact HAL publisher (added lines of patch 0002) + the daemon's exact decide()
python3 - $H/../../patches/0002-a6l-call-route-mute.patch $W/route/a6l_audio_route.c $W <<'PY2'
import sys
def extract(s, sig):
    i = s.index(sig); j = s.index('{', i); d = 0
    for k in range(j, len(s)):
        d += s[k] == '{'; d -= s[k] == '}'
        if d == 0: return s[i:k + 1]
patch = open(sys.argv[1]).read().replace('\r', '')
added = '\n'.join(l[1:] for l in patch.splitlines() if l.startswith('+') and not l.startswith('+++'))
route = open(sys.argv[2]).read()
open(sys.argv[3] + '/publish_methods.inc', 'w').write('\n\n'.join(extract(added, s) for s in
    ('std::string a6lOutName(', 'std::string a6lInName(', 'void ModulePrimary::a6lPublishCallRoute(')))
open(sys.argv[3] + '/decide.inc', 'w').write('\n\n'.join([extract(route, 'struct route_state') + ';',
    extract(route, 'struct route_choice') + ';', extract(route, 'static int has_dev('), extract(route, 'static void decide(')]))
PY2
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -Wall -Wno-unused-function -I$W -o $W/tp $H/publish_tests.cc
cd $W/route/tests; set +e; $W/tp; rp=$?; $W/t 2> $W/err.log; rc=$?; grep -E "^FAIL|^  case|lost-control" $W/err.log | head -40; [ $rp = 0 ] || rc=1; exit $rc
