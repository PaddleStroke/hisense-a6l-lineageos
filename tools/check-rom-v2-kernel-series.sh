#!/usr/bin/env bash
# rom-v2 (agent merge): prove the kernel patch series composes. Copies the touched source dirs of the V67 baseline into a
# scratch tree, applies device/hisense/a6l/kernel/rom-v2/series in order (dry-run first), then builds the qdsp6 dir (the
# only dir two patches share a module set with: q6voice + q6asm-dai xlate + per-direction q6routing) with W=1 against
# out-a6l-phone-v67 and checks every undefined symbol. WSL, ~2 min.
set -uo pipefail
R=/mnt/c/Users/Pierre/Desktop/A6L; K=/home/a6l/kernel/a6l-baseline-7.2; O=/home/a6l/kernel/out-a6l-phone-v67
KD=$R/device/hisense/a6l/kernel; S=${A6L_KSERIES_DIR:-/home/a6l/rom-v2/kseries}; rm -rf $S; mkdir -p $S/t
CL=/home/a6l/android/a6l-lineage24/prebuilts/clang/host/linux-x86/clang-r584948/bin; export PATH=$CL:$PATH
fail() { echo "A6L_KSERIES_FAIL $*"; exit 1; }
for d in sound/soc/qcom sound/soc/codecs include/dt-bindings/sound drivers/pinctrl/qcom drivers/media/platform/qcom/camss drivers/bluetooth drivers/power/supply; do mkdir -p $S/t/$d; cp -r $K/$d/. $S/t/$d/; done
mkdir -p $S/t/drivers/media/i2c; cp $K/drivers/media/i2c/hi846.c $S/t/drivers/media/i2c/
grep -v '^#' $KD/rom-v2/series | while read -r p strip dir rest; do
  [ -n "${p:-}" ] || continue
  tr -d '\r' < $KD/$p > $S/cur.patch
  if [ "$strip" = - ]; then patch --dry-run -s $S/t/$dir < $S/cur.patch > /dev/null && patch -s $S/t/$dir < $S/cur.patch || fail "$p"
  else ( cd $S/t/$dir && patch --dry-run -s -p$strip < $S/cur.patch > /dev/null && patch -s -p$strip < $S/cur.patch ) || fail "$p"; fi
  echo "applied $p"
done || exit 1
grep -q q6asm_dai_of_xlate_dai_name $S/t/sound/soc/qcom/qdsp6/q6asm-dai.c && grep -q tx_sessions $S/t/sound/soc/qcom/qdsp6/q6routing.c && grep -q a6l_pd_users $S/t/drivers/media/platform/qcom/camss/camss-vfe.h || fail "markers"
# merge r4: + drivers/bluetooth (btqca) and drivers/power/supply (qcom_smbx) in the scratch tree above
# merge r4 markers: camfix5 (VFE min clock), qcom_smbx stock FCC bound, btqca MSFT off
# camera-rom (29 Sep 2026): camss rom1 replaces camfix5: VFE min clock is now A6L_VFE_MIN_HZ, + stock CSIPHY clocks marker
grep -rq A6L_VFE_MIN_HZ $S/t/drivers/media/platform/qcom/camss/ && grep -q a6l_csiphy_stock_clocks $S/t/drivers/media/platform/qcom/camss/camss-csiphy.c && grep -q 'A6L_FCC_MAX_UA.*2400000' $S/t/drivers/power/supply/qcom_smbx.c && grep -q a6l_enable_msft $S/t/drivers/bluetooth/btqca.c || fail "markers r4"
Q=$S/qdsp6; cp -r $S/t/sound/soc/qcom/qdsp6 $Q; cp $S/t/sound/soc/qcom/common.h $S/common.h; mkdir -p $Q/include/dt-bindings/sound
cp $S/t/include/dt-bindings/sound/qcom,q6voice.h $Q/include/dt-bindings/sound/; echo 'ccflags-y += -I$(src)/include' >> $Q/Makefile
# btcall (29 Sep 2026): the series changes qcom,q6dsp-lpass-ports.h (pseudo ports 153-155), which the baseline also has: NOSTDINC_FLAGS puts
# the series copy BEFORE the tree's include/ (LINUXINCLUDE precedes ccflags-y)
cp $S/t/include/dt-bindings/sound/qcom,q6dsp-lpass-ports.h $S/t/include/dt-bindings/sound/qcom,q6afe.h $Q/include/dt-bindings/sound/
make -C $K O=$O ARCH=arm64 LLVM=1 M=$Q NOSTDINC_FLAGS="-nostdinc -I$Q/include" CONFIG_SND_SOC_QDSP6_Q6VOICE=m CONFIG_SND_SOC_QDSP6_Q6VOICE_DAI=m W=1 modules -j8 > $S/qdsp6.log 2>&1 || { tail -30 $S/qdsp6.log; fail "qdsp6 build"; }
echo "warnings (q6voice/q6routing/q6asm-dai): $(grep -E 'warning' $S/qdsp6.log | grep -cE 'q6voice|q6mvm|q6cv|q6routing|q6asm-dai')"
# btcall: markers + the modules the pseudo ports touch (q6afe, q6afe-dai, snd-q6dsp-common, q6adm: AFE_PORT_MAX arrays)
grep -q AFE_PORT_ID_VOICE_PLAYBACK_TX $Q/q6afe.c && grep -q q6voice_set_incall_record $Q/q6voice.c && grep -q INCALL_RECORD_RX $Q/q6routing.c && grep -q 'VOICE_PLAYBACK_TX + 1' $S/common.h || fail "markers btcall"
for m in q6voice-common q6mvm q6cvs q6cvp q6voice q6voice-dai q6asm-dai q6routing q6afe q6afe-dai snd-q6dsp-common q6adm; do
  [ -f $Q/$m.ko ] || fail "missing $m.ko"
  for s in $(llvm-nm -u $Q/$m.ko | awk '{print $2}'); do grep -q " $s$" $O/System.map || grep -qP "\t$s\t" $O/Module.symvers || grep -qP "^0x[0-9a-f]+\t$s\t" $Q/Module.symvers || echo "  UNRESOLVED $m: $s"; done
  echo "$m.ko $(sha256sum < $Q/$m.ko | cut -c1-16) vermagic=$(modinfo -F vermagic $Q/$m.ko | cut -d' ' -f1)"
