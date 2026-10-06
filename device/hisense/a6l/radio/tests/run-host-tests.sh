#!/bin/sh
# Offline host build + run of the QMI/QRTR unit tests (no Android deps). Usage: sh run-host-tests.sh [-v]
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
CXX=${CXX:-g++}
OUT=${OUT:-/tmp/a6l-qmi-tests}
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/qmi_tests.cc -o "$OUT"
"$OUT" "$@"
# ril3: the real hal/ModemCore.cpp against android-base stubs + the fake modem (DSDS, radio power,
# SMS ack, call RAT, SSR fault injection)
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" "$D"/tests/fake_modem.cc \
    "$D"/tests/modemcore_tests.cc -o "$OUT-modemcore"
"$OUT-modemcore" "$@" 2>/dev/null
# volte2: IMSDCM (770) server + IMSA codec over the fake modem, and IMSA in the real ModemCore
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/volte2_tests.cc -o "$OUT-volte2"
"$OUT-volte2" "$@"
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" "$D"/tests/fake_modem.cc \
    "$D"/tests/volte2_modemcore_tests.cc -o "$OUT-volte2-modemcore"
"$OUT-volte2-modemcore" "$@" 2>/dev/null
# volte3: call domain (IMS/CS), typed dial, VoLTE indication register, audio RAT change, NAS VSIDs
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/volte3_tests.cc -o "$OUT-volte3"
"$OUT-volte3" "$@"
# pinsafe (27 Sep): SIM provisioning (primary GW session activated by the HAL) + PIN guard, radio power re-check
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" "$D"/tests/fake_modem.cc \
    "$D"/tests/simprov_tests.cc -o "$OUT-simprov"
"$OUT-simprov" "$@" 2>/dev/null
# volte5 (28 Sep): IMSS/IMSA bind + IMS service enable (stock qcril sequence), stock IMSDCM answers (28 Sep replay)
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/volte5_tests.cc -o "$OUT-volte5"
"$OUT-volte5" "$@"
# volte6 (29 Sep): byte-exact stock IMSDCM answers (29 Sep replay), GLOBAL instance destroy event, IMSS force/toggle,
# NAS voice domain preference (stock qcril TLVs), re-assert policy
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/volte6_tests.cc -o "$OUT-volte6"
"$OUT-volte6" "$@"
# r5 review fixes (28 Sep): F6 call mute client (a6l-q6voiced socket), F9 data roaming decision, F12 WMS broadcast config
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/hal" "$D"/qmi/src/*.cc "$D/hal/VoiceMute.cpp" "$D"/tests/fake_modem.cc \
    "$D"/tests/review5_tests.cc -o "$OUT-review5"
"$OUT-review5" "$@"
# r5 deep review fixes (28 Sep): F23 emergencyDial semantics / F24 per-call CLIR (mock modem only, no real call),
# F25 cell broadcast + ETWS receive, F26 stored SMS kept until Android acks, F27 one ordered SMS ack queue (ModemCore)
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" "$D"/tests/fake_modem.cc \
    "$D"/tests/review5_deep_tests.cc -o "$OUT-review5-deep"
"$OUT-review5-deep" "$@" 2>/dev/null
# r5 review pass2/deep (28 Sep): F14 RTM_NEWADDR + address install, F15 transactional data setup, F16 fatal QRTR
# receive error, F17 (data) WDS loss, F28 loss generation; then F16/F17/F18/F29 in the real ModemCore
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" "$D"/qmi/src/*.cc "$D"/tests/fake_modem.cc "$D"/tests/review5b_tests.cc -o "$OUT-review5b"
"$OUT-review5b" "$@"
# optional real-kernel check of the RTM_NEWADDR (user+net namespace with a dummy link; skipped when unavailable)
if unshare -rn sh -c 'ip link add d0 type dummy && ip link set d0 up' 2>/dev/null; then
    unshare -rn sh -c "ip link add d0 type dummy && ip link set d0 up && A6L_NETNS_IF=d0 '$OUT-review5b' 2>/dev/null" | sed 's/^/  (netns) /'
fi
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" "$D"/tests/fake_modem.cc \
    "$D"/tests/review5b_modemcore_tests.cc -o "$OUT-review5b-modemcore"
"$OUT-review5b-modemcore" "$@" 2>/dev/null
# r5 review round5 (28 Sep): F42 live modems / UICC apps, F43 per-card ICCID (real ModemCore, two fake cards), F44 PIN1
# vs UPIN id (no credential ever sent), F45 allowed network types, F46 MNC width on the wire, F47 APDU override
# contract, F48 FD query
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" -DA6L_HAL_DIR="\"$D/hal\"" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" \
    "$D"/tests/fake_modem.cc "$D"/tests/review5c_tests.cc -o "$OUT-review5c"
"$OUT-review5c" "$@" 2>/dev/null
# r5 review rounds 7/8 (28 Sep): F55 TTY honest, F56 finite DTMF stop result, F57 targeted reject, F58 last call fail
# cause (real ModemCore), F59 data reconfiguration refresh, F60 disconnect during setup, F61 NITZ age
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" -DA6L_HAL_DIR="\"$D/hal\"" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" \
    "$D"/tests/fake_modem.cc "$D"/tests/review7_tests.cc -o "$OUT-review7"
"$OUT-review7" "$@" 2>/dev/null
# r5 review round 11 (28 Sep): F17 follow-up (rejected VOICE/WMS registration kept pending + retried with backoff at
# startup and after an isolated return), F65 NITZ keeps missing time zone / DST unknown (real ModemCore)
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" \
    "$D"/tests/fake_modem.cc "$D"/tests/review11_tests.cc -o "$OUT-review11"
"$OUT-review11" "$@" 2>/dev/null
# r5 bug hunt round 2 "radio" (29 Sep): R1 modem lost during initModem (real ModemCore), R2 PIN guard cleared after
# PUK/change/SC lock (HAL contract), R3 unbound slot 2 never reaches SIM 1's NAS/WMS settings (HAL contract), R4
# a6l-imsdcm observes the IMS PDN's WDS session from before START (DISCONNECTED during setup, WDS loss)
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" -DA6L_HAL_DIR="\"$D/hal\"" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" \
    "$D"/tests/fake_modem.cc "$D"/tests/bughunt2_tests.cc -o "$OUT-bughunt2"
"$OUT-bughunt2" "$@" 2>/dev/null
# telephony-flows (29 Sep): USSD codec + dialogue + ModemCore indications, emergency (no SIM / PIN / limited service,
# test calls to real numbers refused, number list, EM reg states; fake modem only, never a real call), MMS second PDN,
# HAL source contract
$CXX -std=c++17 -O1 -g -Wall -Wextra -Werror -pthread -fsanitize=address,undefined \
    -I"$D/qmi/include" -I"$D/tests/hoststub" -DA6L_HAL_DIR="\"$D/hal\"" "$D"/qmi/src/*.cc "$D/hal/ModemCore.cpp" \
    "$D"/tests/fake_modem.cc "$D"/tests/telephony_flows_tests.cc -o "$OUT-telephony-flows"
"$OUT-telephony-flows" "$@" 2>/dev/null
