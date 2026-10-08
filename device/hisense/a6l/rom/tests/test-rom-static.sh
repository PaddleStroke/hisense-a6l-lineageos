#!/usr/bin/env bash
# r5 review fixes F11/F13/H49 (28 Sep 2026): offline structural checks of the ROM init/ueventd/loader sources (no build).
# F11: a6l-modules.sh display publishes vendor.a6l.gpu in BOTH branches (executed with shims) and never sets
#      persist.graphics.egl itself; init.qcom.rc copies it in post-fs-data with an 'angle' default.
# F13: no early flash chown in init.qcom.rc; ueventd rules for white:flash (system:camera, no world-write bits).
# H49: `on charger` starts the offcharge group, which loads only the fuel gauge + charger group; guard started after it.
# r6b boot fix (30 Sep 2026): the display group is STARTED (non-blocking) in early-init; bounded waits (post-fs-data, charger,
#      adsp group); vendor.a6l.gpu always published even when an insmod hangs (simulated); insmod logged before it runs;
#      userdebug-only persistent boot log (rom/debug/bootlog.mk) + A6L_STAGE markers; log_buf_len; ramoops DT overlay.
# usage: bash rom/tests/test-rom-static.sh [<repo>/device/hisense/a6l/rom]  -> A6L_ROM_STATIC_TEST PASS|FAIL n
D=${1:-$(cd "$(dirname "$0")/.." && pwd)}; W=$(mktemp -d); fails=0
exp() { [ "$2" = "$3" ] && echo "ok   $1 ($3)" || { echo "FAIL $1: got '$2' want '$3'"; fails=$((fails+1)); }; }
RC=$W/init.qcom.rc; tr -d '\r' < $D/init/init.qcom.rc > $RC
UE=$W/ueventd.rc; tr -d '\r' < $D/vendor-etc/ueventd.rc > $UE
MS=$W/a6l-modules.sh; tr -d '\r' < $D/bin/a6l-modules.sh > $MS
# block <trigger-line-regex>: lines of that init section
block() { awk -v re="$1" '$0 ~ re {f=1; next} f && /^(on|service|import) / {f=0} f' $RC; }
# ---- F11
exp "F11 script never sets persist.graphics.egl" "$(grep -v '^ *#' $MS | grep -c 'setprop persist.graphics.egl')" 0
exp "F11 post-fs-data copies vendor.a6l.gpu with angle default" "$(block '^on post-fs-data$' | grep -c 'setprop persist.graphics.egl \${vendor.a6l.gpu:-angle}')" 1
# run the display group with shims: renderD128 present / absent (paths under /sys are faked by rewriting them)
mkdir -p $W/bin $W/sys/class/drm
printf '#!/bin/sh\necho "$1=$2" >> %s/props\n' $W > $W/bin/setprop
printf '#!/bin/sh\nexit 0\n' > $W/bin/sleep; printf '#!/bin/sh\nexit 0\n' > $W/bin/insmod
# getprop shim: last value set through the setprop shim ("" when unset)
printf '#!/bin/sh\ngrep "^$1=" %s/props 2>/dev/null | tail -1 | cut -d= -f2-\n' $W > $W/bin/getprop
chmod +x $W/bin/*
sed "s#/sys/class/drm#$W/sys/class/drm#g; s#/proc/modules#$W/modules#g; s#^A=/dev/a6l#A=$W/a6l#; s#/vendor/lib#$W/vendor/lib#g" $MS > $W/m.sh; : > $W/modules
# r6d: the props file starts with ro.product.cpu.abilist32 (the product runs zygote64_32) unless NO32=1
disp() { if [ "${NO32:-0}" = 1 ]; then : > $W/props; else echo "ro.product.cpu.abilist32=armeabi-v7a,armeabi" > $W/props; fi
  env PATH="$W/bin:$PATH" dash $W/m.sh display > $W/disp.log 2>&1; grep '^vendor.a6l.gpu=' $W/props | tail -1; }
mkdir -p $W/vendor/lib64/egl $W/vendor/lib/egl
for a in lib lib64; do touch $W/vendor/$a/egl/libEGL_mesa.so $W/vendor/$a/egl/libGLESv2_mesa.so $W/vendor/$a/libgallium_dri.so; done
touch $W/sys/class/drm/renderD128; exp "F11 render node -> mesa" "$(disp)" vendor.a6l.gpu=mesa
# r6d: r6c's image (Mesa only in lib64) with a 32-bit zygote -> angle, never mesa (loader abort in app_process32)
rm $W/vendor/lib/egl/libEGL_mesa.so; exp "R6D-1 no 32-bit libEGL_mesa -> angle" "$(disp)" vendor.a6l.gpu=angle
exp "R6D-1 logs the missing library" "$(grep -c 'Mesa incomplete (missing: .*/vendor/lib/egl/libEGL_mesa.so)' $W/disp.log)" 1
exp "R6D-2 64-bit-only zygote: lib64 Mesa suffices" "$(NO32=1 disp)" vendor.a6l.gpu=mesa
touch $W/vendor/lib/egl/libEGL_mesa.so; rm $W/vendor/lib64/libgallium_dri.so
exp "R6D-3 no 64-bit gallium -> angle" "$(disp)" vendor.a6l.gpu=angle; touch $W/vendor/lib64/libgallium_dri.so
rm $W/sys/class/drm/renderD128;    exp "F11 no render node -> angle (explicit)" "$(disp)" vendor.a6l.gpu=angle
# ---- F13
exp "F13 no early flash chown in init.qcom.rc" "$(grep -v '^ *#' $RC | grep -c 'leds/.*flash')" 0
for a in brightness flash_brightness flash_timeout flash_strobe; do
  exp "F13 ueventd white:flash $a" "$(awk -v a=$a '$1=="/sys/class/leds/white:flash" && $2==a {print $3, $4, $5}' $UE)" "0664 system camera"
