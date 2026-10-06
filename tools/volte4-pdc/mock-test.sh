#!/bin/bash
# Offline mock test of volte4-test.sh, run by the REAL V74 recovery mksh + toybox 0.8.13 (from ramdisk.cpio.gz) under
# qemu-aarch64 (binfmt). Fake: qmicli, volte-probe, volte2-test.sh, dmesg, /proc entries, remoteproc, block uevents.
# usage (WSL): mock-test.sh <bundle_dir> <recovery_root>
set -u
B=$1; RD=$2; M=/tmp/vo5-mock; export QEMU_LD_PREFIX=$RD
SH="$RD/system/bin/sh"; TB=$RD/system/bin/toybox
pass=0; failn=0
ok() { echo "MOCK_OK   $*"; pass=$((pass+1)); }
ko() { echo "MOCK_FAIL $*"; failn=$((failn+1)); }
# /tmp/bin = toybox links, as on the phone (dmesg replaced by the fake)
mkdir -p /tmp/bin; for a in $(qemu-aarch64 $TB 2>/dev/null); do ln -sf $TB /tmp/bin/$a; done; rm -f /tmp/bin/dmesg
cat > /tmp/bin/dmesg <<'X'
#!/bin/sh
cat /tmp/vo5-mock/dmesg.txt 2>/dev/null
X
chmod 755 /tmp/bin/dmesg
setup() { # $1 = variant
  rm -rf $M; mkdir -p $M/proc/100/fd $M/proc/101/fd $M/proc/102/fd $M/rp/remoteproc0 $M/rmtfs $M/v2 $M/block/mmcblk0p5 $M/block/mmcblk0p6
  printf '/tmp/radio2/bin/rmtfs\0-o\0%s\0-v\0' $M/rmtfs > $M/proc/100/cmdline
  printf '/tmp/radio2/bin/tqftpserv\0' > $M/proc/101/cmdline
  printf '/tmp/radio2/bin/diag-router\0' > $M/proc/102/cmdline
  ln -s $M/rmtfs/modem_fs1 $M/proc/100/fd/3
  echo 4080000.remoteproc > $M/rp/remoteproc0/name; echo running > $M/rp/remoteproc0/state; echo enabled > $M/rp/remoteproc0/recovery
  printf 'MAJOR=179\nMINOR=5\nDEVNAME=mmcblk0p5\nDEVTYPE=partition\nPARTNAME=modemst1\n' > $M/block/mmcblk0p5/uevent
  printf 'MAJOR=179\nMINOR=6\nDEVNAME=mmcblk0p6\nDEVTYPE=partition\nPARTNAME=modemst2\n' > $M/block/mmcblk0p6/uevent
  echo 4 > $M/block/mmcblk0p5/size; echo 4 > $M/block/mmcblk0p6/size; echo 179:5 > $M/block/mmcblk0p5/dev; echo 179:6 > $M/block/mmcblk0p6/dev
  head -c 2048 /dev/urandom > $M/rmtfs/modem_fs1; head -c 2048 /dev/urandom > $M/rmtfs/modem_fs2
  echo "[    1.0] boot" > $M/dmesg.txt
  # PDC state: id|desc|size|version
  cat > $M/cfgs <<X
7af52eaad094b6617fd163ca7370863364601f22|ROW_Commercial|45828|0x8030808
83ed837eb5fb5ced4c9240c6390067b80a912c7a|VoLTE_OPNMKT_CT|69008|0x80413E1
f89b251250f56049b2e0accb10842d571df38192|VoLTE-CU|50116|0x8041560
8ae9d9e3dfc2b139024bc8cae91060cabd292481|Volte_OpenMkt-Commercial-CMCC|52772|0x8032010
X
  echo 7af52eaad094b6617fd163ca7370863364601f22 > $M/active
  cat > $M/qmicli <<'X'
#!/bin/sh
M=/tmp/vo5-mock; echo "qmicli $*" >> /tmp/vo5-qmicli-calls.log
[ "$1" = -d ] && [ "$2" = qrtr://0 ] || { echo "error: bad device"; exit 1; }
[ "$(cat $M/rp/remoteproc0/state)" = running ] || { echo "error: couldn't open the QmiDevice: node 0 not found"; exit 1; }
case "$3" in
--pdc-noop) exit 0;;
--pdc-list-configs=software)
  [ -f $M/noind ] && { echo "Total configurations: 0"; exit 0; }
  n=$(grep -c . $M/cfgs); echo "Total configurations: $n"; i=0
  while IFS='|' read -r id d s v; do i=$((i+1)); st=Inactive; [ "$id" = "$(cat $M/active)" ] && st=Active
    printf 'Configuration %s:\n\tDescription: %s\n\tType:        software\n\tSize:        %s\n\tStatus:      %s\n\tVersion:     %s\n\tID:          %s\n' "$i" "$d" "$s" "$st" "$v" "$(echo $id | sed 's/../&:/g; s/:$//' | tr a-f A-F)"
  done < $M/cfgs; exit 0;;
