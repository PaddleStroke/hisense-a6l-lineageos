#!/system/bin/sh
# A6L diag-r5 module test kit, PHONE side (30 Sep 2026, docs/diag-r5-20260930.md).
# Runs inside the diag boot image (r5p/V67 kernel + V75-usb RAM ramdisk) or the V75 recovery. RAM only: loads kernel
# modules and reads sysfs/debugfs; the only writes are to /tmp, /lib/firmware (rootfs RAM), /proc/sys, sysrq, kmsg and
# (steps gpuoff/dispoff) a platform driver_override. Never rmmod msm (oops in adreno_remove). No eMMC writes.
# Start it DETACHED from the laptop (tools on the laptop: host/diag-run.sh) so a freeze cannot kill the log stream:
#   adb shell 'cd /tmp/diag && /system/bin/toybox setsid /system/bin/sh ./d.sh <step> [args] </dev/null >>log.txt 2>&1 &'
# Every action is announced in kmsg BEFORE it runs ("A6L_DIAG <step> BEGIN insmod msm.ko ..."), so the last line in the
# laptop's `dmesg -w` stream (and in pstore console-ramoops after a freeze) names what never returned.
# Steps: setup | base | pre | msm [params] | gpuoff | dispoff | panels | one <ko> [params] | rom | snap <tag> | ioload [s] |
#        status | pstore | qemu (offline self-test)
set -u
T=/system/bin/toybox
if [ ! -x /tmp/bin/grep ]; then $T mkdir -p /tmp/bin; for a in $($T); do $T ln -sf $T /tmp/bin/$a; done; fi
export PATH=/tmp/bin:/system/bin
D=${D:-$(cd "$(dirname "$0")" && pwd)}
STEP=${1:-status}; [ $# -gt 0 ] && shift
k() { echo "A6L_DIAG $STEP $*" > /dev/kmsg; echo "A6L_DIAG $STEP $*"; }
up() { read u _ < /proc/uptime; echo $u; }
diagtag=$(tr ' ' '\n' < /proc/cmdline | grep '^androidboot.a6l_diag=' | cut -d= -f2)
# module set: MODVERSIONS makes r5 and V67 builds mutually refused; the boot image tag decides (A6L_DIAG_MODS overrides):
# r5p-* -> mods-r5, k1-* -> mods-k1, r5m-* -> mods-r5m (each kernel its own builds), v67-dt6b or no tag (= V74/V75 recovery, V67 kernel) -> mods-v67
case "$diagtag" in r5m*) M=$D/mods-r5m ;; k1*) M=$D/mods-k1 ;; r5*) M=$D/mods-r5 ;; *) M=$D/mods-v67 ;; esac
[ -n "${A6L_DIAG_MODS:-}" ] && M=$D/$A6L_DIAG_MODS
loaded() { grep -q "^$(echo "$1" | tr - _) " /proc/modules; }
# ins <ko> [params]: announce, insmod in the background, watch it; stacks + sysrq-w/l if it does not return
ins() {
    ko=$1; shift; n=${ko%.ko}
    if loaded "$n"; then k "SKIP $ko already loaded"; return 0; fi
    [ -f "$M/$ko" ] || { k "FAIL missing $M/$ko"; return 2; }
    k "BEGIN insmod $ko $* t=$(up)"; sleep ${A6L_DIAG_PAUSE:-2}
    insmod "$M/$ko" "$@" & p=$!
    i=0; rc=
    while :; do
        if ! kill -0 $p 2>/dev/null; then wait $p; rc=$?; break; fi
        sleep 1; i=$((i+1))
        if [ $i = 15 ] || [ $i = 45 ] || [ $i = 120 ]; then
            k "STUCK insmod $ko ${i}s pid=$p wchan=$(cat /proc/$p/wchan 2>/dev/null) state=$(grep '^State' /proc/$p/status 2>/dev/null | tr '\t' ' ')"
            cat /proc/$p/stack 2>/dev/null | while read -r l; do k "STACK $l"; done
            echo w > /proc/sysrq-trigger; echo l > /proc/sysrq-trigger
        fi
    done
    if [ "$rc" = 0 ]; then k "OK insmod $ko t=$(up)"; else k "FAIL insmod $ko rc=$rc t=$(up)"; dmesg | tail -n 8 | while read -r l; do k "TAIL $l"; done; fi
    sleep 1
    return $rc
}
list() { grep -v '^[[:space:]]*#' "$D/lists/$1" | grep -v '^[[:space:]]*$'; }
prelist() { list display.txt | while read -r ko a; do [ "$ko" = msm.ko ] && break; echo "$ko"; done; }
postlist() { list display.txt | sed -n '/^msm.ko/,$p' | grep -v '^msm.ko'; }
snap() {
    tag=$1; f=/tmp/diag/snap-$tag.txt; mkdir -p /tmp/diag
    {
        echo "== snap $tag uptime=$(up) diag=$diagtag uname=$(uname -r)"; cat /proc/cmdline
        echo "== modules"; cat /proc/modules
        echo "== deferred"; cat /sys/kernel/debug/devices_deferred 2>/dev/null
        echo "== genpd"; cat /sys/kernel/debug/pm_genpd/pm_genpd_summary 2>/dev/null
        echo "== clk (mmss/mdss/gpu/gfx/bimc/dsi/mdp/byte/pclk/esc/ahb/axi)"; grep -iE "mmss|mdss|gpu|gfx|bimc|dsi|mdp|byte|pclk|esc|mnoc|snoc|cnoc" /sys/kernel/debug/clk/clk_summary 2>/dev/null
        echo "== regulators"; cat /sys/kernel/debug/regulator/regulator_summary 2>/dev/null
        echo "== interconnect"; cat /sys/kernel/debug/interconnect/interconnect_summary 2>/dev/null
        echo "== platform gpu/mdss"; for d in /sys/bus/platform/devices/*.gpu /sys/bus/platform/devices/*display-subsystem* /sys/bus/platform/devices/*.iommu; do
            [ -e "$d" ] || continue; echo "$d driver=$(readlink $d/driver 2>/dev/null) override=$(cat $d/driver_override 2>/dev/null) rpm=$(cat $d/power/runtime_status 2>/dev/null)"; done
        echo "== drm"; ls -l /sys/class/drm 2>/dev/null
        echo "== meminfo"; head -n 5 /proc/meminfo
    } > $f 2>&1
    k "SNAP $tag saved $f ($(wc -l < $f) lines)"
}
setup() {
    if [ "${A6L_DIAG_NOKFENCE:-0}" = 1 ] && [ -e /sys/module/kfence/parameters/sample_interval ]; then
        echo 0 > /sys/module/kfence/parameters/sample_interval; k "KFENCE sampling off (sample_interval=$(cat /sys/module/kfence/parameters/sample_interval))"; fi
    k "BEGIN setup diag=$diagtag modset=$M uname=$(uname -r) t=$(up)"
    grep -q " /sys/kernel/debug " /proc/mounts || mount -t debugfs debugfs /sys/kernel/debug
    echo 1 > /proc/sys/kernel/sysrq
    echo "8 4 1 8" > /proc/sys/kernel/printk
    for s in softlockup_panic:0 hardlockup_panic:0 softlockup_all_cpu_backtrace:1 hardlockup_all_cpu_backtrace:1 watchdog_thresh:5 \
             printk_ratelimit:0 printk_ratelimit_burst:10000 panic_on_oops:0 panic_on_warn:0; do
        f=/proc/sys/kernel/${s%%:*}; [ -e $f ] && echo ${s##*:} > $f 2>/dev/null
    done
    k "sysctl sysrq=$(cat /proc/sys/kernel/sysrq) printk=$(cat /proc/sys/kernel/printk | tr '\t' ' ') softlockup_panic=$(cat /proc/sys/kernel/softlockup_panic 2>/dev/null) watchdog_thresh=$(cat /proc/sys/kernel/watchdog_thresh 2>/dev/null) hung_task=$(ls /proc/sys/kernel/hung_task_timeout_secs 2>/dev/null || echo absent)"
    mkdir -p /lib/firmware && cp -r "$D"/fw/* /lib/firmware/ && k "firmware -> /lib/firmware ($(find /lib/firmware -type f | wc -l) files)"
    mkdir -p /sys/fs/pstore 2>/dev/null; grep -q " /sys/fs/pstore " /proc/mounts || mount -t pstore pstore /sys/fs/pstore 2>/dev/null
    mkdir -p /tmp/diag/pstore-prev; cp /sys/fs/pstore/* /tmp/diag/pstore-prev/ 2>/dev/null
    k "pstore: $(ls /sys/fs/pstore 2>/dev/null | tr '\n' ' ') ramoops=$(ls /sys/module/ramoops 2>/dev/null >/dev/null && echo yes || echo no) $(grep -i ramoops /proc/iomem 2>/dev/null | head -n 1)"
    if [ ! -e /tmp/diag/hb.pid ] || ! kill -0 $(cat /tmp/diag/hb.pid) 2>/dev/null; then
        setsid sh -c 'while :; do read u _ < /proc/uptime; echo "A6L_DIAG hb $u" > /dev/kmsg; sleep ${A6L_DIAG_HB:-2}; done' </dev/null >/dev/null 2>&1 &
        echo $! > /tmp/diag/hb.pid
    fi
    k "DONE setup heartbeat pid=$(cat /tmp/diag/hb.pid) t=$(up)"
}
msm() {
    [ -e /lib/firmware/qcom/a530_pm4.fw ] || setup
    ins msm.ko separate_gpu_kms=1 "$@"; rc=$?
    sleep 5
    k "after msm: drm=$(ls /sys/class/drm 2>/dev/null | tr '\n' ' ') gpu_drv=$(readlink /sys/bus/platform/devices/5000000.gpu/driver 2>/dev/null)"
    return $rc
}
override() { # override <glob> <tag>: keep msm from binding that device (driver_override = a name no driver has)
    for d in $1; do [ -e "$d/driver_override" ] || continue
        [ -e "$d/driver" ] && { k "FAIL $d already bound to $(readlink $d/driver)"; return 3; }
        echo a6l-diag-none > $d/driver_override && k "OVERRIDE $d -> a6l-diag-none ($2)"; return 0; done
    k "FAIL no device for $1"; return 3
}
case $STEP in
setup) setup ;;
base) setup; for ko in $(list base.txt); do ins $ko; done; k "DONE base" ;;
pre) [ -e /lib/firmware/qcom/a530_pm4.fw ] || setup; for ko in $(prelist); do ins $ko || exit 4; done; k "DONE pre (up to msm) t=$(up)"; snap pre ;;
msm) msm "$@"; k "DONE msm rc=$?" ;;
gpuoff) override "/sys/bus/platform/devices/*.gpu" "adreno not probed: msm = display only" ;;
dispoff) override "/sys/bus/platform/devices/*.display-subsystem" "mdss not probed: msm = GPU only" ;;
panels) postlist | while read -r ko a; do ins $ko $a; done; sleep 8
        for c in /sys/class/drm/card*-*; do [ -e $c/status ] && k "connector ${c##*/} status=$(cat $c/status) modes=$(head -n 1 $c/modes)"; done; k "DONE panels"; snap panels ;;
one) ins "$@" ;;
rom) setup; for ko in $(list base.txt); do ins $ko; done
     list display.txt | while read -r ko a; do ins $ko $a; done; k "DONE rom-order"; snap rom ;;
