#!/bin/bash
# volte6 (29 Sep 2026): offline mock of volte6-test.sh, run by the REAL V74 recovery mksh + toybox (qemu-aarch64),
# like tools/volte5/mock-test-volte5.sh. Fakes: a6l-imsdcm (replays the 29 Sep 770 sequence, then the volte6 kick),
# a6l-qmi, volte-probe (host shell scripts), ipa sysfs node, a fake /proc with rmtfs in RAM mode.
# Also Astra's stale-registration case (research/fresh-eye-20260928/work/stale-registration-test.sh): a lost
# registration / withdrawn IMSA must NOT be reported as registered.
# usage (WSL): mock-test-volte6.sh <volte6-test.sh> <recovery_root>
set -u
S=$1; RD=$2; M=/tmp/volte6-mock; export QEMU_LD_PREFIX=$RD
SH="$RD/system/bin/sh"; TB=$RD/system/bin/toybox
pass=0; failn=0
ok() { echo "MOCK_OK   $*"; pass=$((pass+1)); }
ko() { echo "MOCK_FAIL $*"; failn=$((failn+1)); }
mkdir -p /tmp/bin; for a in $(qemu-aarch64 $TB 2>/dev/null); do ln -sf $TB /tmp/bin/$a; done
setup() {
  pkill -f "$M/bin/a6l-imsdcm" 2>/dev/null; rm -rf $M; mkdir -p $M/bin $M/b $M/logs $M/proc/77; touch $M/ipa
  printf '/tmp/radio2/bin/rmtfs\0-o\0/tmp/rmtfs\0-v\0' > $M/proc/77/cmdline
  cp $S $M/b/volte6-test.sh; (cd $M/b && sha256sum volte6-test.sh > SHA256SUMS)
  mkdir -p $M/b/pdc; echo 'echo "A6L_VOLTE4_ACTIVATE_PASS active=France-Commercial-Orange id=965d restart=no D=$D VOLTE2=$VOLTE2 MODE=$MODE restart_req=${MANUAL_RESTART:-0}"' > $M/b/pdc/volte4-test.sh
  echo France-Commercial-Orange > $M/active
  cat > $M/bin/volte-probe <<'X'
#!/bin/sh
M=/tmp/volte6-mock
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
M=/tmp/volte6-mock; echo "imsdcm $* IMSS=$A6L_IMSDCM_IMSS KICK=$A6L_IMSDCM_KICK VDP=$A6L_IMSDCM_VDP MUX=$A6L_IMSDCM_MUX" >> $M/calls.log
if [ "$1" = --status ]; then
  echo "A6L_IMSDCM_IMSS bind sub=0: ok"; echo "A6L_IMSDCM_IMSS get ims_service_enabled=1 volte=1 vt=0 wifi=0 settings_resp=0"
  echo "A6L_IMSDCM_NAS get vdp=2(cs-pref) usage=1(voice) vops=- lte_voice_domain=- lte_srv=-"
  [ "$A6L_IMSDCM_VDP" = set ] && { echo "A6L_IMSDCM_NAS set voice_domain_pref=3(ims-pref): ok"; echo "A6L_IMSDCM_NAS get-after vdp=3(ims-pref) usage=1(voice) vops=- lte_voice_domain=- lte_srv=-"; }
  echo "A6L_IMSDCM_IMSA bind sub=0: ok"; echo "A6L_IMSDCM_STATUS imsa=present registered=0 voice=0"; exit 0; fi
mkdir -p $M/proc/$$; printf '/tmp/volte6/a6l-imsdcm\0--duration\03600\0--imsa\0' > $M/proc/$$/cmdline
echo "A6L_IMSDCM_PUBLISHED service=770 idl=1.15 instance_word=0x1 mux=$A6L_IMSDCM_MUX profile=modem"
echo "A6L_IMSDCM_REQ LINK_ADDR port=9099 family=v6 addr=fe80::64c1:f4b5:70f5:aae6 -> ok"
echo "A6L_IMSDCM_REQ from=0:114 0x002e REGISTER_APP_STATE app=1 instance=0 -> ok (stock behaviour) type=0 txn=2 msg=0x002e [10]01 [11]00000000"
echo "A6L_IMSDCM_REQ from=0:114 0x0034 SERVICE_ENABLE_STATUS rcs_mask=0x0 -> ok (stock behaviour) type=0 txn=3 msg=0x0034 [01]0000000000000000"
echo "A6L_IMSDCM_IMSS PRESENT after 0 s (mode=$A6L_IMSDCM_IMSS)"
echo "A6L_IMSDCM_IMSS get ims_service_enabled=1 volte=1 vt=0 wifi=0"
echo "A6L_IMSDCM_IMSS enable: already on (no write)"
echo "A6L_IMSDCM_IMSS RESULT ims_service_enabled=1 wrote=no"
echo "A6L_IMSDCM_NAS get vdp=3(ims-pref) usage=1(voice) vops=0 lte_voice_domain=3(cs) lte_srv=2"
echo "A6L_IMSDCM_IMSA PRESENT (service 33 published) after 0 s"
echo "A6L_IMSDCM_IMSA bind sub=0: ok"
echo "A6L_IMSDCM_IMSA reg status=not-registered err=0"
echo "A6L_IMSDCM_IMSA STATE registered=0 src=query"
sleep 0.3
echo "A6L_IMSDCM_REQ from=0:114 0x0033 SUB_DESTROY_INSTANCE instance=1 -> ok (stock behaviour) type=0 txn=4 msg=0x0033 [01]01000000"
echo "A6L_IMSDCM_EVENT destroy instance=1 (GLOBAL)"
echo "A6L_IMSDCM_KICK n=1 why=lte-full-service+destroy-instance-1 mode=$A6L_IMSDCM_KICK t=3s"
echo "A6L_IMSDCM_IMSS set volte=1 ims_service_enabled=1: ok"
echo "A6L_IMSDCM_IMSS RESULT ims_service_enabled=1 wrote=ok (kick)"
if [ -f $M/stale ]; then
  echo "A6L_IMSDCM_IMSA REGISTERED (query)"; echo "A6L_IMSDCM_IMSA_IND reg status=not-registered"; echo "A6L_IMSDCM_IMSA GONE (service 33 withdrawn)"
elif [ -f $M/noreg ]; then
  echo "A6L_IMSDCM_IMSA_POLL t=15s reg status=not-registered err=0 | services voice=- sms=- vt=- ut=available/wwan"
else
  echo "A6L_IMSDCM_REQ from=0:114 PDP_ACTIVATE apn='ims' type=ims rat=lte family=v6 profile=2 seq=1 sub=0 slot=0 inst=2"
  echo "A6L_IMSDCM_PDP id=1 apn='ims' family=v6 state=up addr=2a01:cb00::1 handle=0x1234 err=none | start(profile 2)=ok"
  echo "A6L_IMSDCM_IND PDP_ACTIVATE id=1 success addr=2a01:cb00::1"
  sleep 0.3
  echo "A6L_IMSDCM_IMSA_IND reg status=registered tech=wwan"
  echo "A6L_IMSDCM_IMSA REGISTERED (indication)"
  echo "A6L_IMSDCM_IMSA STATE registered=1 src=indication"
  echo "A6L_IMSDCM_NAS now t=20s vdp=3(ims-pref) usage=1(voice) vops=1 lte_voice_domain=1(ims) lte_srv=2"
  echo "A6L_IMSDCM_IMSA_IND services voice=available/wwan sms=available vt=- ut=-"
fi
trap 'echo "A6L_IMSDCM_SUMMARY pdps=1"; echo A6L_IMSDCM_DONE; rm -rf $M/proc/$$; exit 0' TERM
while :; do sleep 1; done
X
  cat > $M/bin/a6l-qmi <<'X'
#!/bin/sh
M=/tmp/volte6-mock; echo "qmi $*" >> $M/calls.log
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
  env -i PATH=/usr/bin:/bin HOME=/tmp D=$M/b L=$M/logs A6L_V6_BIN=$M/bin A6L_V6_IPA=$M/ipa A6L_V6_PROC=$M/proc A6L_DRYRUN_MSS=running A6L_T=0.3 \
    WATCH=20 WAIT_REG=10 QEMU_LD_PREFIX=$RD "$@" qemu-aarch64 $SH $M/b/volte6-test.sh > $M/out.txt 2>&1
  rc=$?; o=$(grep -v linker $M/out.txt)
  if [ $rc = $erc ] && echo "$o" | grep -q -- "$mk"; then ok "$* -> rc=$rc $mk"; else ko "$* -> rc=$rc (want $erc, $mk)"; echo "$o" | tail -n 20; fi
}
line() { grep -n "$1" $M/calls.log | head -n 1 | cut -d: -f1; }
setup
run 0 "A6L_VOLTE6_CHECK_PASS modem=running ipa=bound efs=ram active='France-Commercial-Orange'" MODE=check
grep -q "A6L_IMSDCM_NAS get vdp=2(cs-pref)" $M/out.txt && ok "check: NAS voice domain pref shown" || ko "check NAS"
grep -q "imsdcm --status IMSS=read KICK= VDP=read" $M/calls.log && ok "check: read-only (IMSS=read VDP=read)" || ko "check modes"
run 1 "needs A6L_RF_APPROVED=1" MODE=verify
rm $M/ipa; run 1 "ipa4 not bound" MODE=verify A6L_RF_APPROVED=1; touch $M/ipa
echo ROW_Commercial > $M/active; run 1 "not France-Commercial-Orange" MODE=verify A6L_RF_APPROVED=1; echo France-Commercial-Orange > $M/active
mv $M/proc/77 $M/p77; run 1 "rmtfs is not in RAM mode" MODE=verify A6L_RF_APPROVED=1; mv $M/p77 $M/proc/77
run 1 "ORDER=bogus" MODE=verify A6L_RF_APPROVED=1 ORDER=bogus
: > $M/calls.log
run 0 "A6L_VOLTE6_RESULT imss=1,wrote=ok imsa=present bound=yes dcm_requests=5 destroyed=1 kicks=1 pdp_activate=1 pdp_up=1 registered=yes voice=available vdp=3 vops=1 lte_voice_domain=1" MODE=verify A6L_RF_APPROVED=1 A6L_PIN=7391
grep -q "A6L_VOLTE6_REG registered=1" $M/out.txt && ok "verify: registration seen" || ko "verify reg"
grep -q "A6L_VOLTE6_GATE rmtfs RAM mode" $M/out.txt && ok "verify: RAM EFS gate passed" || ko "gate line"
grep -q "imsdcm --duration 3600 --imsa IMSS=enable KICK=force VDP=read MUX=9" $M/calls.log && ok "verify: daemon IMSS=enable KICK=force (defaults)" || ko "verify daemon env"
[ "$(line 'qmi pin')" -lt "$(line 'imsdcm --status')" ] && [ "$(line 'imsdcm --status')" -lt "$(line 'qmi mode online')" ] && \
  [ "$(line 'qmi mode online')" -lt "$(line 'imsdcm --duration')" ] && ok "verify order: PIN -> NAS/IMSS pre-check -> online -> 770 (sim-first)" || ko "verify order"
