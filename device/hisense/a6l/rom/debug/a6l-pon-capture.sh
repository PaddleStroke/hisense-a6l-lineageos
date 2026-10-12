#!/vendor/bin/sh
# incident-20261012-1203: why did the LAST reset happen, and what were VBAT/IBAT doing before it. Debug builds only.
# READ-ONLY on the PMICs (regmap debugfs reads; nothing is written to any PMIC register). Root (init service or su).
#
#   a6l-pon-capture.sh kmsg            decoded one-line summary -> /dev/kmsg (init `on fs`: lands in krec + bootlog rings
#                                      even if the boot dies before /data is mounted)
#   a6l-pon-capture.sh boot            full capture -> /data/vendor/a6l-pon/boot-NNNN.txt (NNNN = boot counter kept in
#                                      /data/vendor/a6l-pon/bootcount; the newest 64 captures are kept) + summary in kmsg
#   a6l-pon-capture.sh now             full capture to stdout (attended: adb shell su -c "sh a6l-pon-capture.sh now")
#   a6l-pon-capture.sh watch [s] [min] power trace -> /data/vendor/a6l-pon/watch-NNNN.csv, one sample every s seconds
#                                      (default 1) for min minutes (default 0 = until stopped). A line is written (and
#                                      fsync'ed) when VBAT/IBAT/BCL/gold frequency change noticeably, else every 30 s, so
#                                      the last line before a sudden reset is at most ~1 s old under load. 4 MiB cap.
#
# Registers (PM660 = SPMI SID 0, regmap "0-00"; PM660L = SID 2, regmap "0-02"), PON gen2 layout from downstream
# qpnp-power-on.c (PON_PERPH_SUBTYPE 0x805 = 0x04 = GEN2 primary):
#   0x808 PON_REASON1  b0 HARD_RESET b1 SMPL b2 RTC b3 DC b4 USB b5 PON1 b6 CBL b7 KPDPWR   (what powered it on)
#   0x8c7 OFF_REASON   b7 POFF_SEQ -> POFF_REASON1 0x80c; b6 FAULT_SEQ -> FAULT_REASON1/2 0x8c8/9;
#                      b5 S3_RESET_SEQ -> S3_RESET_REASON 0x8ca                              (which register is current)
#   0x87f SMPL_CTL b7 enable; 0x874 S3_SRC; 0x85a/b PS_HOLD type/en; 0x840-0x843 KPDPWR S1/S2
#   PM660 FG BCL 0x4200 (stock bcl@4200, bcl_peripheral.c): 0x4246 MONITOR_EN b7, 0x425c VBAT_MIN (x39 mV, copy 0x425e),
#   0x425d IBAT_MAX (signed x80 mA, copy 0x425f): latched min/max since the last clear (cleared only by a6l_bcl.ko).
# mksh note: never name a helper "r" (mksh predefines alias r='fc -e -': that is what emptied thermlog2.sh's columns).
umask 077
OUT=${A6L_PON_DIR:-/data/vendor/a6l-pon}
DBG=${A6L_PON_DBG:-/sys/kernel/debug}
KEEP=64
MNT=0
for TD in ${TMPDIR:-/data/local/tmp} /dev; do [ -d $TD ] && [ -w $TD ] && break; done
TMP=$TD/a6l-pon.$$

dbg_on() {
    [ -d $DBG/regmap ] && return 0
    mount -t debugfs debugfs $DBG 2>/dev/null && MNT=1
    [ -d $DBG/regmap ]
}
dbg_off() { [ $MNT = 1 ] && umount $DBG 2>/dev/null; MNT=0; }
trap 'dbg_off; rm -f $TMP.*' EXIT
trap 'exit 0' HUP INT TERM

# page SID FIRST COUNT -> "aaaa vv" lines. regmap debugfs lines are "aaaa: vv\n" (9 bytes) for a 16-bit register map.
page() {
    f=$DBG/regmap/0-0$1/registers
    [ -r $f ] || return 1
    dd if=$f bs=9 skip=$((0x$2)) count=$3 2>/dev/null | tr -d ':'
}
# reg FILE ADDR -> value (hex, no 0x) or "??"
reg() { v=$(awk -v a=$2 '$1 == a { print $2; exit }' $1 2>/dev/null); echo ${v:-??}; }
# names VALHEX "n0 n1 .. n7" -> names of the set bits ("-" = unused bit)
names() {
    case $1 in [0-9a-f][0-9a-f]) ;; *) echo "?"; return ;; esac
    v=$((0x$1)); i=0; o=
    for n in $2; do
        [ $(( (v >> i) & 1 )) = 1 ] && { [ "$n" = - ] && o="$o bit$i" || o="$o $n"; }
        i=$((i + 1))
    done
    echo "${o# }"
}
# s8 VALHEX -> signed decimal
s8() { case $1 in [0-9a-f][0-9a-f]) ;; *) echo 0; return ;; esac; v=$((0x$1)); [ $v -ge 128 ] && v=$((v - 256)); echo $v; }

