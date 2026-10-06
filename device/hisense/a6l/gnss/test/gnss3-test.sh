#!/system/bin/sh
# A6L GNSS assisted test v3 (agent misc2, 25 Sep 2026) for the V74 recovery RAM session. Modem must be up (v74/radio2,
# A6L_KEEP=1). GNSS is receive-only; XTRA/time/position injection only feeds the modem's GNSS engine.
# Changes vs gnss2: injects the file the MODEM asks for (xtra3grcej.bin on this modem; picked from the query's
# server URL), keeps formatType like stock, retries QMI_ERR_INTERNAL (3) twice, prints QMI error names, uses the
# modem's max part size. Set the clock first (recovery has no NTP): date -u MMDDhhmmYYYY.ss
# usage: sh /tmp/gnss3/gnss3-test.sh <step> [LAT,LON[,ACC_M]] [seconds]
#   query                         which XTRA servers/sizes the modem wants + current validity (20 s)
#   xtra  [LAT,LON,ACC] [600]     time + coarse position + XTRA (modem's file first), then a fix attempt (quiet)
#   inject-only [LAT,LON,ACC]     time + position + XTRA only, then validity (no fix attempt; indoors is fine)
#   plain [600]                   same session without XTRA (A/B comparison, time only)
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/gnss3}; T=$D/a6l_gnss_test; L=/tmp/gnss3-logs; mkdir -p $L
[ -x $T ] || chmod 755 $T
ts=$(date +%H%M%S 2>/dev/null || echo now)
step=$1; shift 2>/dev/null
POS=""; case "$1" in *,*) POS="--inject-pos $1"; shift;; esac
S=${1:-600}
echo "A6L_GNSS3 clock: $(date -u 2>/dev/null)"
case "$(date -u +%Y 2>/dev/null)" in 202[6-9]) ;; *) echo "A6L_GNSS3 WARNING clock not set: run date -u MMDDhhmmYYYY.ss first (time injection skipped otherwise)";; esac
files() {  # modem's wish first, then the known-good fallbacks
    want=$($T --xtra-query --seconds 12 2>&1 | tee $L/query-$ts.txt | sed -n 's#.*server=https\{0,1\}://[^ ]*/\([^/ ]*\.bin\).*#\1#p' | head -n 1)
    echo "A6L_GNSS3 modem asks for: ${want:-?}" >&2
    for f in $want xtra3grcej.bin xtra3grc.bin xtra2.bin; do [ -f $D/$f ] && echo $f; done | awk '!s[$0]++'
}
inject() {  # $1 = extra args (--seconds N or --xtra-only)
    for f in $(files); do
        echo "A6L_GNSS3 using $f ($(wc -c < $D/$f) bytes)"
        $T --inject-time $POS --xtra $D/$f --xtra-required $1 --record $L/xtra-$ts.pkt --nmea-only-log $L/xtra-$ts.nmea 2>&1 | tee $L/xtra-$ts.txt
        grep -q "XTRA_OK" $L/xtra-$ts.txt && return 0
        echo "A6L_GNSS3 $f not accepted, trying the next file"; ts=$ts-b
    done
    return 1
}
case "$step" in
query)
    $T --xtra-query --seconds 20 2>&1 | tee $L/query-$ts.txt ;;
xtra)
    inject "--seconds $S" ;;
inject-only)
    inject "--seconds 25" ;;
plain)
    $T --inject-time $POS --seconds $S --nmea-only-log $L/plain-$ts.nmea 2>&1 | tee $L/plain-$ts.txt ;;
*)
    sed -n '2,13p' "$0"; exit 1 ;;
esac
echo "A6L_GNSS3_DONE $step (logs $L; send $L/*.txt and *.pkt)"