done
exp "F13 ueventd white:flash max_brightness read-only" "$(awk '$1=="/sys/class/leds/white:flash" && $2=="max_brightness" {print $3}' $UE)" 0444
exp "F13 no world-writable sysfs rule" "$(awk '$1 ~ /^\/sys\// && $3 ~ /[2367]$/' $UE | wc -l)" 0
exp "F13 every /sys rule has 5+ fields" "$(awk '$1 ~ /^\/sys\// && NF < 5' $UE | wc -l)" 0
# ---- F49 (29 Sep 2026): VibratorOL LED backend on a6l_gpio_vib: DAC for the system-uid service, restart after the misc group
for a in activate duration state; do
  exp "F49 ueventd leds/vibrator $a" "$(awk -v a=$a '$1=="/sys/class/leds/vibrator" && $2==a {print $3, $4, $5}' $UE)" "0664 system system"
done
exp "F49 vibrator HAL restarted after the misc group" "$(block '^on property:init.svc.vendor.a6l_modules_misc=stopped$' | grep -c '^ *restart vendor.qti.vibrator$')" 1
exp "F49 misc group loads a6l_gpio_vib" "$(grep -v '^#' $D/modules/misc.txt | grep -c '^a6l_gpio_vib.ko$')" 1
exp "F49 hal_vibrator may write the vibrator LED" "$(grep -c '^allow hal_vibrator_default sysfs_a6l_vibrator:file rw_file_perms;' $D/sepolicy/vendor/hal_extras.te)" 1
exp "F49 vibrator LED genfs label" "$(grep -c '^genfscon sysfs /devices/platform/a6l-vibrator .*sysfs_a6l_vibrator' $D/sepolicy/vendor/genfs_contexts)" 1
# r5 bug hunt power (29 Sep 2026): SPMI child device names "800f000.spmi:pmic@N:<node>" (phone readlink/dmesg evidence);
# power_supply = sysfs_batteryinfo (health HAL, off-mode charger_vendor, recovery read only that type)
GF=$W/genfs; tr -d '\r' < $D/sepolicy/vendor/genfs_contexts > $GF
S0=/devices/platform/soc@0/800f000.spmi/spmi-0
exp "power genfs charger@1000 batteryinfo" "$(grep -c "^genfscon sysfs $S0/0-00/800f000.spmi:pmic@0:charger@1000 u:object_r:sysfs_batteryinfo:s0$" $GF)" 1
exp "power genfs FG battery@4000 batteryinfo" "$(grep -c "^genfscon sysfs $S0/0-00/800f000.spmi:pmic@0:battery@4000 u:object_r:sysfs_batteryinfo:s0$" $GF)" 1
exp "flash genfs led-controller@d300 SPMI name" "$(grep -c "^genfscon sysfs $S0/0-03/800f000.spmi:pmic@3:led-controller@d300 " $GF)" 1
exp "no SPMI genfs path with the bus-id prefix (0-0N:pmic@)" "$(grep -v '^ *#' $GF | grep -c '/0-0[0-9]:pmic@')" 0
# ---- r5 bug hunt boot-init (29 Sep 2026): Android-required kernel features built as modules in the ROM's V67 Image must be
# loaded by the base group (a6l-modules.sh display, early-init): FUSE (vold /dev/fuse: /storage/emulated, microSD), UHID (BT HID)
KC=$D/../../../../firmware/extracted/phone-kernel-v67-candidate-20260919/config
if [ -f "$KC" ]; then
  for c in FUSE_FS:fuse.ko UHID:uhid.ko; do
    v=$(grep "^CONFIG_${c%%:*}=" $KC | cut -d= -f2)
    case "$v" in y) exp "base CONFIG_${c%%:*} built in" y y;;
      m) exp "base CONFIG_${c%%:*}=m -> ${c#*:} in base.txt" "$(grep -v '^#' $D/modules/base.txt | grep -c "^${c#*:}\$")" 1;;
      *) exp "base CONFIG_${c%%:*} enabled in the V67 config" "$v" "y|m";; esac
  done
