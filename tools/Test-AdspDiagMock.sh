#!/usr/bin/env bash
# Host-side mock test for device/hisense/a6l/diagnostic/adsp_diag_r2.sh.
# Fake sysfs/device-tree/kmsg + stub dmesg/insmod; a simulator plays the kernel.
# Proves control flow, fail-closed behaviour, cleanup and exit codes — nothing about the DSP.
set -u
SCRIPT=$(cd "$(dirname "$0")/.." && pwd)/device/hisense/a6l/diagnostic/adsp_diag_r2.sh
WORK=$(mktemp -d); pass=0; failn=0
mk() { # mk <scenario>
    T=$WORK/$1; rm -rf "$T"; mkdir -p "$T"/{bin,sys/class/remoteproc/remoteproc0,sys/bus/rpmsg/devices,sys/devices/platform/soc@0/15700000.remoteproc,dt/chosen,dt/soc@0/remoteproc@15700000,ram/diag/modules,ram/diag/firmware,ram/lib/firmware/qcom/hisense}
    for d in smem smp2p-adsp firmware:scm 17911000.mailbox 1f40000.hwlock 5100000.iommu; do mkdir -p "$T/sys/bus/platform/devices/$d/driver"; done
    printf 'diag1\0' > "$T/dt/chosen/hisense,a6l-adsp"; printf 'okay\0' > "$T/dt/soc@0/remoteproc@15700000/status"
    R=$T/sys/class/remoteproc/remoteproc0; echo adsp > $R/name; echo offline > $R/state; echo enabled > $R/recovery; echo x > $R/firmware
    ln -s "$T/sys/devices/platform/soc@0/15700000.remoteproc" $R/device
    ( cd "$T/ram/diag/firmware"; echo mdt > adsp.mdt; for i in $(seq -w 2 21); do echo "seg$i" > adsp.b$i; done; sha256sum adsp.* > SHA256SUMS )
    ( cd "$T/ram/diag/modules"; for m in qrtr.ko qcom_q6v5_pas.ko; do echo "$m" > $m; done; printf '# order\nqrtr.ko\nqcom_q6v5_pas.ko\n' > order.txt; sha256sum *.ko > SHA256SUMS )
    echo "tmpfs $T/ram tmpfs rw 0 0" > "$T/mounts"; : > "$T/kmsg"; : > "$T/extra-dmesg"
    printf '#!/bin/sh\ncat "%s/kmsg" "%s/extra-dmesg"\n' "$T" "$T" > "$T/bin/dmesg"
    printf '#!/bin/sh\necho "insmod $1" >> "%s/insmod.log"\n' "$T" > "$T/bin/insmod"; chmod +x "$T/bin/"*
}
sim() { # sim <T> <mode>: emulate the remoteproc state machine for up to 20 s
    local R=$1/sys/class/remoteproc/remoteproc0 i=0
    while [ $i -lt 200 ]; do
        case "$(cat $R/state 2>/dev/null)" in
            start) case $2 in ok) echo running > $R/state;; crash) echo running > $R/state; echo "remoteproc remoteproc0: fatal error received: mock" >> "$1/extra-dmesg";; stuck) :;; esac;;
            stop) echo offline > $R/state;;
        esac; sleep 0.1; i=$((i+1))
    done
}
run() { # run <scenario> <simmode> <expect-rc> <expect-result-prefix>
    local T=$WORK/$1; sim "$T" "$2" & local sp=$!
    ( PATH="$T/bin:$PATH" DIR=$T/ram/diag SYS=$T/sys PROCDT=$T/dt FWDIR=$T/ram/lib/firmware/qcom/hisense/a6l KMSG=$T/kmsg MOUNTS=$T/mounts HOLD_S=2 START_TIMEOUT_S=3 STOP_TIMEOUT_S=3 sh "$SCRIPT" ) > "$T/out.txt" 2>&1
    local rc=$? res; kill $sp 2>/dev/null; wait $sp 2>/dev/null
    res=$(cat "$T/ram/diag/evidence/RESULT" 2>/dev/null)
    local R=$T/sys/class/remoteproc/remoteproc0 ok=1
    [ "$rc" = "$3" ] || ok=0
    case "$res" in "$4"*) ;; *) ok=0;; esac
    [ "$(cat $R/recovery)" = enabled ] || { ok=0; echo "  recovery not restored"; }
    # preinst: the script must NOT delete firmware it did not install itself.
    [ "$1" != preinst ] && [ -e "$T/ram/lib/firmware/qcom/hisense/a6l/adsp.mdt" ] && { ok=0; echo "  firmware left installed"; }
    case "$1" in stuck) ;; *) [ "$(cat $R/state)" = offline ] || { ok=0; echo "  state left $(cat $R/state)"; };; esac
    if [ $ok = 1 ]; then pass=$((pass+1)); echo "PASS $1 rc=$rc result=$res"; else failn=$((failn+1)); echo "FAIL $1 rc=$rc result=$res"; tail -n 8 "$T/out.txt"; fi
}
mk ok;        run ok ok 0 PASS
mk crash;     run crash crash 1 FAIL-START-DMESG
mk stuck;     run stuck stuck 1 FAIL-START
mk badhash;   echo tampered >> $WORK/badhash/ram/diag/firmware/adsp.b07; run badhash ok 1 FAIL-HASH
mk badmod;    echo tampered >> $WORK/badmod/ram/diag/modules/qrtr.ko; run badmod ok 1 FAIL-HASH
mk extramod;  echo evil > $WORK/extramod/ram/diag/modules/evil.ko; echo evil.ko >> $WORK/extramod/ram/diag/modules/order.txt; run extramod ok 1 FAIL-MODULE
mk identity;  rm $WORK/identity/sys/class/remoteproc/remoteproc0/device; mkdir -p $WORK/identity/sys/devices/platform/soc@0/4080000.remoteproc; ln -s $WORK/identity/sys/devices/platform/soc@0/4080000.remoteproc $WORK/identity/sys/class/remoteproc/remoteproc0/device; run identity ok 1 FAIL-IDENTITY
mk wrongimg;  printf 'other\0' > $WORK/wrongimg/dt/chosen/hisense,a6l-adsp; run wrongimg ok 1 FAIL-IMAGE
mk notram;    echo "ext4 $WORK/notram/ram ext4 rw 0 0" | awk '{print "/dev/sda",$2,$3,$4,$5,$6}' > $WORK/notram/mounts; run notram ok 1 FAIL-PATH
mk preinst;   mkdir -p $WORK/preinst/ram/lib/firmware/qcom/hisense/a6l; echo x > $WORK/preinst/ram/lib/firmware/qcom/hisense/a6l/adsp.mdt; run preinst ok 1 FAIL-ORDER
# write_bounded against a FIFO with no reader: must return 124 within the bound.
T=$WORK/fifo; mkdir -p $T; mkfifo $T/state
sed -n '/^write_bounded() {/,/^}/p' "$SCRIPT" > $T/fn.sh
s=$(date +%s); ( EV=$T; log() { :; }; . $T/fn.sh; write_bounded start $T/state 2 start ); rc=$?; e=$(( $(date +%s) - s ))
if [ $rc = 124 ] && [ $e -le 5 ]; then pass=$((pass+1)); echo "PASS blocked-write rc=$rc ${e}s"; else failn=$((failn+1)); echo "FAIL blocked-write rc=$rc ${e}s"; fi
pkill -f "$T/state" 2>/dev/null
echo "A6L_ADSP_DIAG_MOCK passed=$pass failed=$failn"; rm -rf "$WORK"; [ $failn = 0 ]