# decode PON page file of one PMIC -> summary text on stdout; sets SUM (one line)
decode() {
    f=$1; who=$2
    p1=$(reg $f 0808); off=$(reg $f 08c7); w1=$(reg $f 080a); w2=$(reg $f 080b); o1=$(reg $f 080c); o2=$(reg $f 080d)
    f1=$(reg $f 08c8); f2=$(reg $f 08c9); s3=$(reg $f 08ca); smpl=$(reg $f 087f); s3src=$(reg $f 0874)
    sub=$(reg $f 0805)
    seq=$(names $off "- - - - - S3_RESET_SEQ FAULT_SEQ POFF_SEQ")
    pon=$(names $p1 "HARD_RESET SMPL RTC DC_CHG USB_CHG PON1 CBL KPDPWR")
    echo "== $who PON (subtype 0x$sub)"
    echo "PON_REASON1     0x808 = 0x$p1 [$pon]   (PON_REASON2 0x809 = 0x$(reg $f 0809))"
    echo "OFF_REASON      0x8c7 = 0x$off [$seq]"
    echo "POFF_REASON1/2  0x80c = 0x$o1 [$(names $o1 "SOFT PS_HOLD PMIC_WD GP1 GP2 KPDPWR_AND_RESIN RESIN_N KPDPWR_N")] 0x80d = 0x$o2 [$(names $o2 "- - - CHARGER TFT UVLO OTST3 STAGE3")]"
    echo "WARM_RESET1/2   0x80a = 0x$w1 [$(names $w1 "SOFT PS_HOLD PMIC_WD GP1 GP2 KPDPWR_AND_RESIN RESIN_N KPDPWR_N")] 0x80b = 0x$w2"
    echo "SOFT_RESET1/2   0x80e = 0x$(reg $f 080e) 0x80f = 0x$(reg $f 080f)"
    echo "FAULT_REASON1/2 0x8c8 = 0x$f1 [$(names $f1 "GP_FAULT0 GP_FAULT1 GP_FAULT2 GP_FAULT3 MBG_FAULT OVLO UVLO AVDD_RB")] 0x8c9 = 0x$f2 [$(names $f2 "- - - FAULT_N PBS_WATCHDOG_TO PBS_NACK RESTART_PON OTST3")]"
    echo "S3_RESET_REASON 0x8ca = 0x$s3 [$(names $s3 "- - - - FAULT_N PBS_WATCHDOG_TO PBS_NACK KPDPWR_ANDOR_RESIN")]"
    case $smpl in [0-9a-f][0-9a-f]) sm="en=$(( (0x$smpl >> 7) & 1 )) delay_code=$((0x$smpl & 3))" ;; *) sm=? ;; esac
    case $s3src in [0-9a-f][0-9a-f]) s3s=$((0x$s3src & 3)) ;; *) s3s=? ;; esac
    echo "SMPL_CTL 0x87f = 0x$smpl ($sm; 0..3 = 0.5/1/1.5/2 s)  S3_SRC 0x874 = 0x$s3src (src $s3s: 0 KPDPWR 1 RESIN 2 AND 3 OR) S3_DBC 0x875 = 0x$(reg $f 0875)"
    echo "PS_HOLD 0x85a/b = 0x$(reg $f 085a)/0x$(reg $f 085b)  KPDPWR S1_TIMER 0x840 = 0x$(reg $f 0840) S2_TIMER 0x841 = 0x$(reg $f 0841) S2_TYPE 0x842 = 0x$(reg $f 0842) S2_EN 0x843 = 0x$(reg $f 0843)"
    echo "(reset types: 1 WARM_RESET 4 SHUTDOWN 5 DVDD_SHUTDOWN 7 HARD_RESET 8 DVDD_HARD_RESET)"
    case "$seq" in
        *POFF_SEQ*)     cur="POFF[$(names $o1 "SOFT PS_HOLD PMIC_WD GP1 GP2 KPDPWR_AND_RESIN RESIN_N KPDPWR_N")]" ;;
        *FAULT_SEQ*)    cur="FAULT[$(names $f1 "GP_FAULT0 GP_FAULT1 GP_FAULT2 GP_FAULT3 MBG_FAULT OVLO UVLO AVDD_RB") $(names $f2 "- - - FAULT_N PBS_WATCHDOG_TO PBS_NACK RESTART_PON OTST3")]" ;;
        *S3_RESET_SEQ*) cur="S3[$(names $s3 "- - - - FAULT_N PBS_WATCHDOG_TO PBS_NACK KPDPWR_ANDOR_RESIN")]" ;;
        *)              cur="none(OFF_REASON=0x$off)" ;;
    esac
    SUM="$SUM $who:on=0x$p1[$pon] off=$cur warm=0x$w1"
}