snap) snap "${1:-now}" ;;
ioload) # READ-ONLY eMMC load (the ROM loaded msm while mke2fs formatted userdata): reads userdata for N s
     s=${1:-60}; d=; for b in /sys/class/block/mmcblk*p*; do grep -qx PARTNAME=userdata $b/uevent && d=$(cat $b/dev); done
     [ -n "$d" ] || { k "FAIL ioload: no userdata (insmod /sdhci-msm.ko first)"; exit 5; }
     mkdir -p /dev/block/a6ldiag; rm -f /dev/block/a6ldiag/ud; mknod /dev/block/a6ldiag/ud b ${d%%:*} ${d##*:}
     setsid sh -c "dd if=/dev/block/a6ldiag/ud of=/dev/null bs=1M count=100000 & p=\$!; sleep $s; kill \$p" </dev/null >/dev/null 2>&1 &
     k "ioload read-only userdata $d for ${s}s started" ;;
status) k "status diag=$diagtag uname=$(uname -r) modset=$M t=$(up)"; cat /proc/modules | cut -d' ' -f1 | tr '\n' ' '; echo; ls /sys/class/drm 2>/dev/null ;;
pstore) ls -la /tmp/diag/pstore-prev/ 2>/dev/null; for f in /tmp/diag/pstore-prev/*; do [ -f "$f" ] && { echo "== $f"; tail -n 60 "$f"; }; done ;;
qemu) # offline self-test (QEMU virt, no A6L hardware): every kit module must load on this kernel (vermagic/CRCs/symbols)
     setup; fails=0
     for ko in $(list base.txt) $(prelist); do ins $ko || fails=$((fails+1)); done
     A6L_DIAG_PAUSE=0 msm || fails=$((fails+1))
     for ko in $(postlist | cut -d' ' -f1); do A6L_DIAG_PAUSE=0 ins $ko || fails=$((fails+1)); done
     override "/sys/bus/platform/devices/*.gpu" test; snap qemu
     k "QEMU_SELFTEST fails=$fails modules=$(wc -l < /proc/modules)"; kill $(cat /tmp/diag/hb.pid) ;;
*) echo "usage: d.sh setup|base|pre|msm [params]|gpuoff|dispoff|panels|one <ko> [params]|rom|snap <tag>|ioload [s]|status|pstore"; exit 1 ;;
esac
