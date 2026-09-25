#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# Rear e-ink frontlight attended test (agent dualux, docs/dualux-20260925.md §7). V74 recovery, as root, from the
# bundle dir D (default /tmp/fl). Always `export PATH=/tmp/bin:$PATH` first.
#   MODE=probe            read-only: leds/backlight classes, loaded modules, PM660L GPIO6 + LPG ch4 registers, pwm debugfs
#   MODE=load             insmod leds-qcom-lpg, leds-pwm, a6l_fl_ovl -> /sys/class/leds/epd-backlight (brightness 0 = dark)
#   MODE=step LEVEL=n     brightness n (0..255, refused above 40 unless FORCE=1) for HOLD seconds (default 5), then 0
#   MODE=off              brightness 0
# Nothing here writes a PMIC register directly; only the drivers do, through the DT overlay (stock LPG/GPIO settings).
D=${D:-/tmp/fl}; MODE=${MODE:-probe}; L=/sys/class/leds/epd-backlight
say() { echo "A6L_FL $*"; }
regs() {	# read-only regmap dumps of the PM660L ranges we touch (GPIO6 @0xc500 on SID 2, LPG ch4 @0xb400 on SID 3)
    [ -d /sys/kernel/debug/regmap ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
    for r in /sys/kernel/debug/regmap/*; do
        [ -f "$r/registers" ] || continue
        n=$(basename "$r")
        case "$n" in *0-02*|*0-03*|*spmi*|*pm660l*) ;; *) continue;; esac
        grep -iE '^(c5[0-4][0-9a-f]|c5[4-5][0-9a-f]|b4[0-7][0-9a-f]|b4[dD][0-9a-f]|b4[eE][0-9a-f]):' "$r/registers" 2>/dev/null | sed "s|^|A6L_FL_REG $n |" | head -80
    done
}
case "$MODE" in
probe)
    say "leds: $(ls /sys/class/leds 2>/dev/null | tr '\n' ' ')"; say "backlight: $(ls /sys/class/backlight 2>/dev/null | tr '\n' ' ')"
    for b in /sys/class/backlight/*; do [ -d "$b" ] && say "bl $(basename $b) max=$(cat $b/max_brightness) cur=$(cat $b/brightness) power=$(cat $b/bl_power) scale=$(cat $b/scale 2>/dev/null)"; done
    say "modules: $(grep -E 'lpg|leds_pwm|a6l_fl' /proc/modules | cut -d' ' -f1 | tr '\n' ' ')"
    say "dt lpg status: $(cat /proc/device-tree/soc@0/spmi@800f000/pmic@3/pwm/status 2>/dev/null || find /proc/device-tree -path '*pmic@3/pwm/status' -exec cat {} \; 2>/dev/null)"
    cat /sys/kernel/debug/pwm 2>/dev/null | sed 's/^/A6L_FL_PWM /'
    regs; say PROBE_DONE ;;
load)
    cd "$D" || exit 1
    grep -q '^leds_qcom_lpg ' /proc/modules || insmod ./leds-qcom-lpg.ko || say "WARN leds-qcom-lpg insmod failed"
    grep -q '^leds_pwm ' /proc/modules || insmod ./leds-pwm.ko || say "WARN leds-pwm insmod failed"
    grep -q '^a6l_fl_ovl ' /proc/modules || insmod ./a6l_fl_ovl.ko || say "WARN overlay insmod failed"
    sleep 2; dmesg | grep -iE 'A6L_FL_OVL|lpg|leds-pwm|epd-backlight|pwm' | tail -20 | sed 's/^/A6L_FL_DMESG /'
    if [ -d $L ]; then say "LOAD_PASS $L max=$(cat $L/max_brightness) brightness=$(cat $L/brightness)"; else say "LOAD_FAIL no $L"; fi
    cat /sys/kernel/debug/pwm 2>/dev/null | sed 's/^/A6L_FL_PWM /'; regs ;;
step)
    [ -d $L ] || { say "FAIL no $L (MODE=load first)"; exit 1; }
    n=${LEVEL:-5}; [ "$n" -gt 40 ] && [ "${FORCE:-0}" != 1 ] && { say "refused: LEVEL $n > 40 without FORCE=1"; exit 1; }
    echo "$n" > $L/brightness; say "brightness $(cat $L/brightness)/$(cat $L/max_brightness) for ${HOLD:-5} s — Pierre: is the e-ink lit?"
    cat /sys/kernel/debug/pwm 2>/dev/null | sed 's/^/A6L_FL_PWM /'; regs
    sleep ${HOLD:-5}; echo 0 > $L/brightness; say "STEP_DONE level=$n back to 0" ;;
off) [ -d $L ] && echo 0 > $L/brightness; say "OFF" ;;
*) say "unknown MODE $MODE"; exit 2 ;;
esac