bcl_line() {   # $1 = file with the 0x4200 page of SID 0
    mon=$(reg $1 4246); vm=$(reg $1 425c); vc=$(reg $1 425e); im=$(reg $1 425d); ic=$(reg $1 425f)
    case $vm in [0-9a-f][0-9a-f]) vmv=$(( 0x$vm * 39 ));; *) vmv=?;; esac
    case $im in [0-9a-f][0-9a-f]) ima=$(( $(s8 $im) * 80 ));; *) ima=?;; esac
    [ "$vm" = 7f ] && vmv="no-reading"
    case $mon in [0-9a-f][0-9a-f]) en=$(( (0x$mon >> 7) & 1 ));; *) en=?;; esac
    BCL="bcl_mon=$en vbat_min=${vmv}mV(0x$vm/0x$vc) ibat_max=${ima}mA(0x$im/0x$ic)"
}

ps_line() {
    for p in /sys/class/power_supply/*; do
        l="${p##*/}:"
        for a in status online capacity voltage_now voltage_avg current_now current_max input_current_limit temp; do
            [ -r $p/$a ] && l="$l $a=$(cat $p/$a 2>/dev/null)"
        done
        echo "$l"
    done
}

capture() {   # full report on stdout, SUM = one-line summary
    SUM=
    echo "a6l-pon-capture $(date '+%F %T %z') uptime=$(cut -d' ' -f1 /proc/uptime) boot_id=$(cat /proc/sys/kernel/random/boot_id)"
    echo "cmdline: $(tr ' ' '\n' < /proc/cmdline | grep -E '^(powerup_reason|androidboot.bootreason|androidboot.mode)=' | tr '\n' ' ')"
    if ! dbg_on; then echo "debugfs/regmap unavailable: use a6l_warmreset.ko (crash-20261009/out) instead"; SUM=" no-debugfs"; return; fi
    for sid in 0 2; do
        page $sid 0800 256 > $TMP.pon$sid 2>/dev/null
        if [ "$(head -1 $TMP.pon$sid | cut -d' ' -f1)" != 0800 ]; then
            echo "SID$sid: regmap 0-0$sid missing or unexpected line format: $(head -1 $TMP.pon$sid)"; continue
        fi
        [ $sid = 0 ] && decode $TMP.pon0 PM660 || decode $TMP.pon2 PM660L
    done
    page 0 4200 256 > $TMP.bcl 2>/dev/null
    if [ "$(head -1 $TMP.bcl | cut -d' ' -f1)" = 4200 ]; then
        bcl_line $TMP.bcl
        echo "== PM660 FG BCL 0x4200: $BCL  (0x426a BCL_CFG=0x$(reg $TMP.bcl 426a) 0x42a3 LMH_CFG=0x$(reg $TMP.bcl 42a3))"
        SUM="$SUM $BCL"
    fi
    for m in /sys/module/a6l_bcl/parameters; do
        [ -d $m ] && echo "a6l_bcl: lowest_vbat_mv=$(cat $m/lowest_vbat_mv) highest_ibat_ma=$(cat $m/highest_ibat_ma) trips=$(cat $m/trips)"
    done
    echo "== power supplies"; ps_line
    echo "== thermal zones (a hot SoC right after a reset matters for boot loops)"
    for z in /sys/class/thermal/thermal_zone*; do echo "$(cat $z/type 2>/dev/null)=$(cat $z/temp 2>/dev/null)"; done | tr '\n' ' '; echo
    echo "== raw: PM660 PON page"; rawfmt < $TMP.pon0 2>/dev/null
    echo "== raw: PM660L PON page"; rawfmt < $TMP.pon2 2>/dev/null
    echo "== raw: PM660 BCL page 0x4200"; rawfmt < $TMP.bcl 2>/dev/null
    # SMPS S1 (APC0 silver), S2+S3 (APC1 gold), S5 of PM660 and the PM660/PM660L temp alarms: first 0x20 registers
    for x in 0:1400 0:1700 0:1a00 0:2000 0:2400 2:2400; do
        echo "== raw: SID${x%%:*} 0x${x#*:}"; page ${x%%:*} ${x#*:} 32 2>/dev/null | rawfmt
    done
}

rawfmt() { awk '{ printf "%s:%s ", $1, $2; if (NR % 16 == 0) print "" } END { if (NR % 16) print "" }'; }
kmsg() { echo "$*" > /dev/kmsg 2>/dev/null; }

case "${1:-now}" in
now)
    capture
    echo "SUMMARY:$SUM" ;;
