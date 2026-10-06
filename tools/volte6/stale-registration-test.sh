#!/bin/sh
# volte6 (29 Sep 2026): Astra's stale-registration reproducer (research/fresh-eye-20260928/work/stale-registration-test.sh)
# run against the reg_now/wait_reg functions of volte6-test.sh (extracted, not copied). A registration that was lost,
# or an IMSA service that was withdrawn, must NOT satisfy wait_reg; a current registration must.
# usage: stale-registration-test.sh <volte6-test.sh>
S=$1; T=${TMPDIR:-/tmp}/v6-stale.$$; mkdir -p $T
sed -n '/^reg_now() {/,/^}/p; /^wait_reg() {/,/^}/p' "$S" > $T/fn.sh
grep -q "^reg_now()" $T/fn.sh && grep -q "^wait_reg()" $T/fn.sh || { echo "STALE_FAIL functions not found in $S"; exit 1; }
DLOG=$T/dlog; LOG=$T/log
log() { echo "$*"; }; mask() { cat; }; nap() { :; }
. $T/fn.sh
pass=0; fail=0
case_() { # case_ <name> <want 0|1> <lines...>
  cname=$1; want=$2; shift 2; : > $DLOG; for l in "$@"; do echo "$l" >> $DLOG; done
  SEEN=0; wait_reg 0 > /dev/null
  if [ "$REGD" = "$want" ]; then echo "STALE_OK   $cname -> $REGD"; pass=$((pass+1)); else echo "STALE_FAIL $cname -> $REGD (want $want)"; fail=$((fail+1)); fi
}
case_ "astra: registered, then not-registered, then GONE" 0 \
  "A6L_IMSDCM_IMSA REGISTERED (query)" "A6L_IMSDCM_IMSA_IND reg status=not-registered" "A6L_IMSDCM_IMSA GONE (service 33 withdrawn)"
case_ "registered then indication not-registered" 0 \
  "A6L_IMSDCM_IMSA_IND reg status=registered tech=wwan" "A6L_IMSDCM_IMSA REGISTERED (indication)" "A6L_IMSDCM_IMSA STATE registered=1 src=indication" \
  "A6L_IMSDCM_IMSA_IND reg status=not-registered err=0" "A6L_IMSDCM_IMSA STATE registered=0 src=indication"
case_ "registered then poll not-registered" 0 \
  "A6L_IMSDCM_IMSA REGISTERED (poll)" "A6L_IMSDCM_IMSA_POLL t=30s reg status=not-registered err=0 | services voice=- sms=- vt=- ut=-"
case_ "registered (indication)" 1 \
  "A6L_IMSDCM_IMSA reg status=not-registered err=0" "A6L_IMSDCM_IMSA_IND reg status=registered tech=wwan" "A6L_IMSDCM_IMSA REGISTERED (indication)" \
  "A6L_IMSDCM_IMSA_IND services voice=available/wwan sms=available vt=- ut=-"
case_ "registered (STATE line last)" 1 "A6L_IMSDCM_IMSA STATE registered=1 src=poll"
case_ "registering is not registered" 0 "A6L_IMSDCM_IMSA_IND reg status=registering"
case_ "services line after not-registered does not count" 0 \
  "A6L_IMSDCM_IMSA_IND reg status=not-registered" "A6L_IMSDCM_IMSA_IND services voice=available/wwan"
case_ "empty log" 0
rm -rf $T
echo "STALE_SUMMARY pass=$pass fail=$fail"
[ $fail = 0 ]
