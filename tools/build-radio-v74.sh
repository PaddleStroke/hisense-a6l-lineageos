#!/usr/bin/env bash
# A6L radio v74 build + laptop staging (agent radio, 23 Sep 2026). Builds diag-router (linux-msm/diag 23c12c1 + A6L sysfs
# patch, static bionic), plan-B a6l_diag_sink.ko, the tqftpserv firmware layout and an ath10k board-2.bin from stock BDWLAN,
# then assembles ~/A6L-usb-20260915/v74/radio on the laptop from v71/bundle/{modem-wifi,bluetooth} + these files.
# Nothing touches the phone.
set -eo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; S=$R/firmware/extracted/radio-20260923
K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin
NDK=/home/a6l/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64/bin
IMG=$R/firmware/extracted/peripheral-firmware-20260917-r3/modem/IMAGE
B=/home/a6l/radio-v74-build; rm -rf $B; mkdir -p $B/ov/bin $B/ov/modules/extra $B/ov/firmware; cd $B
tar xzf $S/src/diag-src.tgz; cd diag
SRCS="router/app_cmds.c router/circ_buf.c router/common_cmds.c router/diag.c router/diag_cntl.c router/dm.c router/hdlc.c router/masks.c router/mbuf.c router/peripheral.c router/router.c router/socket.c router/uart.c router/unix.c router/usb.c router/util.c router/watch.c router/peripheral-rpmsg.c"
$NDK/aarch64-linux-android34-clang -static -O2 -Wno-macro-redefined -Wno-address-of-packed-member -DHAS_LIBUDEV=1 -o $B/ov/bin/diag-router $SRCS
$NDK/llvm-strip $B/ov/bin/diag-router; $NDK/llvm-readelf -h -l $B/ov/bin/diag-router | grep -E "Machine|INTERP" || true
echo BUILD_DIAG_ROUTER_OK
mkdir -p $B/sink; cp $S/src/diag-sink/a6l_diag_sink.c $S/src/diag-sink/Kbuild $B/sink/
PATH=$CL:$PATH make -s -C $K O=$O ARCH=arm64 LLVM=1 M=$B/sink modules
cp $B/sink/a6l_diag_sink.ko $B/ov/modules/extra/; PATH=$CL:$PATH llvm-strip --strip-debug $B/ov/modules/extra/a6l_diag_sink.ko
for m in qrtr qrtr-smd; do cp $O/net/qrtr/$m.ko $B/ov/modules/; PATH=$CL:$PATH llvm-strip --strip-debug $B/ov/modules/$m.ko; done
for f in $B/ov/modules/*.ko $B/ov/modules/extra/*.ko; do echo "$(basename $f) $(modinfo -F vermagic $f)"; done
echo BUILD_MODULES_OK
# firmware layout: tqftpserv resolves /readonly/firmware/image/<f> to <fw path>/qcom/hisense/a6l/<f> (dirname of the mss firmware)
F=$B/ov/firmware/qcom/hisense/a6l; mkdir -p $F
( cd $IMG && find MODEM_PR -type f ) | while read -r p; do l=$(echo "$p" | tr 'A-Z' 'a-z'); mkdir -p "$F/$(dirname "$l")"; cp "$IMG/$p" "$F/$l"; done
for f in $IMG/BDWLAN.*; do cp $f $F/$(basename $f | tr 'A-Z' 'a-z'); done
echo "modem_pr files: $(find $F/modem_pr -type f | wc -l), $(du -sh $F/modem_pr | cut -f1)"; ls -la $F/modem_pr/mcfg/configs/mcfg_sw/mbn_sw.dig $F/modem_pr/mcfg/configs/mcfg_hw/mbn_hw.dig
find $F -name '* *' | head -3
sha256sum $IMG/WLANMDSP.MBN | cut -c1-16
mkdir -p $B/ov/firmware/ath10k/WCN3990/hw1.0; python3 $S/src/make-board2.py $IMG $B/ov/firmware/ath10k/WCN3990/hw1.0/board-2.bin
mkdir -p $B/ov/firmware/qca; cp $R/firmware/extracted/controls-radio-prep-20260917/firmware/qca/* $B/ov/firmware/qca/
cp $S/src/run.sh $S/src/run-bt.sh $B/ov/; chmod 755 $B/ov/*.sh
echo "== iw candidates"; ls /home/a6l/android/a6l-lineage24/out/target/product/*/system/bin/iw /home/a6l/android/a6l-lineage24/out/target/product/*/vendor/bin/iw 2>/dev/null || echo "no prebuilt iw in Lineage out"
# repo copy
mkdir -p $S/bin; cp $B/ov/bin/diag-router $B/ov/modules/extra/a6l_diag_sink.ko $B/ov/modules/qrtr.ko $B/ov/modules/qrtr-smd.ko $B/ov/firmware/ath10k/WCN3990/hw1.0/board-2.bin $S/bin/
cp $B/ov/run.sh $B/ov/run-bt.sh $S/bin/; ( cd $S/bin && sha256sum diag-router a6l_diag_sink.ko qrtr.ko qrtr-smd.ko board-2.bin run.sh run-bt.sh > SHA256SUMS; cat SHA256SUMS )
( cd $B/ov && tar czf $S/v74-radio-overlay.tgz . ); sha256sum $S/v74-radio-overlay.tgz
echo STAGE_REPO_OK
# laptop assembly (read-only use of v71; writes only v74/radio)
L="bash $R/.relay/lap.sh"
$L 60 'set -e; cd ~/A6L-usb-20260915; test ! -e v74/radio || mv v74/radio v74/radio.old-$(date +%H%M%S); mkdir -p v74/radio/modules-bt; cp -r v71/bundle/modem-wifi/modules v71/bundle/modem-wifi/bin v71/bundle/modem-wifi/firmware v74/radio/; cp v71/bundle/bluetooth/modules/* v74/radio/modules-bt/; strings v71/bundle/modem-wifi/firmware/ath10k/WCN3990/hw1.0/board-2.bin | grep "^bus=" | head; for f in v71/bundle/modem-wifi/modules/qcom_q6v5_mss.ko; do strings $f | grep "^vermagic="; done; sha256sum v71/bundle/modem-wifi/firmware/qcom/hisense/a6l/wlanmdsp.mbn | cut -c1-16; echo LAP_COPY_OK'
/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf 'C:/Users/Pierre/Desktop/A6L/firmware/extracted/radio-20260923/v74-radio-overlay.tgz' a6l-laptop:A6L-usb-20260915/v74/v74-radio-overlay.tgz
$L 60 'set -e; cd ~/A6L-usb-20260915/v74/radio; mv firmware/ath10k/WCN3990/hw1.0/board-2.bin firmware/ath10k/WCN3990/hw1.0/board-2.bin.v71-orig; tar xzf ../v74-radio-overlay.tgz; mv firmware/ath10k/WCN3990/hw1.0/board-2.bin.v71-orig ../radio-v71-board-2.bin; rm -f SHA256SUMS; find . -type f ! -name "SHA256SUMS*" | sed "s#^\./##" | sort | xargs -d "\n" sha256sum > SHA256SUMS.tmp; mv SHA256SUMS.tmp SHA256SUMS; wc -l SHA256SUMS; du -sh .; ls; cat modules/order.txt; sha256sum SHA256SUMS; echo LAP_STAGE_OK'
