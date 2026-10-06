#!/system/bin/sh
# run-sd.sh (flash/microSD agent, 28 Sep 2026) - ATTENDED microSD slot test, V75-usb recovery (RAM only). READ-ONLY.
#   MODE=check  : read-only: kernel, checksums, sdhc_2 DT status, mmc hosts (eMMC = c0c4000), sdhci_msm, L2/L5, gpio54.
#   MODE=load   : insmod a6l_sd_ovl.ko cd=$CD (0 = gpio54 active-high = stock, 1 = active-low, 2 = broken-cd polling),
#                 wait for the c084000 mmc host -> A6L_SD_LOAD_PASS / A6L_SD_LOAD_FAIL, card presence, cd gpio state.
#   MODE=detect : DETECT_S (default 30) s loop printing A6L_SD_EVENT insert/remove (Pierre inserts/removes the card).
#   MODE=read   : card CID/CSD/size/partitions, blockdev --setro, dd 16 MiB -> /dev/null (MB/s), optional
#                 `mount -o ro` of the first partition to list its root (MOUNT=0 to skip). Never writes to the card.
# Guard: only a block device whose host is c084000.mmc AND whose card type is SD is used; the eMMC (c0c4000,
# mmcblk1 on this kernel) or anything else is refused (A6L_SD_REFUSE).
# usage: adb -s HLTE730T-PROBE shell 'export PATH=/tmp/bin:$PATH TMPDIR=/tmp; D=/tmp/microsd MODE=load sh /tmp/microsd/run-sd.sh'
export PATH=/tmp/bin:$PATH TMPDIR=/tmp
D=${D:-/tmp/microsd}; MODE=${MODE:-check}
SYS=${A6L_SYS:-/sys}; DT=${A6L_DT:-/proc/device-tree}; NODES=${A6L_NODES:-/dev/a6l-sdnodes}   # A6L_*: mock tests only. 29 Sep: /tmp is nodev in the recovery (dd EACCES) -> nodes under /dev
SDHOST=c084000
say() { echo "A6L_SD_$*"; }
isnum() { case "$1" in ''|*[!0-9]*) return 1;; esac; return 0; }
[ -n "$A6L_MOCK" ] || mount -t debugfs none /sys/kernel/debug 2>/dev/null
kmsg() { [ -n "$A6L_MOCK" ] || dmesg | grep -iE "A6L_SD_OVL|c084000|mmc2|sdhci|pm660l_l[25]|regulators-a6lsd|rpm" | tail -${1:-20}; }
lpath() { readlink -f "$1" 2>/dev/null || readlink "$1"; }
sd_host() {  # the mmc_host of c084000 (never the eMMC c0c4000)
  for h in $SYS/class/mmc_host/mmc*; do
    [ -e "$h" ] || continue
    case "$(lpath $h)" in *$SDHOST*) echo "$h"; return;; esac
  done
}
sd_card() { H=$(sd_host); [ -n "$H" ] || return; for c in $H/${H##*/}:*; do [ -d "$c" ] && { echo "$c"; return; }; done; }
cd_line() { grep -E "gpio-?54[ :]|[^0-9]54 .*\||\| *cd " $SYS/kernel/debug/gpio 2>/dev/null | head -1 | tr -s ' '; }
regs() { for r in $SYS/class/regulator/regulator.*; do n=$(cat $r/name 2>/dev/null); case "$n" in pm660l_l2|pm660l_l5) echo "$n state=$(cat $r/state 2>/dev/null) uV=$(cat $r/microvolts 2>/dev/null) users=$(cat $r/num_users 2>/dev/null)";; esac; done; }
is_sd_disk() {  # $1 = disk name (mmcblkN): refuse unless host c084000 and card type SD
  case "$1" in mmcblk[0-9]|mmcblk[0-9][0-9]) ;; *) say "REFUSE $1: not a whole mmcblk disk"; return 1;; esac
  dv=$(lpath $SYS/block/$1/device)
  case "$dv" in *$SDHOST*) ;; *) say "REFUSE $1: host is not $SDHOST ($dv) - eMMC or unknown"; return 1;; esac
  case "$dv" in *c0c4000*) say "REFUSE $1: eMMC host c0c4000"; return 1;; esac
  t=$(cat $SYS/block/$1/device/type 2>/dev/null)
  [ "$t" = SD ] || { say "REFUSE $1: card type '$t' is not SD"; return 1; }
  return 0
}
node() {  # block node for $1 (disk or partition)
  [ -n "$A6L_MOCK" ] && [ -f $NODES/$1 ] && { echo $NODES/$1; return; }
  for n in /dev/block/$1 /dev/$1; do [ -b "$n" ] && { echo "$n"; return; }; done
  s=$SYS/class/block/$1/dev; [ -f $s ] || s=$SYS/block/$1/dev
  mm=$(cat $s 2>/dev/null); [ -n "$mm" ] || return 1
  mkdir -p $NODES; [ -e $NODES/$1 ] || mknod $NODES/$1 b ${mm%%:*} ${mm##*:} || return 1
  echo $NODES/$1
}
card_info() {
  C=$1
  say "CARD dir=$C type=$(cat $C/type 2>/dev/null) name=$(cat $C/name 2>/dev/null) manfid=$(cat $C/manfid 2>/dev/null) oemid=$(cat $C/oemid 2>/dev/null) date=$(cat $C/date 2>/dev/null) serial=$(cat $C/serial 2>/dev/null)"
  say "CARD cid=$(cat $C/cid 2>/dev/null) csd=$(cat $C/csd 2>/dev/null) scr=$(cat $C/scr 2>/dev/null)"
  say "CARD ios: $(h=${C%/*}; cat $SYS/kernel/debug/${h##*/}/ios 2>/dev/null | tr '\n' ';' | tr -s ' ')"
}

case "$MODE" in
check)
  say "CHECK kernel=$(uname -r) D=$D"
  [ -f $D/SHA256SUMS ] && (cd $D && sha256sum -c SHA256SUMS) 2>&1 | sed 's/^/A6L_SD_SUM /'
  say "CHECK dt sdhc_2 mmc@c084000 status=$(cat $DT/soc@0/mmc@c084000/status 2>/dev/null | tr -d '\0') cd-gpios=$([ -e $DT/soc@0/mmc@c084000/cd-gpios ] && echo yes || echo no)"
  say "CHECK modules: $(grep -E '^(a6l_sd_ovl|sdhci_msm) ' /proc/modules 2>/dev/null | cut -d' ' -f1 | tr '\n' ' ')"
  for h in $SYS/class/mmc_host/mmc*; do [ -e "$h" ] && say "CHECK host ${h##*/} -> $(lpath $h)"; done
  for b in $SYS/block/mmcblk*; do [ -e "$b" ] && say "CHECK block ${b##*/} type=$(cat $b/device/type 2>/dev/null) dev=$(lpath $b/device)"; done
  regs | sed 's/^/A6L_SD_REG /'
  say "CHECK cd gpio54: $(cd_line)"
  H=$(sd_host); say "CHECK sd host=${H:-none} card=$(sd_card)"
  say CHECK_DONE
  ;;
