#!/bin/bash
# diag-r5 (30 Sep 2026): QEMU boot of a diag boot image's ramdisk + kernel (virt machine, QEMU DTB instead of the A6L DT) with the
# phone kit appended as a 2nd initramfs archive and ONE extra init.rc service (QEMU copy only) that runs `d.sh qemu`:
# the V75 RAM init must start adbd/a6lprobe/a6lusbwd, and every kit module (base + display lists, msm separate_gpu_kms=1)
# must load on that kernel (vermagic, modversions CRCs, symbols). No A6L hardware: drivers register, nothing binds.
# usage: Test-DiagR5Qemu.sh <img dir> <kernel Image> <tag> <out dir>     -> DIAG_R5_QEMU_PASS / FAIL
set -u
I=$1; KI=$2; TAG=$3; O=$4; W=/home/a6l/diag-r5; K=$W/kit/phone
rm -rf $O; mkdir -p $O/x
GEN=/home/a6l/kernel/out-a6l-android-init/usr/gen_init_cpio
DTB=$(ls /mnt/c/Users/Pierre/Desktop/A6L/firmware/extracted/android-framework-v72-*-r3/virt.dtb | sort | tail -n 1)
cp $W/v75rd/system/etc/init/hw/init.rc $O/x/init.rc
printf '\n# QEMU self-test only (Test-DiagR5Qemu.sh), never in a phone image\non early-init\n    start diagq\n\nservice diagq /system/bin/sh /diag/d.sh qemu\n    user root\n    group root\n    disabled\n    oneshot\n    seclabel u:r:recovery:s0\n' >> $O/x/init.rc
{ echo "dir /diag 0755 0 0"; (cd $K && find . -mindepth 1 -type d | sed 's|^\./||' | sort | while read d; do echo "dir /diag/$d 0755 0 0"; done)
  (cd $K && find . -type f | sed 's|^\./||' | sort | while read f; do m=0644; [ "$f" = d.sh ] && m=0755; echo "file /diag/$f $K/$f $m 0 0"; done)
  echo "file /system/etc/init/hw/init.rc $O/x/init.rc 0644 0 0"; } > $O/x/list
$GEN $O/x/list > $O/x/extra.cpio
cat $I/ramdisk.cpio $O/x/extra.cpio | gzip -9 > $O/initrd.gz
CMD="console=ttyAMA0 loglevel=8 androidboot.hardware=qcom androidboot.selinux=permissive androidboot.init_rc=/system/etc/init/hw/init.rc printk.devkmsg=on log_buf_len=4M panic=0 androidboot.a6l_diag=$TAG"
timeout 400 qemu-system-aarch64 -machine virt,gic-version=3 -cpu cortex-a53 -smp 4 -m 3072 -nodefaults -nographic -monitor none \
  -serial stdio -nic none -no-reboot -dtb $DTB -kernel $KI -initrd $O/initrd.gz -append "$CMD" > $O/console.log 2>&1 &
q=$!; for i in $(seq 1 380); do sleep 1; grep -q "QEMU_SELFTEST fails=" $O/console.log && { sleep 5; break; }; kill -0 $q 2>/dev/null || break; done
kill $q 2>/dev/null; wait $q 2>/dev/null
L=$O/console.log; ok=1
chk() { if grep -qE "$2" $L; then echo "ok   $1"; else echo "FAIL $1 (no: $2)"; ok=0; fi; }
nck() { if grep -qE "$2" $L; then echo "FAIL $1: $(grep -m1 -E "$2" $L)"; ok=0; else echo "ok   $1"; fi; }
chk init_second_stage "init second stage started"
chk adbd_started "starting service 'adbd'"
chk probe_started "A6L_RAM_PROBE_START"
chk usbwd_started "starting service 'a6lusbwd'"
chk setup_done "A6L_DIAG qemu DONE setup"
chk msm_loaded "A6L_DIAG qemu OK insmod msm.ko"
chk selftest_zero "QEMU_SELFTEST fails=0 "
chk heartbeat "A6L_DIAG hb "
nck no_unknown_symbol "Unknown symbol|disagrees about version|invalid module format|Exec format error"
nck no_panic "Kernel panic|Oops|BUG:|CFI failure|Unable to handle"
grep -E "QEMU_SELFTEST|A6L_DIAG qemu (OK|FAIL)" $L | sed 's/^\[[^]]*\] //' | head -n 40 > $O/summary.txt
[ $ok = 1 ] && echo "DIAG_R5_QEMU_PASS $TAG" || echo "DIAG_R5_QEMU_FAIL $TAG"