else echo "skip base config check (no $KC)"; fi
# the display group loads base first (insmod order captured with a shim)
printf '#!/bin/sh\necho "$1" >> %s/insmods\nexit 0\n' $W > $W/bin/insmod; chmod +x $W/bin/insmod; : > $W/insmods
sed -i "s#^L=/vendor/etc/a6l/modules#L=$D/modules#" $W/m.sh; touch $W/sys/class/drm/renderD128; disp > /dev/null
exp "base: fuse loaded by the display group" "$(grep -c '/fuse.ko$' $W/insmods)" 1
exp "base: uhid loaded by the display group" "$(grep -c '/uhid.ko$' $W/insmods)" 1
exp "base: loaded before msm" "$(grep -n -E '/(fuse|uhid|msm).ko$' $W/insmods | sed 's#.*/##' | tr '\n' ' ')" "fuse.ko uhid.ko msm.ko "
# ---- r6b boot fix (30 Sep 2026, docs/rom-r6b-bootfix-20260930.md)
eib=$(block '^on early-init$' | grep -v '^ *#')
exp "B1 early-init STARTS the display group (non-blocking)" "$(grep -c '^ *start a6l_modules_display$' <<< "$eib")" 1
exp "B1 no exec_start of a module group anywhere" "$(grep -v '^ *#' $RC | grep -c 'exec_start a6l_modules_')" 0
exp "B1 only bounded exec_start in init.qcom.rc" "$(grep -v '^ *#' $RC | grep 'exec_start' | awk '{print $2}' | sort -u | tr '\n' ' ')" "a6l_display_wait "
pfd=$(block '^on post-fs-data$' | grep -v '^ *#' | grep -n -E 'exec_start a6l_display_wait|setprop persist.graphics.egl' | cut -d: -f2- | sed 's/^ *//' | tr '\n' '|')
exp "B2 post-fs-data: bounded wait, then the F11 copy" "$pfd" 'exec_start a6l_display_wait|setprop persist.graphics.egl ${vendor.a6l.gpu:-angle}|'
dw=$(grep '^service a6l_display_wait ' $RC); dws=$(awk '{print $NF}' <<< "$dw")
exp "B3 a6l_display_wait = displaywait done <= 60 s" "$(awk '{print $3, $4, $5}' <<< "$dw")/$([ "$dws" -le 60 ] && echo bounded)" "/vendor/bin/a6l-modules.sh displaywait done/bounded"
exp "B3 charger mode waits for the display (bounded)" "$(block '^on early-init && property:ro.bootmode=charger$' | grep -c '^ *exec_start a6l_display_wait$')" 1
exp "B4 adsp group waits for the display lists (bounded 30 s)" "$(awk '/^adsp\)/{f=1;next} f && /^ *;;/{f=0} f' $MS | grep -c 'while \[ \$i -lt 30 \] && \[ "$(getprop vendor.a6l.display)" = loading \]')" 1
exp "B5 insmod logged before it runs" "$(grep -c 'log "$1: insmod $ko \.\.\."' $MS)" 1
disp > /dev/null; exp "B5 display state done after a normal run" "$(grep '^vendor.a6l.display=' $W/props | tail -1)" vendor.a6l.display=done
exp "B5 'insmod msm.ko ...' precedes 'insmod msm.ko ok'" "$(grep -E 'display: insmod msm.ko (\.\.\.|ok)' $W/disp.log | sed 's/.*insmod //' | tr '\n' '|')" "msm.ko ...|msm.ko ok|"
# hang simulation: msm.ko never returns (4 s here); vendor.a6l.gpu must be published while the loader is still blocked
printf '#!/bin/sh\ncase "$1" in */msm.ko) exec /bin/sleep 4;; esac\nexit 0\n' > $W/bin/insmod; chmod +x $W/bin/insmod
rm -f $W/sys/class/drm/renderD128; : > $W/props; rm -rf $W/a6l
( env PATH="$W/bin:$PATH" dash $W/m.sh display > $W/hang.log 2>&1 & ); k=0
while [ $k -lt 30 ] && ! { grep -q '^vendor.a6l.gpu=' $W/props && grep -q 'insmod msm.ko \.\.\.' $W/hang.log; }; do /bin/sleep 0.1; k=$((k+1)); done
exp "B6 hang: vendor.a6l.gpu published while msm.ko blocks" "$(grep '^vendor.a6l.gpu=' $W/props | tail -1)" vendor.a6l.gpu=angle
exp "B6 hang: display state stays loading" "$(grep '^vendor.a6l.display=' $W/props | tail -1)" vendor.a6l.display=loading
exp "B6 hang: last log line names the blocked module" "$(grep ': insmod ' $W/hang.log | tail -1 | sed 's/^A6L_ROM //')" "display: insmod msm.ko ..."
exp "B6 hang: decider says the lists are still loading" "$(grep -c 'module lists still loading' $W/hang.log)" 1
/bin/sleep 4.5; exp "B6 hang: loader resumes after the insmod returns" "$(grep '^vendor.a6l.display=' $W/props | tail -1)" vendor.a6l.display=done
printf '#!/bin/sh\nexit 0\n' > $W/bin/insmod; chmod +x $W/bin/insmod
# displaywait: returns at once when done, bounded (shimmed sleep) otherwise
: > $W/props; env PATH="$W/bin:$PATH" dash $W/m.sh displaywait done 2 > $W/dw.log 2>&1
exp "B7 displaywait bounded when the group never finishes" "$(grep -o 'after [0-9]*x0.2s' $W/dw.log)" "after 10x0.2s"
echo "vendor.a6l.display=done" > $W/props; env PATH="$W/bin:$PATH" dash $W/m.sh displaywait done 2 > $W/dw.log 2>&1
exp "B7 displaywait returns at once when done" "$(grep -o 'after [0-9]*x0.2s' $W/dw.log)" "after 0x0.2s"
# stage markers + USB independent of the display group
exp "B8 A6L_STAGE markers (early-init init fs x2 post-fs-data boot)" "$(grep -c '^ *write /dev/kmsg "A6L_STAGE' $RC)" 6
UR=$W/usb.rc; tr -d '\r' < $D/init/init.a6l.usb.rc > $UR
exp "B8 USB gadget/adbd rc never waits on the display group" "$(grep -c -E 'a6l_modules|vendor.a6l.display|vendor.a6l.gpu' $UR)" 0
# persistent boot log: userdebug/eng only
BM=$(tr -d '\r' < $D/debug/bootlog.mk)
exp "B9 bootlog.mk guarded by the variant" "$(grep -c '^ifneq ($(TARGET_BUILD_VARIANT),user)$' <<< "$BM")" 1
exp "B9 bootlog.mk installs rc + script only inside the guard" "$(awk '/^ifneq/{f=1} /^endif/{f=0} f' <<< "$BM" | grep -c 'a6l-bootlog.sh\|init.a6l.bootlog-debug.rc')" 2
exp "B9 rom.mk inherits bootlog.mk" "$(grep -c '^$(call inherit-product, device/hisense/a6l/rom/debug/bootlog.mk)$' $D/rom.mk)" 1
BR=$W/bootlog.rc; tr -d '\r' < $D/debug/init.a6l.bootlog-debug.rc > $BR
exp "B9 bootlog starts on fs, right after mount_all (r6c)" "$(awk '/^on fs$/{f=1;next} /^(on|service) /{f=0} f' $BR | grep -c -E '^ *(mkdir /metadata/a6l 0770 root root|start a6l_bootlog)$')" 2
exp "B9 no post-fs start left" "$(grep -c '^on post-fs$' $BR)" 0
exp "B12 on fs: r6b's .prev removed synchronously before the service starts" "$(awk '/^on fs$/{f=1;next} /^(on|service) /{f=0} f' $BR | grep -v '^ *#' | sed 's/^ *//' | tr '\n' '|')" "mkdir /metadata/a6l 0770 root root|rm /metadata/a6l/boot-kmsg.txt.prev|start a6l_bootlog||"
BS=$W/bootlog.sh; tr -d '\r' < $D/debug/a6l-bootlog.sh > $BS
exp "B9 bootlog script syntax" "$(dash -n $BS && echo ok)" ok
# ---- r6c (30 Sep 2026, docs/rom-r6c-20260930.md): logcat + snapshots + budget. Functional runs with shims (sleep = no-op,
# getprop/setprop on $W/props); tiny segments (4 KiB) force the rotation/drop paths; a fake logcat prints and exits.
mkdir -p $W/meta/a6l/pstore-prev $W/pstore; echo old-r6b > $W/meta/a6l/boot-kmsg.txt; head -c 300000 /dev/zero > $W/meta/a6l/boot-kmsg.txt.prev
echo ramoops-console > $W/pstore/console-ramoops-0
for i in $(seq 1 60); do echo "6,$i,1000,-;A6L_STAGE fs: mount_all line $i padding padding padding padding padding"; done > $W/kmsg
printf '#!/bin/sh
echo "$*" > %s/logcat.args
for i in $(seq 1 40); do echo "09-30 12:00:00.000  1167  1167 E minigbm : fake logcat line $i"; done
' $W > $W/bin/fakelogcat; chmod +x $W/bin/fakelogcat
# r6d: fake /data/tombstones (two text tombstones + a .pb that must not be grepped)
mkdir -p $W/tomb; printf 'Cmdline: zygote\npid: 18584, tid: 18584, name: main  >>> zygote <<<\nsignal 6 (SIGABRT), code -1 (SI_QUEUE)\nAbort message: %s\n' "'couldn't find an OpenGL ES implementation'" > $W/tomb/tombstone_00
cp $W/tomb/tombstone_00 $W/tomb/tombstone_01; echo "Cmdline: binary-pb" > $W/tomb/tombstone_00.pb
blrun() { env PATH="$W/bin:$PATH" A6L_BL_DIR=$W/meta/a6l A6L_BL_KMSG=$W/kmsg A6L_BL_PSTORE=$W/pstore A6L_BL_SYSRQ=/dev/null \
          A6L_BL_LOGCAT=$W/bin/fakelogcat A6L_BL_TOMB=$W/tomb A6L_BL_SEG=4096 "$@" dash $BS > $W/bl.out 2>&1; }
