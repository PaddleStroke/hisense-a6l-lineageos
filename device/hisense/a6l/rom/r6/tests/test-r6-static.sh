#!/usr/bin/env bash
# Offline checks of the r6 preparation (completeness audit, 29 Sep 2026). No build.
# usage: bash rom/r6/tests/test-r6-static.sh [<device/hisense/a6l dir>]  -> A6L_R6_STATIC_TEST PASS|FAIL n
A=${1:-$(cd "$(dirname "$0")/../../.." && pwd)}; R6=$A/rom/r6; fails=0
exp() { [ "$2" = "$3" ] && echo "ok   $1 ($3)" || { echo "FAIL $1: got '$2' want '$3'"; fails=$((fails+1)); }; }
c() { tr -d '\r' < "$1"; }
# ---- r6.mk wiring
MK=$(c $R6/r6.mk)
for f in $(echo "$MK" | grep -o '\$(R6_DIR)/[^: ]*' | sed "s#\$(R6_DIR)#$R6#" | sort -u); do exp "r6.mk path exists ${f#$A/}" "$([ -e $f ] && echo y)" y; done
BP=$(c $R6/thermal/Android.bp)
exp "thermal module in Android.bp" "$(echo "$BP" | grep -c 'name: "android.hardware.thermal-service.a6l"')" 1
exp "thermal Android.bp is a soong namespace (inert until r6.mk)" "$(echo "$BP" | grep -c '^soong_namespace {')" 1
exp "r6.mk adds the thermal namespace" "$(echo "$MK" | grep -c 'PRODUCT_SOONG_NAMESPACES += $(R6_DIR)/thermal')" 1
for s in $(echo "$BP" | sed -n '/srcs: \[/,/\]/p' | grep -o '"[^"]*"' | tr -d '"'); do exp "thermal src $s" "$([ -f $R6/thermal/$s ] && echo y)" y; done
for k in init_rc vintf_fragments; do f=$(echo "$BP" | grep "$k:" | grep -o '"[^"]*"' | tr -d '"'); exp "thermal $k file" "$([ -f $R6/thermal/$f ] && echo y)" y; done
exp "rc starts the installed binary" "$(c $R6/thermal/android.hardware.thermal-service.a6l.rc | grep -c '^service vendor.thermal-a6l /vendor/bin/hw/android.hardware.thermal-service.a6l$')" 1
exp "file_contexts labels the binary hal_thermal_default_exec" "$(c $R6/sepolicy/vendor/file_contexts | grep -c '^/vendor/bin/hw/android\\\.hardware\\\.thermal-service\\\.a6l *u:object_r:hal_thermal_default_exec:s0$')" 1
exp "main.cpp reads the installed config" "$(grep -c '"/vendor/etc/thermal-a6l.conf"' $R6/thermal/main.cpp)" 1
exp "prebuilt_etc installs thermal-a6l.conf to /vendor/etc" "$(echo "$BP" | grep -A3 'prebuilt_etc' | grep -c 'name: "thermal-a6l.conf"')" 1
# ---- no vendor property in r6.mk that rom.mk already defines (PRODUCT_*_PROPERTIES: first definition wins)
ROMP=$(c $A/rom/rom.mk | grep -oE '^ +[a-z0-9_.]+=' | tr -d ' =' | sort -u)
for p in $(echo "$MK" | grep -oE '^ +[a-z0-9_.]+=' | tr -d ' =' | sort -u); do exp "r6 property $p not already set by rom.mk" "$(echo "$ROMP" | grep -cxF $p)" 0; done
# ---- thermal config vs the r5 kernel's thermal zones (sdm630.dtsi) + host test
KZ="aoss cpuss0 cpuss1 cpu0 cpu1 cpu2 cpu3 pwr-cluster gpu pm660 pm660l"
KD=${A6L_KSRC:-/home/a6l/kernel/a6l-rom-r5-src}/arch/arm64/boot/dts/qcom
[ -f $KD/sdm630.dtsi ] && KZ=$(for f in sdm630.dtsi pm660.dtsi pm660l.dtsi; do awk '/thermal-zones \{/,0' $KD/$f; done | grep -oE '^\s+[a-z0-9-]+-thermal \{' | sed 's/-thermal {//; s/^\s*//' | tr '\n' ' ')
for z in $(c $R6/thermal/thermal-a6l.conf | grep -v '^#' | grep -o 'zone:[^ ]*' | cut -d: -f2 | tr ',' '\n' | sed 's/-thermal$//'); do
  exp "thermal zone '$z' exists in the r5 DT" "$(echo " $KZ " | grep -c " $z ")" 1; done