kmsg)
    capture > $TMP.k 2>&1
    kmsg "A6L_PON$SUM"
    dbg_off ;;
boot)
    mkdir -p $OUT || exit 1
    n=$(cat $OUT/bootcount 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0;; esac
    n=$((n + 1)); echo $n > $OUT/bootcount
    F=$OUT/boot-$(printf %04d $n).txt
    capture > $F 2>&1
    echo "SUMMARY:$SUM" >> $F
    sync
    kmsg "A6L_PON boot=$n$SUM (full: $F)"
    total=$(ls $OUT/boot-*.txt 2>/dev/null | wc -l)
    [ $total -gt $KEEP ] && rm -f $(ls $OUT/boot-*.txt | sort | head -n $((total - KEEP)))
    dbg_off ;;
watch)
    IV=${2:-1}; MIN=${3:-0}
    mkdir -p $OUT || exit 1
    n=$(cat $OUT/bootcount 2>/dev/null); case "$n" in ''|*[!0-9]*) n=0;; esac
    F=$OUT/watch-$(printf %04d $n).csv
    B=/sys/class/power_supply/qcom-battery
    G=/sys/devices/system/cpu/cpufreq/policy1/scaling_cur_freq; S=/sys/devices/system/cpu/cpufreq/policy0/scaling_cur_freq
    SK=$(ls /sys/bus/iio/devices/iio:device*/in_temp_quiet_therm_input 2>/dev/null | head -1)
    dbg_on || true
    [ -s $F ] || echo "time,uptime_s,vbat_uV,ibat_uA,bcl_vmin_raw,bcl_imax_raw,gold_khz,silver_khz,skin_mC,usb_online,a6l_bcl_trips" > $F
    end=0; [ $MIN -gt 0 ] && end=$(( $(cut -d. -f1 /proc/uptime) + MIN * 60 ))
    pv=0; pi=0; pb=; pg=; last=0
    kmsg "A6L_PON watch start -> $F (every ${IV}s)"
    while :; do
        up=$(cut -d' ' -f1 /proc/uptime); us=${up%.*}
        [ $end -gt 0 ] && [ $us -ge $end ] && break
        v=$(cat $B/voltage_now 2>/dev/null); i=$(cat $B/current_now 2>/dev/null)
        bl=$(page 0 425c 2 2>/dev/null | cut -d' ' -f2 | tr '\n' ',')
        g=$(cat $G 2>/dev/null); s=$(cat $S 2>/dev/null)
        dv=$(( ${v:-0} - pv )); di=$(( ${i:-0} - pi ))
        if [ ${dv#-} -ge 20000 ] || [ ${di#-} -ge 200000 ] || [ "$bl" != "$pb" ] || [ "$g" != "$pg" ] || [ $((us - last)) -ge 30 ]; then
            echo "$(date +%T),$up,$v,$i,${bl%,},$g,$s,$(cat $SK 2>/dev/null),$(cat /sys/class/power_supply/*usb*/online 2>/dev/null | head -1),$(cat /sys/module/a6l_bcl/parameters/trips 2>/dev/null)" >> $F
            sync -d $F 2>/dev/null || sync
            pv=${v:-0}; pi=${i:-0}; pb=$bl; pg=$g; last=$us
        fi
        [ $(stat -c %s $F 2>/dev/null || echo 0) -gt 4194304 ] && { mv -f $F $F.old; echo "rotated" > $F; }
        sleep $IV
    done ;;
*)
    echo "usage: $0 now|kmsg|boot|watch [interval_s] [minutes]"; exit 1 ;;
esac
