#!/usr/bin/env bash
# audfix agent, 24 Sep 2026. OFFLINE build (nothing touches the phone). Run from WSL (relay, nohup):
#  - q6routing.ko = 7.2.3 q6routing.c + device/hisense/a6l/kernel/audfix/a6l-q6routing-per-direction-v75.patch (own M= dir, W=1)
#  - bundle v75/audio4 = v75/kvoice module stack with the new q6routing.ko + tinyplay/tinycap/wavs + audfix run.sh/mixer3
#    -> repo firmware/extracted/audio4-20260924, laptop ~/A6L-usb-20260915/v75/audio4
set -uo pipefail
W=/mnt/c/Users/Pierre/Desktop/A6L; B=/home/a6l/audfix; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
L=/home/a6l/android/a6l-lineage24; SRC=$W/device/hisense/a6l/kernel/audfix; P=$SRC/a6l-q6routing-per-direction-v75.patch
KV=$W/firmware/extracted/kvoice-20260924/v75/kvoice; A3=$W/firmware/extracted/audio3-20260924/v75/audio3; A=$W/firmware/extracted/audio4-20260924
export PATH=$L/prebuilts/clang/host/linux-x86/clang-r584948/bin:$PATH KBUILD_BUILD_USER=a6l KBUILD_BUILD_HOST=a6l-build KBUILD_BUILD_TIMESTAMP='2026-09-14 00:00:00 UTC'
mkdir -p $B; rm -rf $B/out $B/bundle; mkdir -p $B/out
# ---- 1. q6routing.ko
rm -rf $B/q6routing; M=$B/q6routing/qdsp6; mkdir -p $M/a/sound/soc/qcom/qdsp6; cp $K/sound/soc/qcom/qdsp6/*.h $M/; cp $K/sound/soc/qcom/common.h $B/q6routing/  # q6afe.h includes ../common.h
cp $K/sound/soc/qcom/qdsp6/q6routing.c $M/a/sound/soc/qcom/qdsp6/
( cd $M/a && patch -p1 < $P ) || { echo Q6ROUTING_PATCH_FAIL; exit 1; }
cp $M/a/sound/soc/qcom/qdsp6/q6routing.c $M/; rm -rf $M/a; printf 'obj-m += q6routing.o\n' > $M/Makefile
( cd $K && patch -p1 --dry-run -s < $P && echo Q6ROUTING_PATCH_APPLIES_TO_TREE )
make -C $K O=$O ARCH=arm64 LLVM=1 W=1 -j8 M=$M modules > $M/build.log 2>&1 || { grep -a -B2 -A6 "error" $M/build.log | head -40; exit 1; }
llvm-strip --strip-debug -o $B/out/q6routing.ko $M/q6routing.ko
echo "Q6ROUTING_BUILD_PASS warnings=$(grep -a -c 'warning:' $M/build.log)"; grep -a "warning:" $M/build.log | head -10
echo "vermagic new: $(modinfo -F vermagic $B/out/q6routing.ko)"; echo "vermagic kvoice: $(modinfo -F vermagic $KV/modules/q6routing.ko)"
echo "depends new: $(modinfo -F depends $B/out/q6routing.ko) | kvoice: $(modinfo -F depends $KV/modules/q6routing.ko)"
llvm-nm $B/out/q6routing.ko | grep -E "msm_routing_(put|get)_audio_mixer_tx|q6routing_stream_(open|close)|__ksymtab" | head
[ "$(modinfo -F vermagic $B/out/q6routing.ko)" = "$(modinfo -F vermagic $KV/modules/q6routing.ko)" ] && echo VERMAGIC_MATCH || echo VERMAGIC_MISMATCH
# ---- 2. bundle
U=$B/bundle/audio4; mkdir -p $U
cp -r $KV/modules $KV/bin $KV/extra $U/; cp $B/out/q6routing.ko $U/modules/q6routing.ko
cp $A3/bin/tinyplay $A3/bin/tinycap $U/bin/; cp -r $A3/wav $U/; cp $A3/wav-level.py $U/
cp -r $SRC/bundle/mixer3 $U/; cp $SRC/bundle/run.sh $U/run.sh
( cd $U && sed -i 's/\r$//' run.sh mixer3/*.txt modules/order.txt; find . -type f ! -name SHA256SUMS | sed 's|^\./||' | sort | xargs sha256sum > SHA256SUMS )
bash -n $U/run.sh && echo SYNTAX_OK
( cd $U && sha256sum -c --quiet SHA256SUMS && echo BUNDLE_HASH_OK; wc -l < SHA256SUMS; du -sh . )
rm -rf $A; mkdir -p $A/modules $A/v75; cp -r $U $A/v75/; cp $B/out/q6routing.ko $A/modules/; cp $P $A/; cp $M/build.log $A/q6routing-build.log
( cd $A && find . -type f ! -name SHA256SUMS-all | sort | xargs sha256sum > SHA256SUMS-all ); grep -E "q6routing.ko|run.sh|\.patch" $A/SHA256SUMS-all
# ---- 3. laptop
SSH="/mnt/c/Windows/System32/OpenSSH/ssh.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"; SCP="/mnt/c/Windows/System32/OpenSSH/scp.exe -F C:/Users/Pierre/Desktop/A6L/tools/a6l-laptop-ssh.conf"
timeout 60 $SSH a6l-laptop 'mkdir -p A6L-usb-20260915/v75 && rm -rf A6L-usb-20260915/v75/audio4' < /dev/null && \
timeout 180 $SCP -r -q C:/Users/Pierre/Desktop/A6L/firmware/extracted/audio4-20260924/v75/audio4 a6l-laptop:A6L-usb-20260915/v75/ < /dev/null && \
timeout 60 $SSH a6l-laptop 'cd A6L-usb-20260915/v75/audio4 && sha256sum -c --quiet SHA256SUMS && echo LAPTOP_AUDIO4_OK; du -sh .' < /dev/null || echo LAPTOP_COPY_FAIL
echo AUDFIX_DONE