printf 'sys.boot_completed=1\nlogd.ready=true\n' > $W/props; blrun
M=$W/meta/a6l
exp "B12 r6b single-file log migrated: tail -> prev/kmsg-r6b.txt, old files gone" "$(cat $M/prev/kmsg-r6b.txt)/$(ls $M | tr '\n' ' ')" "old-r6b/cur prev "
exp "B12 bootlog.txt start / boot_completed / stop" "$(grep -c -E '^A6L_BOOTLOG start uptime=|^A6L_BOOTLOG sys.boot_completed seen at 2s|^A6L_BOOTLOG stop t=122s' $M/cur/bootlog.txt)" 3
exp "B12 kmsg stream from the ring start in kmsg.aaa" "$(head -n 1 $M/cur/kmsg.aaa)" "6,1,1000,-;A6L_STAGE fs: mount_all line 1 padding padding padding padding padding"
exp "B12 kmsg: head 2 + tail 2 segments kept, middle dropped" "$(ls $M/cur/kmsg.* | wc -l)/$([ -f $M/cur/kmsg.aab ] && echo head2)/$(grep -c 'dropped [0-9]* segment(s) kmsg[.]aac[.][.]' $M/cur/bootlog.txt | awk '{print ($1>0)}')" 4/head2/1
exp "B12 logcat stream captured (after logd.ready)" "$(grep -c 'fake logcat line 1$' $M/cur/logcat.aaa | awk '{print ($1>=1)}')" 1
exp "B12 logcat: head 2 + tail 2 segments (r6d)" "$(ls $M/cur/logcat.* | wc -l)/$([ -f $M/cur/logcat.aab ] && echo head2)" 4/head2
exp "R6D-L1 logcat: no kernel buffer, chatty tags filtered" "$(cat $W/logcat.args)" "-b main,system,crash,events,radio -v threadtime SystemServerTiming:S ProcessCpuTracker:S auditd:S PackageCacher:W aconfigd_mainline:W a6l_dualux:W"
exp "R6D-L2 tombstones.txt: listing + Cmdline/Abort of each text tombstone, .pb skipped" "$(grep -c '^== tombstone_0[01]$' $M/cur/tombstones.txt)/$(grep -c '^Abort message: .couldn.t find an OpenGL ES' $M/cur/tombstones.txt)/$(grep -c 'binary-pb' $M/cur/tombstones.txt)/$(grep -c 'tombstone_00.pb' $M/cur/tombstones.txt)" 2/2/0/1
exp "B12 getprop/ps snapshots present" "$(ls $M/cur/props.txt $M/cur/ps.txt | wc -l)" 2
exp "B12 pstore tail copied" "$(cat $M/cur/pstore/console-ramoops-0)" ramoops-console
exp "B12 no segment above the segment size" "$(find $M -name 'kmsg.*' -size +4k -o -name 'logcat.*' -size +4k | wc -l)" 0
blrun
exp "B13 rotation: cur -> prev trimmed (r6d: kmsg 1+1, logcat 1+1, no pstore)" "$(ls $M/prev/kmsg.* | wc -l)/$(ls $M/prev/logcat.* | wc -l)/$(ls -d $M/prev/pstore 2>/dev/null | wc -l)" 2/2/0
exp "B13 prev keeps the first segment of each stream" "$(ls $M/prev/kmsg.aaa $M/prev/logcat.aaa | wc -l)" 2
blrun A6L_BL_DF_AVAIL_KB=100
exp "B14 budget: low free space -> prev deleted, then logcat + snapshots stopped" "$([ -d $M/prev ] && echo prev || echo noprev)/$(grep -c -E 'previous boot deleted|logcat stream and snapshots stopped' $M/cur/bootlog.txt)" noprev/2
exp "B14 budget: kmsg still logged" "$(ls $M/cur/kmsg.aaa | wc -l)/$(grep -c '^A6L_BOOTLOG stop t=' $M/cur/bootlog.txt)" 1/1
rm -rf $M; mkdir -p $M; blrun A6L_BL_CAP_KB=1
exp "B14 budget: size cap (no prev) -> logcat stopped, kmsg bounded" "$(grep -c 'logcat stream and snapshots stopped' $M/cur/bootlog.txt)/$(ls $M/cur/kmsg.* | wc -l | awk '{print ($1<=4)}')" 1/1
exp "B14 metadata budget constants (256 KiB segments, 3 MiB cap, 1 MiB free)" "$(grep -c -E '^SEG=\$\{A6L_BL_SEG:-262144\}; KH=2; KT=2; LH=2; LT=2$|^CAP_KB=\$\{A6L_BL_CAP_KB:-3072\}; FREE_KB=\$\{A6L_BL_FREE_KB:-1024\}' $BS)" 2
exp "B14 logcat = /system/bin/logcat -b \$LCB (r6d)" "$(grep -c 'LOGCAT=\${A6L_BL_LOGCAT:-/system/bin/logcat}' $BS)/$(grep -c '\$LOGCAT -b \$LCB -v threadtime \$LCF' $BS)" 1/1
# ---- r6c: first-boot format without a whole-device discard
fsb=$(block '^on fs$' | grep -v '^ *#' | grep -v '^ *$' | sed 's/^ *//' | tr '\n' '|')
exp "B15 mount_all wrapped by the mke2fs profile export/restore" "$fsb" 'write /dev/kmsg "A6L_STAGE fs: mount_all"|export MKE2FS_CONFIG /vendor/etc/mke2fs.a6l.conf|mount_all /vendor/etc/fstab.qcom|export MKE2FS_CONFIG /system/etc/mke2fs.conf|write /dev/kmsg "A6L_STAGE fs: mount_all done"|'
exp "B15 profile: discard = false in [defaults], ext4 type kept" "$(awk '/^\[defaults\]/{f=1;next} /^\[/{f=0} f' $D/vendor-etc/mke2fs.a6l.conf | grep -c '^ *discard = false$')/$(grep -c 'features = has_journal,extent,huge_file,dir_nlink,extra_isize,uninit_bg' $D/vendor-etc/mke2fs.a6l.conf)" 1/1
exp "B15 rom.mk installs the profile" "$(grep -c 'vendor-etc/mke2fs.a6l.conf:$(TARGET_COPY_OUT_VENDOR)/etc/mke2fs.a6l.conf' $D/rom.mk)" 1
exp "B15 both fstabs keep formattable userdata (the profile applies to both)" "$(grep -c '^/dev/block/by-name/userdata .*formattable' $D/vendor-etc/fstab.qcom $D/vendor-etc/fstab.qcom.fbe | awk -F: '{s+=$2} END {print s}')" 2
printf 'ro.build.type=user\n' > $W/props; env PATH="$W/bin:$PATH" dash $W/m.sh display > $W/u.log 2>&1
exp "B11 user build: display lists do not wait for a boot log" "$(grep -c 'display: boot log' $W/u.log)" 0
printf 'ro.build.type=userdebug\nvendor.a6l.bootlog=running\n' > $W/props; env PATH="$W/bin:$PATH" dash $W/m.sh display > $W/ud.log 2>&1
exp "B11 userdebug: lists load after the boot log runs" "$(grep -o 'display: boot log running after 0x0.2s' $W/ud.log)" "display: boot log running after 0x0.2s"
printf 'ro.build.type=userdebug\nro.bootmode=charger\n' > $W/props; env PATH="$W/bin:$PATH" dash $W/m.sh display > $W/ch.log 2>&1
exp "B11 charger mode: no boot-log wait" "$(grep -c 'display: boot log' $W/ch.log)" 0
exp "B11 bootlog publishes vendor.a6l.bootlog=running" "$(grep -c '^setprop vendor.a6l.bootlog running$' $BS)" 1
exp "B10 cmdline log_buf_len=4M" "$(grep -c "printk.devkmsg=on log_buf_len=4M'" $D/../../../../tools/Prepare-RomV2Boot.py)" 1
exp "B10 ramoops overlay merged by the DT build" "$(grep -c 'a6l-watchdog-v75 a6l-ramoops-v75"' $D/../../../../tools/build-rom-v2-dt.sh)/$(grep -c 'compatible = \"ramoops\"' $D/../kernel/a6l-ramoops-v75.dtso)" 1/1
# ---- H49
exp "H49 on charger starts offcharge" "$(block '^on charger$' | grep -c 'start vendor.a6l_modules_offcharge')" 1
exp "H49 on charger starts nothing else" "$(block '^on charger$' | grep -c 'start ')" 1
exp "H49 guard after offcharge (charger enabled)" "$(block '^on property:init.svc.vendor.a6l_modules_offcharge=stopped && property:persist.vendor.a6l.charger=1$' | grep -c 'start a6l_chg_guard')" 1
exp "H49 offcharge service defined" "$(grep -c '^service vendor.a6l_modules_offcharge /vendor/bin/a6l-modules.sh offcharge$' $RC)" 1
off=$(awk '/^offcharge\)/{f=1;next} f && /^ *;;/{f=0} f' $MS | grep -v '^ *#')
exp "H49 offcharge loads the charger group" "$(grep -c 'load_list charger' <<< "$off")" 1
exp "H49 offcharge loads no other group" "$(grep -o 'load_list [a-z]*' <<< "$off" | sort -u | tr '\n' ' ')" "load_list charger "
exp "H49 offcharge: no remoteproc/radio" "$(grep -c 'remoteproc\|a6l-radio\|camera' <<< "$off")" 0
# ---- fastcharge-rom (29 Sep 2026): QC on with the stock limits in charger.txt, guard 9 V aware, sepolicy for the new writes
CT=$(grep -v '^ *#' $D/modules/charger.txt | awk '$1=="qcom_smbx.ko"')
exp "fastcharge: qcom_smbx options = stock FCC/JEITA + QC 9 V / 2.0 A ICL" "$CT" "qcom_smbx.ko fcc_max_ua=2400000 jeita_hard=1 hvdcp_enable=1 hvdcp_max_uv=9000000 hvdcp_icl_ua=2000000"
GS=$(tr -d '\r' < $D/../power/rom/a6l-chg-guard.sh)
exp "fastcharge: guard drops to 5 V before the thermal limits" "$(grep -c 'hv_set 0 "$state' <<< "$GS")/$(grep -c 'hv_set 0 nodata' <<< "$GS")" 1/1
exp "fastcharge: guard manages the gadget pull-up + rerun" "$(grep -c 'soft_connect' <<< "$GS" | awk '{print ($1>0)}')/$(grep -c 'hvdcp_rerun 1' <<< "$GS")" 1/1
exp "fastcharge: sepolicy genfs for the qcom_smbx parameters" "$(grep -c '^genfscon sysfs /module/qcom_smbx/parameters .*sysfs_a6l_chg_param' $D/sepolicy/vendor/genfs_contexts)" 1
exp "fastcharge: guard may write sysfs_udc + the parameters" "$(grep -c -E '^allow a6l_chg_guard (sysfs_udc|sysfs_a6l_chg_param):file' $D/sepolicy/vendor/a6l_fastcharge.te)" 2
# ---- r6e (1 Oct 2026, docs/rom-r6e-20261001.md): no online discard, radio binaries executable, live kernel log, IO-stall detector
exp "R6E-1 fstab.qcom: no discard mount option" "$(tr -d '\r' < $D/vendor-etc/fstab.qcom | grep -v '^ *#' | grep -c discard)" 0
exp "R6E-1 fstab.qcom.fbe: no discard mount option" "$(tr -d '\r' < $D/vendor-etc/fstab.qcom.fbe | grep -v '^ *#' | grep -c discard)" 0
exp "R6E-1 metadata/data still formattable + check" "$(tr -d '\r' < $D/vendor-etc/fstab.qcom | grep -E '^/dev/block/by-name/(metadata|userdata) ' | grep -c 'wait,check,formattable')" 2
CF=$(tr -d '\r' < $D/config.fs)
exp "R6E-2 config.fs: /vendor/a6l/radio/bin/* 0755" "$(awk '/^\[vendor\/a6l\/radio\/bin\/\*\]$/{f=1;next} /^\[/{f=0} f && /^mode:/{print $2}' <<< "$CF")" 0755
exp "R6E-2 BoardConfig-rom.mk uses config.fs" "$(tr -d '\r' < $D/BoardConfig-rom.mk | grep -c '^TARGET_FS_CONFIG_GEN += device/hisense/a6l/rom/config.fs$')" 1
DR=$(tr -d '\r' < $D/debug/init.a6l.bootlog-debug.rc)
pfd=$(awk '/^on post-fs-data$/{f=1;next} /^(on|service) /{f=0} f' <<< "$DR")
exp "R6E-3 debug rc: /dev/kmsg 0644, iowatch at post-fs-data" "$(grep -c -E '^ *(chmod 0644 /dev/kmsg|start a6l_iowatch)$' <<< "$pfd")" 2
# selinux-20261007 pass 2: dmesg_restrict 0 is written from system_ext (main init; vendor_init may not write proc_security)
SD=$(tr -d '\r' < $D/debug/init.a6l.system-debug.rc)
exp "R6E-3 system_ext debug rc: dmesg_restrict 0 at post-fs-data and boot_completed" "$(grep -c -E '^ *write /proc/sys/kernel/dmesg_restrict 0$' <<< "$SD")" 2
exp "R6E-3 no dmesg_restrict write left in the vendor debug rc" "$(grep -c 'write /proc/sys/kernel/dmesg_restrict' <<< "$DR")" 0
exp "R6E-3 bootlog.mk installs the system_ext debug rc" "$(tr -d '\r' < $D/debug/bootlog.mk | awk '/^ifneq/{f=1} /^endif/{f=0} f' | grep -c 'init.a6l.system-debug.rc:$(TARGET_COPY_OUT_SYSTEM_EXT)/etc/init/init.a6l.system-debug.rc')" 1
exp "R6E-3 iowatch service (su, not oneshot)" "$(awk '/^service a6l_iowatch /{f=1;next} /^(on|service) /{f=0} f' <<< "$DR" | grep -c -E '^ *(seclabel u:r:su:s0|disabled|oneshot)$')" 2
exp "R6E-3 bootlog.mk installs the detector (userdebug only block)" "$(tr -d '\r' < $D/debug/bootlog.mk | awk '/^ifneq/{f=1} /^endif/{f=0} f' | grep -c 'a6l-iowatch.sh:$(TARGET_COPY_OUT_VENDOR)/bin/a6l-iowatch.sh')" 1
# simulated stall: the /metadata heartbeat blocks on a FIFO for 8 s (LIMIT 3 s) -> report + sysrq w,l, then recovered
I=$W/iw; mkdir -p $I/bin $I/m $I/d; printf '#!/bin/sh\nexec sync "$@"\n' > $I/bin/fsync; chmod +x $I/bin/fsync
tr -d '\r' < $D/debug/a6l-iowatch.sh > $I/iw.sh
PATH=$I/bin:$PATH A6L_IW_KMSG=$I/kmsg A6L_IW_SYSRQ=$I/sysrq A6L_IW_META=$I/m/hb A6L_IW_DATA=$I/d/hb A6L_IW_TEST=fifo A6L_IW_TEST_RELEASE=8 \
  A6L_IW_LIMIT=3 A6L_IW_REARM=4 A6L_IW_ALIVE=10 A6L_IW_MAXTICKS=14 timeout 60 bash $I/iw.sh 2>/dev/null
exp "R6E-4 stall reported (A6L_IOSTALL target=.../m/hb)" "$(grep -c "^A6L_IOSTALL target=$I/m/hb " $I/kmsg | awk '{print ($1>=1)}')" 1
exp "R6E-4 stall recovered" "$(grep -c "^A6L_IOSTALL recovered target=$I/m/hb after" $I/kmsg)" 1
exp "R6E-4 sysrq w then l per report" "$(head -2 $I/sysrq | tr '\n' ' ')" "w l "
exp "R6E-4 /data heartbeat never reported" "$(grep -c "A6L_IOSTALL target=$I/d/hb" $I/kmsg)" 0
exp "R6E-4 start line + mounts read" "$(grep -c '^A6L_IOWATCH start emmc=' $I/kmsg)" 1
rm -rf $W
echo "A6L_ROM_STATIC_TEST $([ $fails = 0 ] && echo PASS || echo FAIL $fails)"; exit $fails
