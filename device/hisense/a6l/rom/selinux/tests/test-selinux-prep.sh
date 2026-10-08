#!/usr/bin/env bash
# selinux-release (29 Sep 2026): offline tests of the SELinux enforcing-prep flag, the avc planner, the charger UI and the
# Updater placeholders. No build, no phone. docs/selinux-release-20260929.md
# usage: bash device/hisense/a6l/rom/selinux/tests/test-selinux-prep.sh [<repo>] [<lineage tree>]   -> A6L_SELINUX_PREP_TEST PASS|FAIL n
R=${1:-$(cd "$(dirname "$0")/../../../../../.." && pwd)}; L=${2:-/home/a6l/android/a6l-lineage24}
DV=$R/device/hisense/a6l; P=$R/tools/release/a6l_selinux_prep.py; W=$(mktemp -d); fails=0
exp() { [ "$2" = "$3" ] && echo "ok   $1 ($3)" || { echo "FAIL $1: got '$2' want '$3'"; fails=$((fails+1)); }; }
# ---- T: transform on a copy of the installed rc files
for f in $(python3 -c "import importlib.util,sys;s=importlib.util.spec_from_file_location('p','$P');m=importlib.util.module_from_spec(s);s.loader.exec_module(m);print(' '.join(m.INSTALLED_RC+[m.UEVENTD]))"); do
  [ -f $DV/$f ] && { mkdir -p $W/d/$(dirname $f); tr -d '\r' < $DV/$f > $W/d/$f; }; done