exp "battery source = fuel gauge power_supply" "$(c $R6/thermal/thermal-a6l.conf | grep -cE '^battery +BATTERY +.*file:/sys/class/power_supply/qcom-battery/temp +0\.1 ')" 1
exp "genfs labels qcom-battery power_supply batteryinfo (rom sepolicy)" "$(c $A/rom/sepolicy/vendor/genfs_contexts | grep -c 'pmic@0:battery@4000 u:object_r:sysfs_batteryinfo:s0')" 1
exp "thermal host test" "$(bash $R6/thermal/tests/run-tests.sh 2>&1 | tail -1)" "A6L_THERMAL_TEST PASS"
# ---- overlay
OV=$R6/overlay/frameworks/base/core/res/res
exp "overlay xml well-formed" "$(xmllint --noout $OV/values/config.xml $OV/xml/power_profile.xml 2>&1 | wc -l)" 0
L=$(c $OV/values/config.xml | sed -n '/config_autoBrightnessLevels/,/<\/integer-array>/p' | grep -o '<item>[0-9]*' | tr -d '<item>')
V=$(c $OV/values/config.xml | sed -n '/config_autoBrightnessLcdBacklightValues/,/<\/integer-array>/p' | grep -o '<item>[0-9]*' | tr -d '<item>')
exp "autobrightness: backlight values = lux levels + 1" "$(( $(echo "$V" | wc -l) - $(echo "$L" | wc -l) ))" 1
exp "autobrightness lux strictly increasing" "$(echo "$L" | awk 'NR>1 && $1<=p {b=1} {p=$1} END {print b+0}')" 0
exp "autobrightness backlight non-decreasing within 1..255" "$(echo "$V" | awk 'NR>1 && $1<p {b=1} $1<1||$1>255 {b=1} {p=$1} END {print b+0}')" 0
exp "power_profile battery.capacity = fuel gauge 3800 mAh" "$(c $OV/xml/power_profile.xml | grep -o '"battery.capacity">[0-9]*' | cut -d'>' -f2)" 3800
exp "power doc has the 3800 mAh FG profile" "$(grep -c '3800 mAh profile' $A/../../../docs/power-20260926.md 2>/dev/null | awk '{print ($1>0)?"y":"n"}')" y
for o in $A/audio/overlay $A/usb/overlay; do [ -d $o ] || continue
  for n in $(c $OV/values/config.xml | grep -oE 'name="[a-zA-Z_]+"' | cut -d'"' -f2); do
    exp "overlay $n not also in ${o#$A/}" "$(grep -rl "name=\"$n\"" $o 2>/dev/null | wc -l)" 0; done; done
# ---- suspend tool
exp "a6l-suspend-check.sh parses (dash)" "$(dash -n $R6/tools/a6l-suspend-check.sh 2>&1 | wc -l)" 0
# ---- apply script dry run + idempotence on a scratch copy
S=$(mktemp -d); cp -r $A/rom $S/rom; bash $R6/apply-r6.sh --apply $S > /dev/null
exp "apply: rom.mk inherits r6.mk once" "$(grep -c 'inherit-product, device/hisense/a6l/rom/r6/r6.mk' $S/rom/rom.mk)" 1
exp "apply: sepolicy dir added once" "$(grep -c '^A6L_SEPOLICY_DIRS += rom/r6/sepolicy/vendor$' $S/rom/BoardConfig-rom.mk)" 1
exp "apply: second run changes nothing" "$(bash $R6/apply-r6.sh --apply $S | grep -c '^done:')" 0
rm -rf $S
echo "A6L_R6_STATIC_TEST $([ $fails = 0 ] && echo PASS || echo FAIL) $fails"; exit $fails
