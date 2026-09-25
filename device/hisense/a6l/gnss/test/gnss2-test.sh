#!/system/bin/sh
# A6L GNSS assisted test (misc agent, 24 Sep 2026) for the V74 recovery RAM session. Modem must be up (v74/radio2,
# A6L_KEEP=1). GNSS is receive-only; XTRA/time/position injection only feeds the modem's GNSS engine.
# Set the clock first (recovery has no NTP):  date -u MMDDhhmmYYYY.ss   (from the laptop: date -u +%m%d%H%M%Y.%S)
# usage: sh /tmp/gnss2/gnss2-test.sh <step> [LAT,LON[,ACC_M]] [seconds]
#   query                         ask the modem which XTRA servers/sizes it wants + current XTRA validity (20 s)
#   xtra  [LAT,LON,ACC] [600]     time + coarse position + XTRA (xtra3grc.bin, falls back to xtra2.bin), then fix
#                                 attempt with quiet output; NMEA in /tmp/gnss2-logs/*.nmea
#   plain [600]                   same session without XTRA (A/B comparison, time only)
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/gnss2}; T=$D/a6l_gnss_test; L=/tmp/gnss2-logs; mkdir -p $L
[ -x $T ] || chmod 755 $T
ts=$(date +%H%M%S 2>/dev/null || echo now)
step=$1; shift 2>/dev/null
POS=""; case "$1" in *,*) POS="--inject-pos $1"; shift;; esac
S=${1:-600}
echo "A6L_GNSS2 clock: $(date -u 2>/dev/null)"
case "$step" in
query)
    $T --xtra-query --seconds 20 2>&1 | tee $L/query-$ts.txt ;;
xtra)
    for f in xtra3grc.bin xtra2.bin; do
        [ -f $D/$f ] || continue
        echo "A6L_GNSS2 using $f"
        $T --inject-time $POS --xtra $D/$f --xtra-required --seconds $S --record $L/xtra-$ts.pkt --nmea-only-log $L/xtra-$ts.nmea 2>&1 | tee $L/xtra-$ts.txt
        grep -q "XTRA_OK" $L/xtra-$ts.txt && break
        echo "A6L_GNSS2 $f not accepted, trying the next file"; ts=$ts-b
    done ;;
plain)
    $T --inject-time $POS --seconds $S --nmea-only-log $L/plain-$ts.nmea 2>&1 | tee $L/plain-$ts.txt ;;
*)
    sed -n '2,12p' "$0"; exit 1 ;;
esac
echo "A6L_GNSS2_DONE $step (logs $L)"