grep -q "dcm| A6L_IMSDCM_EVENT destroy instance=1 (GLOBAL)" $M/out.txt && ok "destroy event shown" || ko "destroy event"
grep -q "\[01\]0000000000000000" $M/out.txt && ok "QMI hex never masked" || ko "QMI hex masked"
grep -q "imsi=2080\*\*\*\*\*90" $M/out.txt && ! grep -q 208011234567890 $M/out.txt && ok "IMSI masked" || ko "IMSI mask"
grep -q "7391" $M/out.txt $M/logs/*.txt && ko "PIN printed" || ok "PIN never printed"
[ -n "$(pgrep -f "$M/bin/a6l-imsdcm")" ] && ok "daemon keeps running after verify" || ko "daemon gone"
# FRESH=1: a second verify restarts the daemon (fresh 770 publish), the old log is kept
: > $M/calls.log
run 0 "restarting a6l-imsdcm (FRESH=1" MODE=verify A6L_RF_APPROVED=1
[ "$(grep -c 'imsdcm --duration' $M/calls.log)" = 1 ] && ls $M/logs/imsdcm-prev-*.txt > /dev/null 2>&1 && ok "fresh restart + previous log kept" || ko "fresh restart"
# stale registration (Astra 28 Sep): REGISTERED, then not-registered, then GONE -> registered=no
touch $M/stale
run 0 "A6L_VOLTE6_REG registered=0" MODE=verify A6L_RF_APPROVED=1
grep -q "registered=no" $M/out.txt && ok "stale registration not reported (RESULT registered=no)" || ko "stale registration reported"
rm $M/stale
# daemon-first (volte5 order) + VDP=set before the attach + REATTACH when not registered
touch $M/noreg; : > $M/calls.log
run 0 "A6L_VOLTE6_RESULT" MODE=verify A6L_RF_APPROVED=1 A6L_PIN=7391 ORDER=daemon-first A6L_IMSDCM_VDP=set REATTACH=1 A6L_IMSDCM_KICK=toggle
[ "$(line 'imsdcm --duration')" -lt "$(line 'qmi mode online')" ] && ok "daemon-first order" || ko "daemon-first order"
grep -qE "imsdcm --status IMSS=read KICK=[a-z]* VDP=set" $M/calls.log && grep -q "A6L_IMSDCM_NAS set voice_domain_pref=3(ims-pref): ok" $M/out.txt && ok "VDP=set before online" || ko "VDP set"
grep -q "KICK=toggle" $M/calls.log && ok "KICK=toggle passed" || ko "kick toggle"
[ "$(grep -c 'qmi pin' $M/calls.log)" = 2 ] && grep -q "qmi mode lowpower" $M/calls.log && grep -q "5b REATTACH" $M/out.txt && ok "REATTACH: low power, PIN again, online" || ko "reattach"
grep -q "7391" $M/out.txt $M/logs/*.txt && ko "PIN printed (reattach)" || ok "PIN never printed (reattach)"
rm $M/noreg
run 0 "A6L_VOLTE6_RESULT" MODE=verify A6L_RF_APPROVED=1 A6L_IMSDCM_IMSS=read A6L_IMSDCM_KICK=off
grep -q "A6L_VOLTE6_GATE" $M/out.txt && ko "read-only run should not need the gate" || ok "read-only run: no write gate"
run 1 "is running: the PDC gate refuses" MODE=load
run 0 "A6L_VOLTE6_CALL_RESULT domain=ims registered=1 srvcc=0 audio=off" MODE=call A6L_RF_APPROVED=1 NUM=0600000000 A6L_DIAL_TO=0600000000
run 1 "emergency number refused" MODE=call A6L_RF_APPROVED=1 NUM=112 A6L_DIAL_TO=112
run 1 "set NUM=<own number>" MODE=call A6L_RF_APPROVED=1 NUM=0600000000 A6L_DIAL_TO=0611111111
run 0 "A6L_VOLTE6_RESULT" MODE=status
run 0 "A6L_VOLTE6_DONE mode=stop result=OK" MODE=stop
run 0 "A6L_VOLTE6_PDC_activate_OK ACTIVATE_PASS active=France-Commercial-Orange id=965d restart=no D=$M/b/pdc VOLTE2=$M/b MODE=activate" MODE=activate
grep -q "restart_req=0" $M/out.txt && ok "activate: no restart by default" || ko "activate default restart"
run 0 "A6L_VOLTE6_PDC_activate_OK ACTIVATE_PASS" MODE=activate RESTART=1
grep -q "restart_req=1" $M/out.txt && grep -q "A6L_VOLTE6_NOTE RESTART=1" $M/out.txt && ok "activate RESTART=1 -> volte4 MANUAL_RESTART=1" || ko "RESTART=1 not passed"
mv $M/proc/77 $M/p77; run 1 "RESTART=1 needs rmtfs in RAM mode" MODE=activate RESTART=1; mv $M/p77 $M/proc/77
sleep 1; [ -z "$(pgrep -f "$M/bin/a6l-imsdcm")" ] && ok "stop killed a6l-imsdcm" || ko "daemon still running"
grep -q "qmi mode lowpower" $M/calls.log && ok "stop -> low power" || ko "stop lowpower"
printf 'X' >> $M/b/volte6-test.sh; run 1 "bundle hash: mismatch volte6-test.sh" MODE=check
echo "MOCK_SUMMARY pass=$pass fail=$failn"
[ $failn = 0 ]
