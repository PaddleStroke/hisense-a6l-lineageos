#!/bin/bash
# volte5 (28 Sep 2026): offline mock of volte5-test.sh, run by the REAL V74 recovery mksh + toybox (qemu-aarch64),
# like tools/volte4-pdc/mock-test.sh. Fakes: a6l-imsdcm, a6l-qmi, volte-probe (host shell scripts), ipa sysfs node.
# usage (WSL): mock-test-volte5.sh <volte5-test.sh> <recovery_root>
set -u
S=$1; RD=$2; M=/tmp/vo28-mock; export QEMU_LD_PREFIX=$RD
SH="$RD/system/bin/sh"; TB=$RD/system/bin/toybox
pass=0; failn=0
ok() { echo "MOCK_OK   $*"; pass=$((pass+1)); }
ko() { echo "MOCK_FAIL $*"; failn=$((failn+1)); }
mkdir -p /tmp/bin; for a in $(qemu-aarch64 $TB 2>/dev/null); do ln -sf $TB /tmp/bin/$a; done
setup() {
  pkill -f "$M/bin/a6l-imsdcm" 2>/dev/null; rm -rf $M; mkdir -p $M/bin $M/b $M/logs $M/proc; touch $M/ipa
  cp $S $M/b/volte5-test.sh; (cd $M/b && sha256sum volte5-test.sh > SHA256SUMS)
  mkdir -p $M/b/pdc; echo 'echo "A6L_VOLTE4_ACTIVATE_PASS active=France-Commercial-Orange id=965d restart=no D=$D VOLTE2=$VOLTE2 MODE=$MODE"' > $M/b/pdc/volte4-test.sh
  echo France-Commercial-Orange > $M/active
  cat > $M/bin/volte-probe <<'X'
#!/bin/sh
M=/tmp/vo28-mock
if [ "$(cat $M/active)" = France-Commercial-Orange ]; then id=965d8d766f9ac0e698ca5d4433395a31526d2d8d; else id=7af52eaad094b6617fd163ca7370863364601f22; fi
echo "A6L_VOLTE_PDC_ACTIVE type=SW id=$id"
echo "A6L_VOLTE_PDC_INFO type=SW id=965d8d766f9ac0e698ca5d4433395a31526d2d8d size=50348 version=0x08040b20 desc='France-Commercial-Orange'"
echo "A6L_VOLTE_PDC_INFO type=SW id=7af52eaad094b6617fd163ca7370863364601f22 size=45828 version=0x08030808 desc='ROW_Commercial'"
echo "A6L_VOLTE_IMS_SVC 33 IMSA(ims-application) PRESENT"
echo "A6L_VOLTE_NAS_SYSINFO lte_voice_domain=1 (0 none,1 IMS,2 1X,3 3GPP-CS)"
echo "A6L_VOLTE_NAS_SYSINFO tlv=0x19 len=29 010301030100010000ffff0109897201000000013230383031ff01a2cd"
X
  cat > $M/bin/a6l-imsdcm <<'X'
#!/bin/sh
M=/tmp/vo28-mock; echo "imsdcm $* IMSS=$A6L_IMSDCM_IMSS MUX=$A6L_IMSDCM_MUX" >> $M/calls.log
if [ "$1" = --status ]; then
  echo "A6L_IMSDCM_IMSS bind sub=0: ok"; echo "A6L_IMSDCM_IMSS get ims_service_enabled=0 volte=0 vt=0 wifi=0 settings_resp=0"
  echo "A6L_IMSDCM_IMSA bind sub=0: ok"; echo "A6L_IMSDCM_STATUS imsa=present registered=0 voice=0"; exit 0; fi
# qemu-user cannot list the host /proc: the fake daemon registers itself in the fake /proc (A6L_V5_PROC)
mkdir -p $M/proc/$$; printf '/tmp/volte5/a6l-imsdcm\0--duration\03600\0--imsa\0' > $M/proc/$$/cmdline
echo "A6L_IMSDCM_PUBLISHED service=770 idl=1.15 instance_word=0x1 mux=$A6L_IMSDCM_MUX profile=modem"
echo "A6L_IMSDCM_IMSS PRESENT after 0 s (mode=$A6L_IMSDCM_IMSS)"
echo "A6L_IMSDCM_IMSS bind sub=0: ok"
echo "A6L_IMSDCM_IMSS get ims_service_enabled=0 volte=0 vt=0 wifi=0 settings_resp=0"
echo "A6L_IMSDCM_IMSS set volte=1 ims_service_enabled=1: ok"
echo "A6L_IMSDCM_IMSS RESULT ims_service_enabled=1 wrote=ok"
echo "A6L_IMSDCM_IMSA PRESENT (service 33 published) after 0 s"
echo "A6L_IMSDCM_IMSA bind sub=0: ok"
echo "A6L_IMSDCM_IMSA reg status=not-registered"
sleep 0.3
echo "A6L_IMSDCM_REQ from=0:116 0x0034 SERVICE_ENABLE_STATUS rcs_mask=0x0 -> ok (stock behaviour) type=0 txn=3 msg=0x0034 [01]0000000000000000"
echo "A6L_IMSDCM_REQ from=0:116 PDP_ACTIVATE apn='ims' type=ims rat=lte family=v6 profile=6 seq=1 sub=0 slot=0 inst=0"
echo "A6L_IMSDCM_PDP id=1 apn='ims' family=v6 state=up addr=2a01:cb00::1 handle=0x1234 err=none | start(profile 6)=ok"
echo "A6L_IMSDCM_IND PDP_ACTIVATE id=1 success addr=2a01:cb00::1"
sleep 0.3
echo "A6L_IMSDCM_IMSA_IND reg status=registered tech=wwan"
echo "A6L_IMSDCM_IMSA REGISTERED (indication)"
echo "A6L_IMSDCM_IMSA_IND services voice=available/wwan sms=available vt=- ut=-"
trap 'echo "A6L_IMSDCM_SUMMARY pdps=1"; echo A6L_IMSDCM_DONE; rm -rf $M/proc/$$; exit 0' TERM
while :; do sleep 1; done
X
  cat > $M/bin/a6l-qmi <<'X'
#!/bin/sh
M=/tmp/vo28-mock; echo "qmi $*" >> $M/calls.log
case "$1" in
pin) echo "A6L_QMI_VERIFY_PIN ok retries_left=3 puk_left=10 (before: 3)";;
mode) echo "A6L_QMI_SET_OPMODE ok -> $2";;
reg) echo "A6L_QMI_SERVING reg=registered cs=1 ps=1 rat=lte roaming=no plmn=20801 'Orange F' imsi=208011234567890";;
ims-status) echo "A6L_VOLTE3_IMSA now imsa=present registered=1 voice=1 reg=[status=registered tech=wwan] services=[voice=available/wwan]"
  echo "A6L_VOLTE3_NAS now ok lte_ims_voice=1 lte_voice_domain=1(0 none,1 IMS,2 1X,3 CS)"
  echo "A6L_VOLTE3_AUDIO_SESSION lte=volte lte_vsid=0x10C02000";;
