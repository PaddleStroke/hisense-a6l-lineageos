#!/bin/bash
# r5 review F49 host test (29 Sep 2026): unchanged a6l_gpio_vib.c (userspace kstub/) + unchanged QTI VibratorOL
# LedVibratorDevice methods extracted from the Lineage tree. usage: bash run-tests.sh [<lineage tree>] -> A6L_VIB_F49_TEST PASS
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); L=${1:-/home/a6l/android/a6l-lineage24}
SRC=$L/vendor/qcom/opensource/vibrator/aidl/VibratorOL/Vibrator.cpp; W=$(mktemp -d); trap 'rm -rf $W' EXIT
python3 - "$SRC" "$W/led_methods.inc" <<'PY'
import sys
s=open(sys.argv[1]).read(); out=[]
def extract(sig):
    i=s.index(sig); j=s.index('{',i); d=0
    for k in range(j,len(s)):
        d+= s[k]=='{'; d-= s[k]=='}'
        if d==0: return s[i:k+1]
for sig in ('LedVibratorDevice::LedVibratorDevice()','int LedVibratorDevice::write_value(','int LedVibratorDevice::on(','int LedVibratorDevice::off()'):
    out.append(extract(sig))
open(sys.argv[2],'w').write('\n\n'.join(out)+'\n')
# contract checks on the selection code (text): LED backend first in on/off, caps = ON_CALLBACK only when LED detected
assert 'if (ledVib.mDetected)\n        ret = ledVib.on(timeoutMs);' in s
assert 'if (ledVib.mDetected)\n        ret = ledVib.off();' in s
c=extract('ndk::ScopedAStatus VibratorOL::getCapabilities(')
assert c.index('if (ledVib.mDetected)') < c.index('CAP_AMPLITUDE_CONTROL')
assert '"/sys/class/leds/vibrator"' in s
print('ok   VibratorOL selection contract (LED backend first, CAP_ON_CALLBACK only)')
PY
F="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Wall -Wno-unused-function -Wno-sign-compare"
tr -d '\r' < $H/../a6l_gpio_vib.c > $W/a6l_gpio_vib.c; mkdir -p $W/t; cp -r $H/kstub $H/test_vib_drv.c $H/vib_test_api.h $H/test_vib_hal.cc $W/t/
gcc -std=gnu11 $F -I$W/t/kstub -c $W/t/test_vib_drv.c -o $W/drv.o
g++ -std=c++17 $F -I$W -I$W/t -c $W/t/test_vib_hal.cc -o $W/hal.o
g++ $F $W/drv.o $W/hal.o -o $W/t_vib -lpthread
ASAN_OPTIONS=detect_leaks=0 $W/t_vib   # devm_* stand-ins never free