load)
  CD=${CD:-0}; case "$CD" in 0|1|2) ;; *) say "LOAD_FAIL CD=$CD (0|1|2)"; exit 1;; esac
  if [ -z "$A6L_MOCK" ]; then
    grep -q '^sdhci_msm ' /proc/modules || [ -d /sys/bus/platform/drivers/sdhci_msm ] || say "LOAD_WARN sdhci_msm not loaded (eMMC driver) - sdhc_2 cannot probe"
    grep -q '^a6l_sd_ovl ' /proc/modules || insmod $D/a6l_sd_ovl.ko cd=$CD || { kmsg; say "LOAD_FAIL insmod a6l_sd_ovl.ko"; exit 1; }
  fi
  H=; i=0; while [ $i -lt 10 ]; do H=$(sd_host); [ -n "$H" ] && break; sleep 1; i=$((i+1)); done
  kmsg 25
  regs | sed 's/^/A6L_SD_REG /'
  say "LOAD dt status=$(cat $DT/soc@0/mmc@c084000/status 2>/dev/null | tr -d '\0') cd gpio54: $(cd_line)"
  if [ -z "$H" ]; then
    cat $SYS/kernel/debug/devices_deferred 2>/dev/null | sed 's/^/A6L_SD_DEFERRED /'
    say "LOAD_FAIL no mmc host for $SDHOST after 10 s"; exit 1
  fi
  sleep 2; C=$(sd_card)
  say "LOAD host=${H##*/} ($(lpath $H)) card=$([ -n "$C" ] && echo "PRESENT ${C##*/} type=$(cat $C/type 2>/dev/null)" || echo absent)"
  say LOAD_PASS
  ;;
detect)
  H=$(sd_host); [ -n "$H" ] || { say "DETECT_FAIL no $SDHOST host (run MODE=load first)"; exit 1; }
  N=${DETECT_S:-30}; isnum "$N" || N=30; ins=0; rem=0
  C=$(sd_card); prev=${C:+${C##*/}}; G=$(cd_line)
  say "DETECT start host=${H##*/} card=${prev:-absent} gpio='$G' - Pierre: insert / remove the card now ($N s)"
  t=0; while [ $t -lt $N ]; do
    sleep 1; t=$((t+1))
    C=$(sd_card); cur=${C:+${C##*/}}; G2=$(cd_line)
    if [ "$cur" != "$prev" ]; then
      if [ -n "$cur" ]; then ins=$((ins+1)); say "EVENT t=${t}s INSERT $cur type=$(cat $C/type 2>/dev/null) name=$(cat $C/name 2>/dev/null)"
      else rem=$((rem+1)); say "EVENT t=${t}s REMOVE $prev"; fi
      prev=$cur
    fi
    [ "$G2" != "$G" ] && { say "EVENT t=${t}s GPIO '$G2'"; G=$G2; }
    [ $((t % 5)) = 0 ] && say "DETECT t=${t}s card=${cur:-absent}"
  done
  kmsg 10
  say "DETECT_DONE inserts=$ins removes=$rem (pass = events follow Pierre's actions)"
  ;;
read)
  C=$(sd_card); [ -n "$C" ] || { say "READ_FAIL no card on $SDHOST"; exit 1; }
  card_info $C
  B=; for b in $C/block/mmcblk*; do [ -d "$b" ] && B=${b##*/}; done
  [ -n "$B" ] || { say "READ_FAIL no block device under $C"; exit 1; }
  is_sd_disk $B || exit 1
  SZ=$(cat $SYS/block/$B/size 2>/dev/null); isnum "$SZ" || SZ=0
  say "READ disk=$B size=$((SZ / 2048)) MiB ($SZ sectors) ro=$(cat $SYS/block/$B/ro 2>/dev/null)"
  P1=
  for p in $SYS/block/$B/${B}p*; do [ -d "$p" ] || continue; pn=${p##*/}; [ -n "$P1" ] || P1=$pn
    say "PART $pn start=$(cat $p/start 2>/dev/null) size=$(( $(cat $p/size 2>/dev/null || echo 0) / 2048 )) MiB"; done
  N=$(node $B) || { say "READ_FAIL no block node for $B"; exit 1; }
  blockdev --setro $N 2>/dev/null; say "READ node=$N getro=$(blockdev --getro $N 2>/dev/null)"
  command -v blkid >/dev/null && blkid 2>/dev/null | grep "$B" | sed 's/^/A6L_SD_BLKID /'
  t0=$(date +%s%N); dd if=$N of=/dev/null bs=1048576 count=16 2>/tmp/sd-dd.txt; rc=$?; t1=$(date +%s%N)
  sed 's/^/A6L_SD_DD /' /tmp/sd-dd.txt
  if isnum "$t0" && isnum "$t1" && [ $((t1 - t0)) -gt 0 ]; then
    ms=$(( (t1 - t0) / 1000000 )); [ $ms -lt 1 ] && ms=1
    say "READ dd rc=$rc 16 MiB in ${ms} ms = $((16000 / ms)).$(( (160000 / ms) % 10 )) MB/s"
  else say "READ dd rc=$rc (no ns clock; see A6L_SD_DD line for the rate)"; fi
  [ $rc = 0 ] || { say "READ_FAIL dd"; exit 1; }
  if [ "${MOUNT:-1}" = 1 ]; then
    M=${P1:-$B}; PN=$(node $M) && blockdev --setro $PN 2>/dev/null
    mkdir -p /tmp/sdro
    if [ -n "$PN" ] && { mount -t vfat -o ro $PN /tmp/sdro 2>/dev/null || mount -t exfat -o ro $PN /tmp/sdro 2>/dev/null; }; then
      say "MOUNT_RO $M on /tmp/sdro: $(grep /tmp/sdro /proc/mounts | cut -d' ' -f3,4)"
      ls -la /tmp/sdro 2>&1 | head -30 | sed 's/^/A6L_SD_LS /'
      umount /tmp/sdro && say "UMOUNT ok"
    else say "MOUNT_RO skipped/failed for $M (not vfat/exfat?) - not an error"; fi
  fi
  say READ_PASS
  ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