mkdir -p $W/d/rom/selinux
nsvc() { grep -h '^service ' $(find $W/d -name '*.rc') | wc -l; }
before=$(nsvc); modp=$(grep -rh 'seclabel u:r:vendor_modprobe:s0' $W/d | wc -l)
exp "T0 repo still has vendor_modprobe seclabels (default build unchanged)" "$([ $modp -ge 10 ] && echo yes)" yes
python3 $P apply $W/d > $W/apply.log 2>&1; exp "T1 apply rc" $? 0
exp "T1 no vendor_modprobe left" "$(grep -rh 'vendor_modprobe' $W/d --include=*.rc | grep -v '^ *#' | wc -l)" 0
exp "T1 only a6l_logcat removed" "$(nsvc)" "$((before-1))"
RC=$W/d/rom/init/init.qcom.rc
exp "T2 no a6l_logcat in init.qcom.rc" "$(grep -v '^ *#' $RC | grep -c a6l_logcat)" 0
exp "T2 modules header comment kept" "$(grep -c '^# ---- modules' $RC)" 1
exp "T2 no su seclabel left in installed rc" "$(grep -rh 'seclabel u:r:su:s0' $W/d --include=*.rc | wc -l)" 0
exp "T2 debug rc = trigger + service, su" "$(grep -c -E '^(on property:logd.ready=true && property:ro.boot.a6l_logcat=1|service a6l_logcat /vendor/bin/a6l-logcat.sh|    seclabel u:r:su:s0)$' $DV/rom/selinux/init.a6l.logcat-debug.rc)" 3
UE=$W/d/rom/vendor-etc/ueventd.rc
exp "T3 dri card 0660 system graphics" "$(awk '$1=="/dev/dri/card*"{print $2,$3,$4}' $UE)" "0660 system graphics"
exp "T3 dri renderD 0666" "$(awk '$1=="/dev/dri/renderD*"{print $2}' $UE)" 0666
exp "T3 no world-writable dri wildcard" "$(awk '$1=="/dev/dri/*"' $UE | wc -l)" 0
exp "T4 marker" "$([ -f $W/d/rom/selinux/.prep-applied ] && echo yes)" yes
python3 $P apply $W/d > $W/apply2.log 2>&1; exp "T4 idempotent" "$(tail -1 $W/apply2.log)" "A6L_SELINUX_PREP_APPLIED 0 file(s)"
# drift: a trigger without its service must be refused
cp -r $W/d $W/drift; python3 - $W/drift/rom/init/init.qcom.rc $DV/rom/init/init.qcom.rc <<'PY'
import sys; s=open(sys.argv[2]).read().replace('\r\n','\n'); s=s.replace('service a6l_logcat /vendor/bin/a6l-logcat.sh','service a6l_logkat /vendor/bin/a6l-logcat.sh'); open(sys.argv[1],'w').write(s)
PY
python3 $P apply $W/drift > $W/drift.log 2>&1; exp "T5 drift refused" "$(grep -c DRIFT $W/drift.log)" 1
# regenerated reference patch == current transform, applies to the repo
( cd $R && python3 $P diff device/hisense/a6l > $W/cur.patch )
exp "T6 rc-enforcing.patch is current" "$(tail -n +6 $DV/rom/sepolicy/rc-enforcing.patch | tr -d '\r' | cmp -s - $W/cur.patch && echo same)" same
exp "T6 rc-enforcing.patch applies (dry run)" "$(cd $R && patch -p1 --dry-run -s < $DV/rom/sepolicy/rc-enforcing.patch > /dev/null 2>&1 && echo ok)" ok
# ---- flag wiring
BR=$DV/rom/BoardConfig-rom.mk
exp "W1 BoardConfig-rom: prep implies rom/sepolicy/vendor" "$(grep -c 'filter 1,$(A6L_SEPOLICY_ROM) $(A6L_RELEASE) $(A6L_SELINUX_PREP)' $BR)" 1
exp "W1 BoardConfig-rom includes selinux + charger" "$(grep -c -E '^include device/hisense/a6l/rom/(selinux/BoardConfig-selinux|charger/BoardConfig-charger)\.mk$' $BR)" 2
exp "W2 rom.mk inherits charger + selinux" "$(grep -c -E 'inherit-product, device/hisense/a6l/rom/(charger/charger|selinux/selinux)\.mk' $DV/rom/rom.mk)" 2
exp "W3 BoardConfig-selinux errors without the tree marker" "$(grep -c 'rom/selinux/.prep-applied' $DV/rom/selinux/BoardConfig-selinux.mk)" 1
exp "W3 selinux.mk: debug rc only for non-user" "$(grep -c 'ifneq ($(TARGET_BUILD_VARIANT),user)' $DV/rom/selinux/selinux.mk)" 1
PL=$R/tools/rom-v2-pipeline.sh
exp "W4 pipeline applies the transform under the flag" "$(grep -c 'a6l_selinux_prep.py apply $T' $PL)" 1
exp "W4 pipeline syncs watchdog (r6.mk inherits it)" "$(grep -c 'usb camera watchdog; do rm -rf' $PL)" 1
exp "W5 default cmdline stays permissive (no enforcing switch)" "$(grep -c "androidboot.selinux=permissive" $R/tools/Prepare-RomV2Boot.py | awk '{print ($1>0)?"yes":"no"}')" yes
# ---- charger UI
exp "C1 charger packages" "$(grep -c -E '^    (android.hardware.health-service.example|charger_res_images|lineage_charger_animation_vendor)' $DV/rom/charger/charger.mk)" 3
d1=$(sed -n 's/^TARGET_SCREEN_DENSITY ?= *//p' $DV/rom/charger/BoardConfig-charger.mk); d2=$(grep -o 'ro.sf.lcd_density=[0-9]*' $DV/rom/rom.mk | cut -d= -f2)
exp "C2 charger density == ro.sf.lcd_density (duplicate prop must be equal)" "$d1" "$d2"
exp "C3 charger_vendor may open the KMS node (gpu_device)" "$(grep -c '^allow charger_vendor gpu_device:chr_file rw_file_perms;' $DV/rom/selinux/sepolicy/vendor/charger_vendor.te)" 1
exp "C4 on charger path intact" "$(grep -c -E '^on charger$|^    start vendor.a6l_modules_offcharge$' $DV/rom/init/init.qcom.rc)" 2
# ---- updater placeholders
exp "U1 release URI https + {device}" "$(grep -c 'lineage.updater.uri=https://raw.githubusercontent.com/.*/updater/{device}.json' $DV/rom/release/release.mk)" 1
exp "U2 non-release placeholder URI (unpublished)" "$(grep -c 'lineage.updater.uri=https://.*/updater/unpublished/{device}.json' $DV/rom/android/android.mk)" 1
exp "U3 feed placeholder is an empty v2 list" "$(python3 -c "import json;print(json.load(open('$DV/rom/release/updater/a6l.json'))==[])")" True
# ---- avc planner on the fixture
ls -lR $DV/rom/selinux/sepolicy $DV/rom/sepolicy $DV/eink/sepolicy > $W/policy-before.txt 2>/dev/null
python3 $R/tools/release/a6l-avc-plan.py --repo $R --ps $DV/rom/selinux/tests/fixtures/ps-AZ.txt --out $W/plan $DV/rom/selinux/tests/fixtures/avc-sample.txt > $W/plan.txt
exp "A1 plan summary" "$(tail -1 $W/plan.txt)" "A6L_AVC_PLAN groups=10 allow=3 label=2 neverallow=3 platform=1 debug=1 blocking=1"
exp "A2 system exec flagged NEVERALLOW" "$(sed -n '/^## NEVERALLOW/,/^## /p' $W/plan.txt | grep -c 'a6l_radio_ctl system_file')" 1
exp "A3 epdd rule proposed in its owning dir" "$(cat $W/plan/proposed/eink/sepolicy/vendor/a6l_epdd.te | grep -c 'allow a6l_epdd a6l_spidev_device:chr_file { ioctl read write };')" 1
exp "A4 vendor_modprobe process reported" "$(grep -c 'a6l-radio.sh runs as vendor_modprobe' $W/plan.txt)" 1
# selinux-20261007: rom/selinux/sepolicy/vendor now holds reviewed rules (pass 1/2), so A5 compares the policy dirs
# before/after the planner run instead of expecting an empty directory
exp "A5 planner never writes into the repo" "$(cmp -s $W/policy-before.txt <(ls -lR $DV/rom/selinux/sepolicy $DV/rom/sepolicy $DV/eink/sepolicy 2>/dev/null) && echo unchanged)" unchanged
# ---- init service domains against the real policy (needs the Lineage tree for system/sepolicy)
if [ -d $L/system/sepolicy ]; then
  for m in "" --prep; do
    python3 $P services $DV --aosp $L/system/sepolicy $m --lineage $L/device/lineage/sepolicy/common/vendor > $W/svc$m.log 2>&1
    exp "S services $m" "$(tail -1 $W/svc$m.log | cut -d' ' -f1-3)" "A6L_SELINUX_SERVICES PASS policy=$([ -n "$m" ] && echo prep || echo default)"
  done
  exp "S prep: no vendor_modprobe / su service in installed rc" "$(grep -E '^(WARN|FAIL)' $W/svc--prep.log | wc -l)" 0
else echo "skip S (no $L/system/sepolicy)"; fi
rm -rf $W
[ $fails = 0 ] && echo "A6L_SELINUX_PREP_TEST PASS" || { echo "A6L_SELINUX_PREP_TEST FAIL $fails"; exit 1; }