dial-ims) [ "$A6L_RF_APPROVED" = 1 ] && [ "$A6L_DIAL_TO" = "$2" ] || { echo "A6L_QMI_REFUSED"; exit 3; }
  echo "A6L_QMI_DIAL_IMS ok id=1"; echo "A6L_QMI_VOICE state=origination"; echo "A6L_QMI_CALL_DOMAIN ims"; sleep 1
  echo "A6L_VOLTE3_CALL_RESULT domain=ims srvcc=0";;
*) echo "unknown $*";;
esac
X
  chmod 755 $M/bin/*
}
run() { # run <expect_rc> <marker> env...
  erc=$1; mk=$2; shift 2
  env -i PATH=/usr/bin:/bin HOME=/tmp D=$M/b L=$M/logs A6L_V5_BIN=$M/bin A6L_V5_IPA=$M/ipa A6L_V5_PROC=$M/proc A6L_DRYRUN_MSS=running A6L_T=0.3 \
    WATCH=20 WAIT_REG=10 QEMU_LD_PREFIX=$RD "$@" qemu-aarch64 $SH $M/b/volte5-test.sh > $M/out.txt 2>&1
  rc=$?; o=$(grep -v linker $M/out.txt)
  if [ $rc = $erc ] && echo "$o" | grep -q -- "$mk"; then ok "$* -> rc=$rc $mk"; else ko "$* -> rc=$rc (want $erc, $mk)"; echo "$o" | tail -n 20; fi
}
setup
run 0 "A6L_VOLTE5_CHECK_PASS modem=running ipa=bound active='France-Commercial-Orange'" MODE=check
grep -q "A6L_IMSDCM_STATUS imsa=present" $M/out.txt && ok "check: a6l-imsdcm --status read" || ko "check status"
grep -q "imsdcm --status IMSS=read" $M/calls.log && ok "check: IMSS read-only" || ko "check IMSS mode"
run 1 "needs A6L_RF_APPROVED=1" MODE=verify
rm $M/ipa; run 1 "ipa4 not bound" MODE=verify A6L_RF_APPROVED=1; touch $M/ipa
echo ROW_Commercial > $M/active; run 1 "not France-Commercial-Orange" MODE=verify A6L_RF_APPROVED=1; echo France-Commercial-Orange > $M/active
: > $M/calls.log
run 0 "A6L_VOLTE5_RESULT imss=1,wrote=ok imsa=present bound=yes dcm_requests=2 pdp_activate=1 pdp_up=1 registered=yes voice=available lte_voice_domain=1" MODE=verify A6L_RF_APPROVED=1 A6L_PIN=7391
grep -q "A6L_VOLTE5_REG registered=1" $M/out.txt && ok "verify: registration seen" || ko "verify reg"
grep -q "imsdcm --duration 3600 --imsa IMSS=enable MUX=9" $M/calls.log && ok "verify: daemon IMSS=enable mux=9 (ipa bound)" || ko "verify daemon env"
[ "$(grep -n 'qmi pin' $M/calls.log | cut -d: -f1)" -lt "$(grep -n 'qmi mode online' $M/calls.log | cut -d: -f1)" ] && \
  [ "$(grep -n 'imsdcm --duration' $M/calls.log | cut -d: -f1)" -lt "$(grep -n 'qmi mode online' $M/calls.log | cut -d: -f1)" ] && ok "verify: PIN and 770/IMSS before online" || ko "verify order"
grep -q "\[01\]0000000000000000" $M/out.txt && ok "QMI hex never masked" || ko "QMI hex masked"
grep -q "len=29 010301030100010000ffff0109897201000000013230383031ff01a2cd" $M/logs/*.txt && ok "tlv hex kept in logs" || ko "tlv hex"
grep -q "imsi=2080\*\*\*\*\*90" $M/out.txt && ! grep -q 208011234567890 $M/out.txt && ok "IMSI masked" || ko "IMSI mask"
grep -q "7391" $M/out.txt $M/logs/*.txt && ko "PIN printed" || ok "PIN never printed"
[ -n "$(pgrep -f "$M/bin/a6l-imsdcm")" ] && ok "daemon keeps running after verify" || ko "daemon gone"
run 1 "is running: the PDC gate refuses" MODE=load
run 0 "A6L_VOLTE5_CALL_RESULT domain=ims registered=1 srvcc=0 audio=off" MODE=call A6L_RF_APPROVED=1 NUM=0600000000 A6L_DIAL_TO=0600000000
run 1 "emergency number refused" MODE=call A6L_RF_APPROVED=1 NUM=112 A6L_DIAL_TO=112
run 1 "set NUM=<own number>" MODE=call A6L_RF_APPROVED=1 NUM=0600000000 A6L_DIAL_TO=0611111111
run 0 "A6L_VOLTE5_RESULT" MODE=status
run 0 "A6L_VOLTE5_DONE mode=stop result=OK" MODE=stop
run 0 "A6L_VOLTE5_PDC_activate_OK ACTIVATE_PASS active=France-Commercial-Orange id=965d restart=no D=$M/b/pdc VOLTE2=$M/b MODE=activate" MODE=activate
sleep 1; [ -z "$(pgrep -f "$M/bin/a6l-imsdcm")" ] && ok "stop killed a6l-imsdcm" || ko "daemon still running"
grep -q "qmi mode lowpower" $M/calls.log && ok "stop -> low power" || ko "stop lowpower"
printf 'X' >> $M/b/volte5-test.sh; run 1 "bundle hash: mismatch volte5-test.sh" MODE=check
echo "MOCK_SUMMARY pass=$pass fail=$failn"
[ $failn = 0 ]
