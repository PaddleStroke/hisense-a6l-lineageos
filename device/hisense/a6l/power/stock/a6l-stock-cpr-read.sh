#!/system/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# a6l-stock-cpr-read.sh (agent power, 26 Sep 2026) - READ-ONLY dump of the CPU CPR/OSM fuse-derived data from the
# ROOTED STOCK Android (msm-4.4 kernel). Replaces the V74 MODE=fuses qfprom read, which RESET the phone on 24 Sep.
# Nothing here touches qfprom, a regulator, a clock or a frequency. It only reads the kernel log, debugfs and sysfs.
# Run from the laptop (stock booted, Magisk su; best within ~2 min of a fresh boot so the boot log is still in dmesg):
#   adb -s 1e529013 push a6l-stock-cpr-read.sh /data/local/tmp/ && \
#   adb -s 1e529013 shell su -c 'sh /data/local/tmp/a6l-stock-cpr-read.sh' > stock-cpr-$(date +%H%M%S).txt 2>&1
# Optional CORNERS=1 (su -c 'CORNERS=1 sh ...'): walks the downstream cpr3 debugfs *display* selector corner/index
# (cpr3_debug_corner_index_set only stores vreg->debug_corner, msm-4.4 cpr3-regulator.c; no hardware access) to print
# every corner's open-loop/floor/ceiling voltage. It restores the original index at the end.
say() { echo "A6L_STOCK $*"; }
say "BEGIN $(date) uptime=$(cut -d' ' -f1 /proc/uptime)s id=$(id -u) kernel=$(uname -r)"
say "fingerprint=$(getprop ro.build.fingerprint) hw=$(getprop ro.hardware) board=$(getprop ro.product.board)"
for f in soc_id revision foundry_id serial_number raw_id raw_version hw_platform platform_subtype pmic_model; do
    [ -r /sys/devices/soc0/$f ] && say "soc0 $f=$(cat /sys/devices/soc0/$f 2>/dev/null)"
done
echo "===== 1. kernel log (boot-time CPR prints: speed bin, fusing rev, fused open-loop, quotients)"
dmesg 2>/dev/null > /data/local/tmp/a6l-dmesg.txt
n=$(grep -ciE 'cpr|kbss|apc[01]|mem.acc|osm|speed.bin|fusing|open-loop' /data/local/tmp/a6l-dmesg.txt)
grep -iE 'cpr|kbss|apc[01]_|apc[01]:|mem.acc|clk-cpu-osm|osm|speed.bin|fusing|open-loop|quot\[|lmh|msm_thermal' /data/local/tmp/a6l-dmesg.txt | head -400 | sed 's/^/A6L_KLOG /'
say "klog_lines=$n first_line: $(head -1 /data/local/tmp/a6l-dmesg.txt | cut -c1-80)"
grep -q 'fused.*open-loop' /data/local/tmp/a6l-dmesg.txt || say "WARN no 'fused ... open-loop' lines: the boot log has rotated. Reboot stock and run this again within 2 minutes of boot."
for p in /sys/fs/pstore/console-ramoops* /proc/last_kmsg; do
    [ -r "$p" ] && grep -iE 'fused.*open-loop|speed bin|fusing revision|quot\[' "$p" | head -60 | sed "s|^|A6L_LASTKMSG ${p##*/} |"
done
echo "===== 2. cpr3-regulator debugfs (read-only files)"
DBG=/sys/kernel/debug; [ -d $DBG/cpr3-regulator ] || mount -t debugfs debugfs $DBG 2>/dev/null
C=$DBG/cpr3-regulator
if [ -d $C ]; then
    find $C -type f 2>/dev/null | sort | while read -r f; do
        case "$f" in *trigger*|*aging*|*/corner/index) continue;; esac   # write-only / selector handled below
        v=$(timeout 2 cat "$f" 2>/dev/null | tr '\n' ' ' | cut -c1-300)
        say "DBG ${f#$C/} = $v"
    done
    if [ "${CORNERS:-0}" = 1 ]; then
        for idx in $(find $C -path '*/corner/index' 2>/dev/null); do
            d=${idx%/index}; vr=${d%/corner}; cnt=$(cat $vr/corner_count 2>/dev/null); orig=$(cat $idx 2>/dev/null)
            say "CORNERS ${vr#$C/} count=$cnt orig_index=$orig"
            i=1; while [ -n "$cnt" ] && [ $i -le $cnt ]; do
                echo $i > $idx 2>/dev/null || { say "CORNERS index write refused at $i"; break; }
                say "CORNER ${vr#$C/} $i open_loop=$(cat $d/open_loop_volt 2>/dev/null) floor=$(cat $d/floor_volt 2>/dev/null) ceiling=$(cat $d/ceiling_volt 2>/dev/null) last=$(cat $d/last_volt 2>/dev/null) quots=$(cat $d/target_quots 2>/dev/null | tr '\n' ' ')"
                i=$((i+1))
            done
            [ -n "$orig" ] && echo $orig > $idx 2>/dev/null
        done
    else
        say "CORNERS skipped (run with CORNERS=1 for the per-corner table)"
    fi
else
    say "WARN no $C (debugfs not mounted / not built)"
fi
echo "===== 3. regulator + clock debugfs for the CPU rails (read-only)"
for r in $DBG/regulator/*apc* $DBG/regulator/*pwrcl* $DBG/regulator/*perfcl*; do
    [ -d "$r" ] || continue
    for f in voltage enable use_count consumers; do [ -r $r/$f ] && say "REG ${r##*/}/$f = $(timeout 2 cat $r/$f 2>/dev/null | tr '\n' ' ' | cut -c1-200)"; done
done
for c in $DBG/clk/pwrcl_clk $DBG/clk/perfcl_clk $DBG/clk/cbf_clk; do
    [ -d "$c" ] && say "CLK ${c##*/} rate=$(cat $c/clk_rate 2>/dev/null) measure=$(timeout 2 cat $c/clk_measure 2>/dev/null)"
done
echo "===== 4. cpufreq (sysfs, read-only)"
for p in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -d "$p" ] || continue
    say "CPUFREQ ${p##*/} cpus=$(cat $p/related_cpus) cur=$(cat $p/scaling_cur_freq) min=$(cat $p/cpuinfo_min_freq) max=$(cat $p/cpuinfo_max_freq) gov=$(cat $p/scaling_governor)"
    say "CPUFREQ ${p##*/} avail=$(cat $p/scaling_available_frequencies 2>/dev/null)"
done
echo "===== 5. thermal zones"
for z in /sys/class/thermal/thermal_zone*; do say "THERM ${z##*/} $(cat $z/type 2>/dev/null)=$(cat $z/temp 2>/dev/null)"; done
echo "===== 6. charger/FG reference (stock qpnp-smb2 / fg-gen3 policy, read-only)"
for s in battery usb bms main; do
    P=/sys/class/power_supply/$s; [ -d $P ] || continue
    for f in status health charge_type current_now voltage_now temp capacity input_current_limited constant_charge_current_max input_current_settled current_max voltage_max step_charging_enabled sw_jeita_enabled; do
        [ -r $P/$f ] && say "PSY $s/$f=$(cat $P/$f 2>/dev/null)"
    done
done
rm -f /data/local/tmp/a6l-dmesg.txt
say "DONE"