done
grep -q -i mbhc $S/t/sound/soc/codecs/msm8916-wcd-analog.c || fail "marker r5 mbhc2"   # merge r5: MBHC v2 in sound/soc/codecs
# camera-rom (29 Sep 2026): the series camss (rom1) builds W=1 without warnings against v67 and imports only exported symbols
C=$S/camss; cp -r $S/t/drivers/media/platform/qcom/camss $C
make -C $K O=$O ARCH=arm64 LLVM=1 M=$C W=1 modules -j8 > $S/camss.log 2>&1 || { tail -20 $S/camss.log; fail "camss build"; }
[ "$(grep -c 'warning:' $S/camss.log)" = 0 ] || { grep 'warning:' $S/camss.log | head; fail "camss warnings"; }
for s in $(llvm-nm -u $C/qcom-camss.ko | awk '{print $2}'); do grep -qP "\t$s\t" $O/Module.symvers || fail "camss unresolved $s"; done
echo "qcom-camss.ko (series) $(modinfo -F parm $C/qcom-camss.ko | cut -d: -f1 | tr '\n' ' ')vermagic=$(modinfo -F vermagic $C/qcom-camss.ko | cut -d' ' -f1)"
# hi846 merge (29 Sep 2026): series hi846.c = mainline + s_ctrl fix (no 4-lane patch; ROM DT 2-lane); builds W=1 without warnings
H=$S/t/drivers/media/i2c/hi846.c; grep -q $'^\tret = 0;$' $H && ! grep -q 'nr_lanes == 4' $H || fail "marker hi846 set-ctrl fix / no 4-lane"
HM=$S/hi846; mkdir -p $HM; cp $H $HM/; echo 'obj-m += hi846.o' > $HM/Kbuild
make -C $K O=$O ARCH=arm64 LLVM=1 M=$HM W=1 modules > $S/hi846.log 2>&1 || { tail -20 $S/hi846.log; fail "hi846 build"; }
[ "$(grep -c 'warning:' $S/hi846.log)" = 0 ] || { grep 'warning:' $S/hi846.log | head; fail "hi846 warnings"; }
for s in $(llvm-nm -u $HM/hi846.ko | awk '{print $2}'); do grep -qP "\t$s\t" $O/Module.symvers || fail "hi846 unresolved $s"; done
echo "hi846.ko (series) srcversion=$(modinfo -F srcversion $HM/hi846.ko) (ROM firmware/extracted/hi846-rom-20260929: $(modinfo -F srcversion $R/firmware/extracted/hi846-rom-20260929/v67/hi846.ko))"
# fastcharge-rom (29 Sep 2026): series qcom_smbx = fcc-jeita + hvdcp (power29 + P4/P5); builds W=1 without warnings, same srcversion as the staged ROM module
SX=$S/t/drivers/power/supply/qcom_smbx.c; grep -q hvdcp_rerun $SX && grep -q a6l_hv_release_susp $SX || fail "marker fastcharge-rom hvdcp"
SXM=$S/smbx; mkdir -p $SXM; cp $SX $SXM/; echo 'obj-m += qcom_smbx.o' > $SXM/Kbuild
make -C $K O=$O ARCH=arm64 LLVM=1 M=$SXM W=1 modules > $S/smbx.log 2>&1 || { tail -20 $S/smbx.log; fail "qcom_smbx build"; }
[ "$(grep 'warning:' $S/smbx.log | grep -vc 'compiler differs')" = 0 ] || { grep 'warning:' $S/smbx.log | head; fail "qcom_smbx warnings"; }
[ "$(modinfo -F srcversion $SXM/qcom_smbx.ko)" = "$(modinfo -F srcversion $R/firmware/extracted/fastcharge-rom-20260929/v67/qcom_smbx.ko)" ] || fail "qcom_smbx srcversion != fastcharge-rom-20260929"
echo "qcom_smbx.ko (series) srcversion=$(modinfo -F srcversion $SXM/qcom_smbx.ko) = fastcharge-rom-20260929"
echo A6L_KSERIES_PASS
