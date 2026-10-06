#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Static + host checks for rom/android (overlays-carrier-updater, 29 Sep 2026). Offline, no phone, no `m`.
# usage: bash rom/android/tests/check-android-overlays.sh [<lineage tree>]   (tree: resource-name, aapt2 and APN checks)
set -uo pipefail
H=$(cd "$(dirname "$0")/.." && pwd); A=$(cd "$H/../.." && pwd); L=${1:-}
pass=0; fail=0
exp() { [ -n "${V:-}" ] && echo "ok $1"; if [ "$2" = "$3" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL: $1 (got '$2', want '$3')"; fi; }
c() { tr -d '\r' < "$1"; }
keys() { c "$1" | grep -o 'name="[A-Za-z0-9_]*"' | sed 's/name="//;s/"//' | sort -u; }
FW=$H/overlay/frameworks/base/core/res/res/values/config.xml
R6=$A/rom/r6/overlay/frameworks/base/core/res/res/values/config.xml
WIFI=$H/rro/A6LWifiOverlay/res/values/config.xml; CC=$H/rro/A6LCarrierConfigOverlay/res/xml/vendor.xml
SUI=$H/rro/A6LSystemUIOverlay/res/values/config.xml; SET=$H/rro/A6LSettingsOverlay/res/values/config.xml

# ---- 1. well-formed XML
for f in $(find $H -name '*.xml') $R6; do exp "xml $(basename $(dirname $f))/$(basename $f)" "$(xmllint --noout $f 2>&1 | wc -l)" 0; done

# ---- 2. one owner per framework key (static overlays are merged: a duplicate silently depends on overlay order)
ALL=$(for o in $H/overlay $A/rom/r6/overlay $A/audio/overlay $A/usb/overlay; do [ -d $o ] || continue
      find $o -path '*frameworks/base/core/res/res/values/*.xml' | while read f; do keys $f; done; done | sort)
exp "framework overlay keys unique across rom/android, rom/r6, audio, usb" "$(echo "$ALL" | uniq -d | tr '\n' ' ')" ""

# ---- 3. values
v() { c "$1" | grep -o "name=\"$2\">[^<]*" | sed 's/.*>//'; }
exp "navigation bar on" "$(v $FW config_showNavigationBar)" true
exp "cutout = stock bounding rect 420..660 x 0..75 px" "$(v $FW config_mainBuiltInDisplayCutout)" "M -120,0 L 120,0 L 120,75 L -120,75 Z"
exp "doze (device idle) on" "$(v $FW config_enableAutoPowerModes)" true
exp "no AOD" "$(v $FW config_dozeAlwaysOnDisplayAvailable)" false
exp "brightness min/max/default/dim/doze = stock" "$(for k in Minimum Maximum Default; do v $FW config_screenBrightnessSetting$k; done; v $FW config_screenBrightnessDim; v $FW config_screenBrightnessDoze)" "$(printf '1\n255\n77\n1\n17')"
LUX=$(c $R6 | sed -n '/config_autoBrightnessLevels/,/<\/integer-array>/p' | grep -o '<item>[0-9]*' | tr -d '<item>')
BL=$(c $R6 | sed -n '/config_autoBrightnessLcdBacklightValues/,/<\/integer-array>/p' | grep -o '<item>[0-9]*' | tr -d '<item>')
exp "auto-brightness = stock curve (31 lux / 32 backlight)" "$(echo "$LUX" | wc -l)/$(echo "$BL" | wc -l)/$(echo $LUX | cut -d' ' -f31)/$(echo $BL | cut -d' ' -f1)" "31/32/9534/3"
exp "power_profile battery.capacity 3800" "$(c $A/rom/r6/overlay/frameworks/base/core/res/res/xml/power_profile.xml | grep -o 'battery.capacity">[0-9]*' | sed 's/.*>//')" 3800
exp "Wi-Fi 5 GHz on" "$(v $WIFI config_wifi5ghzSupport)" true
exp "charging thresholds SystemUI == Settings" "$(grep '<integer' $SUI | tr -d ' \r')" "$(grep '<integer' $SET | tr -d ' \r')"
exp "fast threshold 9 W" "$(v $SUI config_chargingFastThreshold)" 9000000
exp "carrier config: VoLTE unavailable, Ut off" "$(c $CC | grep -c -E 'carrier_volte_available_bool" value="false|carrier_supports_ss_over_ut_bool" value="false')" 2
exp "lcd density 400" "$(c $A/rom/rom.mk | grep -o 'ro.sf.lcd_density=[0-9]*')" ro.sf.lcd_density=400

# ---- 4. product wiring (what the r5/r6 pipeline builds: lineage_gsi_a6l.mk -> rom/rom.mk -> rom/android/android.mk;
#         rom/ is copied whole into the tree by tools/stage-rom-v2-prebuilts.sh)
exp "lineage_gsi_a6l.mk inherits rom.mk" "$(c $A/lineage_gsi_a6l.mk | grep -c '^\$(call inherit-product, device/hisense/a6l/rom/rom.mk)')" 1
exp "rom.mk inherits rom/android/android.mk" "$(c $A/rom/rom.mk | grep -c '^\$(call inherit-product, device/hisense/a6l/rom/android/android.mk)')" 1
exp "stage-rom-v2-prebuilts copies rom/ whole" "$(c $A/../../../tools/stage-rom-v2-prebuilts.sh | grep -c 'cp -r $R/device/hisense/a6l/rom/. $T/')" 1
exp "overlay dir wired" "$(c $H/android.mk | grep -c '^DEVICE_PACKAGE_OVERLAYS += $(A6L_ANDROID_DIR)/overlay')" 1
PK=$(c $H/android.mk | sed -n '/^PRODUCT_PACKAGES/,/^$/p' | grep -v PRODUCT_PACKAGES | tr -d '\\ ' | grep -v '^$' | grep -v '^#' | sort)
BP=$(c $H/Android.bp | grep -A1 -E '^runtime_resource_overlay \{' | grep -o 'name: "[^"]*"' | sed 's/name: "//;s/"//' | sort)
exp "PRODUCT_PACKAGES == Android.bp installable modules" "$(echo $PK)" "$(echo $BP)"
for m in $(c $H/Android.bp | grep -o 'manifest: "[^"]*"\|main: "[^"]*"' | sed 's/.*: "//;s/"//') $(c $H/Android.bp | grep -o 'resource_dirs: \["[^"]*"' | sed 's/.*\["//;s/"//'); do
  exp "Android.bp path $m" "$([ -e $H/$m ] && echo y)" y; done
exp "updater placeholder only for non-release" "$(c $H/android.mk | sed -n '/^ifneq ($(A6L_RELEASE),1)/,/^endif/p' | grep -c 'lineage.updater.uri=.*/unpublished/{device}.json')" 1
exp "release URI unchanged" "$(c $A/rom/release/release.mk | grep -c 'lineage.updater.uri=https://raw.githubusercontent.com/PaddleStroke/hisense-a6l-lineageos/main/updater/{device}.json')" 1
exp "pipeline applies rom/android/patches" "$(c $A/../../../tools/rom-v2-pipeline.sh | grep -c 'rom/android/patches/$P4')" 1
exp "gapps knob" "$(c $A/rom/rom.mk | grep -c '^ifneq ($(A6L_NO_GAPPS),1)')" 1

# ---- 5. APN fix-up (host)
T=$(mktemp -d); trap 'rm -rf $T' EXIT
cat > $T/in.xml <<'X'
<?xml version="1.0" encoding="utf-8"?>
<apns version="8">
  <apn mcc="208" mnc="01" carrier="Orange World" apn="" protocol="IPV6" type="ia"/>
  <apn mcc="208" mnc="01" carrier="Orange World" apn="orange" authtype="1" password="orange" protocol="IPV6" type="default,dun,supl,xcap,mms" user="orange"/>
  <apn mcc="208" mnc="02" carrier="Orange World" apn="orange" type="default"/>
  <apn mcc="208" mnc="01" carrier="Orange Fr IMS" apn="ims" protocol="IPV4V6" roaming_protocol="IPV4V6" type="ims"/>
  <apn mcc="208" mnc="01" carrier="Breizh Mobile" apn="orange" mvno_match_data="Breizh Mobile" mvno_type="spn" type="default,supl"/>
  <apn mcc="208" mnc="10" carrier="SFR" apn="sl2sfr" protocol="IPV6" type="default"/>
</apns>
X
python3 $H/apn/a6l_apn_fixup.py $T/in.xml $T/out.xml; exp "fixup rc" $? 0
exp "fixup: 3 Orange World entries IPV4V6 home+roaming" "$(grep -c 'carrier="Orange World".*protocol="IPV4V6".*roaming_protocol="IPV4V6"\|carrier="Orange World" roaming_protocol="IPV4V6".*protocol="IPV4V6"' $T/out.xml)" 3
exp "fixup: other lines untouched" "$(diff $T/in.xml $T/out.xml | grep -c '^>')" 3
exp "fixup: MVNO/IMS/SFR untouched" "$(grep -c -F -f <(grep -v 'Orange World' $T/in.xml) $T/out.xml)" "$(grep -vc 'Orange World' $T/in.xml)"
exp "fixup: idempotent" "$(python3 $H/apn/a6l_apn_fixup.py $T/out.xml $T/out2.xml; cmp -s $T/out.xml $T/out2.xml && echo same)" same
printf '<apns version="8">\n  <apn mcc="208" mnc="10" carrier="SFR" apn="x" type="default"/>\n</apns>\n' > $T/none.xml
python3 $H/apn/a6l_apn_fixup.py $T/none.xml $T/none.out 2>/dev/null; exp "fixup: no Orange entry -> rc 1" $? 1

# ---- 6. against the Lineage tree
if [ -n "$L" ]; then
  CFG=$L/frameworks/base/core/res/res/values/config.xml
  for k in $(keys $FW); do exp "framework key $k exists" "$(grep -c "name=\"$k\"" $CFG)" 1; done
  for k in $(keys $WIFI); do exp "wifi key $k overlayable" "$(grep -c "name=\"$k\"" $L/packages/modules/Wifi/service/ServiceWifiResources/res/values/overlayable.xml)" 1; done
  for k in $(keys $SUI); do exp "SettingsLib key $k exists" "$(grep -c "name=\"$k\"" $L/frameworks/base/packages/SettingsLib/res/values/config.xml)" 1; done
  for k in $(keys $CC); do exp "carrier config key $k" "$(grep -c "\"$k\"" $L/frameworks/base/telephony/java/android/telephony/CarrierConfigManager.java)" 1; done
  exp "CarrierConfig reads R.xml.vendor" "$(grep -c 'R.xml.vendor)' $L/packages/apps/CarrierConfig/src/com/android/carrierconfig/DefaultCarrierConfigService.java)" 1
  exp "Orange France carrier id 32 asset present" "$(ls $L/packages/apps/CarrierConfig/assets/carrier_config_carrierid_32_Orange.xml 2>/dev/null | wc -l)" 1
  exp "vendor/apn modules apns-conf + apns-conf-schema" "$(grep -c -E 'name: "(apns-conf|apns-conf-schema)"' $L/vendor/apn/Android.bp)" 2
  # APN: the tree patch applies to vendor/apn (check only, the tree is not modified), and the list built from the patched
  # sources (make-apns.sh, as the genrule does) == the r5 generated list with the fix-up applied, and passes the XSD
  PA=$H/patches/vendor/apn/0001-FR-Orange-World-IPV4V6.patch
  exp "vendor/apn patch applies to the tree (or is applied)" "$(cd $L/vendor/apn && { git -c safe.directory='*' apply --check $PA 2>/dev/null || git -c safe.directory='*' apply --reverse --check $PA 2>/dev/null; } && echo ok)" ok
  mkdir -p $T/vendor/apn && cp $L/vendor/apn/*.xml $L/vendor/apn/make-apns.sh $L/vendor/apn/apns-conf.xsd $T/vendor/apn/ && (cd $T/vendor/apn && patch -s -p1 < $PA)
  exp "patched FR.xml: 6 Orange World entries IPV4V6 home+roaming" "$(grep -c 'roaming_protocol="IPV4V6"' $T/vendor/apn/FR.xml)" "$(( $(grep -c 'roaming_protocol="IPV4V6"' $L/vendor/apn/FR.xml) + 6 ))"
  (cd $T && XMLLINT=$(command -v xmllint) bash vendor/apn/make-apns.sh $(ls vendor/apn/*.xml | sort) > $T/apns-patched.xml 2>$T/make-apns.err); exp "make-apns on patched sources rc" $? 0
  exp "patched list validates against apns-conf.xsd" "$(xmllint --noout --schema $T/vendor/apn/apns-conf.xsd $T/apns-patched.xml 2>&1 | grep -c 'validates')" 1
  G=$L/out/soong/.intermediates/vendor/apn/apns-conf/gen/apns-conf.xml
  if [ -f $G ]; then
    python3 $H/apn/a6l_apn_fixup.py $G $T/apns.xml; exp "fixup on the r5 generated list rc" $? 0
    exp "fixup on r5 list: 6 entries changed (208-00/01/02 ia + orange)" "$(diff $G $T/apns.xml | grep -c '^>')" 6
    exp "patched-source list == fixed-up r5 list" "$(cmp -s $T/apns.xml $T/apns-patched.xml && echo same || diff $T/apns.xml $T/apns-patched.xml | head -4)" same
    exp "entry count unchanged" "$(grep -c '<apn ' $T/apns-patched.xml)" "$(grep -c '<apn ' $G)"
  fi
  AAPT=$L/out/host/linux-x86/bin/aapt2; JAR=$(ls $L/prebuilts/sdk/current/public/android.jar 2>/dev/null)
  if [ -x $AAPT ] && [ -n "$JAR" ]; then
    for r in $H/rro/*/; do n=$(basename $r); mkdir -p $T/$n
      $AAPT compile --dir $r/res -o $T/$n/res.zip 2>$T/$n/err && $AAPT link -I $JAR --manifest $r/AndroidManifest.xml -o $T/$n/$n.apk $T/$n/res.zip 2>>$T/$n/err
      exp "aapt2 compile+link $n" "$? $(cat $T/$n/err | head -3)" "0 "; done
    mkdir -p $T/fw; $AAPT compile --dir $H/overlay/frameworks/base/core/res/res -o $T/fw/res.zip; exp "aapt2 compile framework overlay" $? 0
  fi
  BPF=$L/prebuilts/build-tools/linux-x86/bin/bpfmt; [ -x $BPF ] && exp "bpfmt clean" "$($BPF -l $H/Android.bp | wc -l)" 0
fi
echo "check-android-overlays: $pass pass, $fail fail"; [ $fail = 0 ] && echo ANDROID_OVERLAYS_PASS
exit $((fail > 0))