--pdc-load-config=*)
  f=${3#--pdc-load-config=}; sz=$(wc -c < "$f"); o=0
  while [ $o -lt $sz ]; do o=$((o+1024)); [ $o -gt $sz ] && o=$sz; echo "Uploaded $o  of $sz"; done
  echo "$(sha1sum < "$f" | cut -c1-40)|France-Commercial-Orange|$sz|0x8040B20" >> $M/cfgs; echo "Finished loading"; exit 0;;
--pdc-activate-config=software,*)
  id=${3#--pdc-activate-config=software,}; grep -q "^$id|" $M/cfgs || { echo "error: couldn't activate config: InvalidId"; exit 1; }
  echo "[qrtr://0] Successfully requested config activation"
  [ -f $M/killrmtfs ] && rm -rf $M/proc/100
  [ -f $M/nossr ] && { echo $id > $M/active; exit 0; }   # volte5: 28 Sep behaviour (applied in place, no SSR)
  ( sleep 1; echo crashed > $M/rp/remoteproc0/state
    echo "[  99.1] qcom-q6v5-mss 4080000.remoteproc: fatal error received: mcfg_utils.c:1103:MCFG reset" >> $M/dmesg.txt
    echo "[  99.1] remoteproc remoteproc0: crash detected in 4080000.remoteproc: type fatal error" >> $M/dmesg.txt
    sleep 2; echo $id > $M/active; echo running > $M/rp/remoteproc0/state
    echo "[ 105.0] remoteproc remoteproc0: remote processor 4080000.remoteproc is now up" >> $M/dmesg.txt ) > /dev/null 2>&1 &
  [ -f $M/hang ] && sleep 100; exit 0;;
esac
echo "unknown $*"; exit 1
X
  cat > $M/v2/volte-probe <<'X'
#!/bin/sh
M=/tmp/vo5-mock; a=$(cat $M/active)
echo "A6L_VOLTE_PDC_ACTIVE type=SW id=$a"
while IFS='|' read -r id d s v; do echo "A6L_VOLTE_PDC_INFO type=SW id=$id size=$s version=$v desc='$d'"; done < $M/cfgs
grep -q "^$a|France" $M/cfgs && echo "A6L_VOLTE_IMS_SVC 33 IMSA(ims-application) PRESENT" || echo "A6L_VOLTE_IMS_SVC 33 IMSA(ims-application) absent"
X
  cat > $M/v2/volte2-test.sh <<'X'
echo "A6L_VOLTE2_STEP 1 radio2 skipped (modem running) KEEP_ONLINE=$KEEP_ONLINE SKIP_RADIO=$SKIP_RADIO"
echo "A6L_QMI_SERVING reg=registered iccid=8933011234567890123 imsi=208011234567890"
echo "A6L_IMSDCM_REQ from=0:116 0x0034 SERVICE_ENABLE_STATUS -> ok type=0 txn=3 msg=0x0034 [01]0000000000000000"
echo "A6L_VOLTE_NAS_SYSINFO tlv=0x19 len=29 010301030100010000ffff0109897201000000013230383031ff01a2cd"
echo "A6L_VOLTE2_RESULT dcm_requests=5 pdp_activate=1 pdp_up=1 imsa=present registered=yes (logs /tmp/volte2-logs)"
X
  chmod 755 $M/qmicli $M/v2/volte-probe
  case "${1:-}" in
    noprobe) rm $M/v2/volte-probe;;
  esac
}
run() { # run <expect_rc> <expect_marker> env... (MODE in env)
  erc=$1; mk=$2; shift 2
  env -i PATH=/usr/bin:/bin HOME=/tmp D=$B VOLTE2=$M/v2 L=$M/logs A6L_PROC=$M/proc A6L_RPROC=$M/rp A6L_BLOCK=$M/block \
    A6L_RMTFS_DIR=$M/rmtfs A6L_QMICLI=$M/qmicli A6L_KMSG=$M/dmesg.txt A6L_T=0.2 READ_REAL=0 WATCH_AFTER=2 QEMU_LD_PREFIX=$RD \
    "$@" qemu-aarch64 $SH $B/volte4-test.sh > $M/out.txt 2>&1
  rc=$?; o=$(grep -v "linker" $M/out.txt); RN=$((${RN:-0}+1)); mkdir -p ~/vo5/mock-out; grep -v linker $M/out.txt > ~/vo5/mock-out/$RN-$(echo "$*" | tr " =" "__").txt
  if [ $rc = $erc ] && echo "$o" | grep -q "$mk"; then ok "$* -> rc=$rc $mk"; else ko "$* -> rc=$rc (want $erc, $mk)"; echo "$o" | tail -n 25; fi
}
# 1 happy path
setup
run 0 A6L_VOLTE4_RO_PASS MODE=ro
grep -q "orange_loaded=no" $M/out.txt && ok "ro reports orange not loaded" || ko "ro orange flag"
run 0 A6L_VOLTE4_LOAD_PASS MODE=load A6L_RF_APPROVED=1
grep -q "A6L_VOLTE4_LOAD_PASS id=965d8d766f9ac0e698ca5d4433395a31526d2d8d" $M/out.txt && ok "load id = sha1 of MBN" || ko "load id"
grep -q "A6L_VOLTE4_GATE rmtfs pid=100 RAM mode" $M/out.txt && ok "rmtfs RAM gate logged" || ko "rmtfs gate log"
run 0 A6L_VOLTE4_LOAD_SKIP MODE=load A6L_RF_APPROVED=1
run 0 A6L_VOLTE4_ACTIVATE_PASS MODE=activate A6L_RF_APPROVED=1
grep -q "A6L_VOLTE4_SSR seen=1" $M/out.txt && ok "SSR seen" || ko "SSR not seen"
grep -q "new_fatal=0" $M/out.txt && ok "no new fatal after restart" || ko "new fatal"
run 0 A6L_VOLTE4_ACTIVATE_SKIP MODE=activate A6L_RF_APPROVED=1
run 0 "A6L_VOLTE4_VOLTE2_RESULT dcm_requests=5 pdp_activate=1 pdp_up=1 imsa=present registered=yes" MODE=verify A6L_RF_APPROVED=1
grep -q "KEEP_ONLINE=1 SKIP_RADIO=1" $M/out.txt && ok "volte2 chained with KEEP_ONLINE=1 SKIP_RADIO=1" || ko "volte2 env"
grep -qE "8933011234567890123|208011234567890" $M/out.txt $M/logs/*.txt && ko "ICCID/IMSI leaked" || ok "ICCID/IMSI masked in output and logs"
run 0 A6L_VOLTE4_VERIFY_PASS MODE=verify NO_VOLTE2=1
run 0 A6L_VOLTE4_REVERT_PASS MODE=revert A6L_RF_APPROVED=1
grep -q "active='ROW_Commercial'" $M/out.txt && ok "revert back to ROW" || ko "revert state"
run 1 "active is 'ROW_Commercial'" MODE=verify NO_VOLTE2=1
# 2 gates (each must abort with rc=20 before any qmicli write)
rm -f /tmp/vo5-qmicli-calls.log
setup; run 20 "A6L_RF_APPROVED!=1" MODE=load
setup; printf '/tmp/radio2/bin/rmtfs\0-v\0' > $M/proc/100/cmdline; run 20 "NOT in RAM mode" MODE=load A6L_RF_APPROVED=1
setup; mkdir -p $M/proc/103; printf '/tmp/radio2/bin/rmtfs\0-o\0%s\0' $M/rmtfs > $M/proc/103/cmdline; run 20 "exactly 1 rmtfs" MODE=activate A6L_RF_APPROVED=1
setup; rm $M/rmtfs/modem_fs2; run 20 "modem_fs2 missing" MODE=load A6L_RF_APPROVED=1
setup; head -c 1024 /dev/urandom > $M/rmtfs/modem_fs1; run 20 "size 1024 != modemst1 2048" MODE=load A6L_RF_APPROVED=1
setup; ln -s /dev/block/mmcblk0p5 $M/proc/101/fd/7; run 20 "real partition open" MODE=load A6L_RF_APPROVED=1
setup; ln -s /dev/block/by-name/modemst2 $M/proc/101/fd/8; run 20 "real partition open" MODE=revert A6L_RF_APPROVED=1
setup; mkdir -p $M/proc/104; printf '/tmp/volte2/a6l-imsdcm\0--imsa\0' > $M/proc/104/cmdline; run 20 "other QMI client running: a6l-imsdcm" MODE=activate A6L_RF_APPROVED=1
setup; mkdir -p $M/proc/105; printf '/tmp/volte4/q/ld-musl-aarch64.so.1\0--library-path\0x\0/tmp/volte4/q/qmicli.bin\0' > $M/proc/105/cmdline; run 20 "another qmicli" MODE=load A6L_RF_APPROVED=1
setup; echo offline > $M/rp/remoteproc0/state; run 20 "modem remoteproc not running" MODE=ro
setup; cp -r $B $M/badbundle; printf 'X' >> $M/badbundle/mbn/France-Commercial-Orange.mbn
  env -i PATH=/usr/bin:/bin D=$M/badbundle VOLTE2=$M/v2 L=$M/logs A6L_PROC=$M/proc A6L_RPROC=$M/rp A6L_BLOCK=$M/block A6L_RMTFS_DIR=$M/rmtfs A6L_QMICLI=$M/qmicli A6L_KMSG=$M/dmesg.txt A6L_T=0.2 READ_REAL=0 QEMU_LD_PREFIX=$RD MODE=load A6L_RF_APPROVED=1 qemu-aarch64 $SH $M/badbundle/volte4-test.sh > $M/out.txt 2>&1; rc=$?
  { [ $rc = 20 ] && grep -q "MBN sha256" $M/out.txt && grep -q "bundle SHA256SUMS" $M/out.txt; } && ok "tampered MBN -> abort rc=20 (sha pin + SHA256SUMS)" || { ko "tampered MBN rc=$rc"; tail -5 $M/out.txt; }
grep -q "load-config\|activate-config" /tmp/vo5-qmicli-calls.log 2>/dev/null && ko "a write was issued during the gate tests" || ok "no write issued in any gate test"
# 3 degraded reads: qmicli list gives 0 (no indications) -> volte-probe fallback; and no probe at all
setup; touch $M/noind; run 0 A6L_VOLTE4_RO_PASS MODE=ro
grep -q "qmicli_total=0 active='ROW_Commercial'" $M/out.txt && ok "fallback to volte-probe when qmicli lists 0" || ko "fallback"
setup noprobe; run 0 A6L_VOLTE4_RO_PASS MODE=ro
setup noprobe; touch $M/noind; run 1 "no active SW config read" MODE=ro
# 4 activate while qmicli hangs (waits for device removal): timeout path, SSR still detected
setup; run 0 A6L_VOLTE4_LOAD_PASS MODE=load A6L_RF_APPROVED=1; touch $M/hang
run 0 A6L_VOLTE4_ACTIVATE_PASS MODE=activate A6L_RF_APPROVED=1
grep -q "A6L_VOLTE4_QMICLI_RC 124" $M/out.txt && ok "qmicli hang -> killed after 60 s, activation still verified" || ko "hang path"
# 6 volte5: activation applied in place (no SSR, as on 28 Sep) -> PASS restart=no; exited pid (no cmdline) -> no noise
setup; run 0 A6L_VOLTE4_LOAD_PASS MODE=load A6L_RF_APPROVED=1; touch $M/nossr; mkdir -p $M/proc/4242/fd
run 0 "A6L_VOLTE4_ACTIVATE_PASS active='France-Commercial-Orange' id=965d8d766f9ac0e698ca5d4433395a31526d2d8d restart=no" MODE=activate A6L_RF_APPROVED=1
grep -q "A6L_VOLTE4_SSR seen=0" $M/out.txt && grep -q "WATCH 2s (no restart): new_fatal=0" $M/out.txt && ok "no-SSR path watched" || ko "no-SSR watch"
grep -qiE "cmdline|can't open|No such file" $M/out.txt && ko "cmdline noise" || ok "no /proc/<pid>/cmdline noise"
setup; touch $M/nossr; run 0 A6L_VOLTE4_LOAD_PASS MODE=load A6L_RF_APPROVED=1; touch $M/killrmtfs
run 1 "did not come back cleanly" MODE=activate A6L_RF_APPROVED=1
grep -q "A6L_VOLTE4_DAEMON rmtfs pid=DEAD" $M/out.txt && ok "no-SSR path: dead rmtfs -> FAIL" || ko "dead rmtfs not seen"
# 7 volte5: masking keeps QMI hex, masks IMSI/ICCID
setup; touch $M/nossr; run 0 A6L_VOLTE4_LOAD_PASS MODE=load A6L_RF_APPROVED=1; run 0 "restart=no" MODE=activate A6L_RF_APPROVED=1
run 0 "A6L_VOLTE4_VOLTE2_RESULT" MODE=verify A6L_RF_APPROVED=1
grep -q "\[01\]0000000000000000" $M/out.txt && ok "QMI hex [01]0000000000000000 kept" || ko "QMI hex masked"
grep -q "len=29 010301030100010000ffff0109897201000000013230383031ff01a2cd" $M/out.txt && ok "tlv hex kept" || ko "tlv hex masked"
grep -q "iccid=8933\*\*\*\*\*23 imsi=2080\*\*\*\*\*90" $M/out.txt && ok "IMSI/ICCID masked" || ko "IMSI/ICCID mask"
# 5 activate without load
setup; run 1 "is not loaded: run MODE=load first" MODE=activate A6L_RF_APPROVED=1
echo "MOCK_SUMMARY pass=$pass fail=$failn"
[ $failn = 0 ]
