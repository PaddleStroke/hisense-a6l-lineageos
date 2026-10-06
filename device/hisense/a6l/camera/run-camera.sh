#!/system/bin/sh
# A6L camera attended test, camfix12 revision (29 Sep 2026, bundle camera14 = firmware/extracted/camera-20260929-camfix12).
# ATTENDED ONLY: switches the 2.8 V camera rail (gpio51), MCLKs and (new) the CSID rails pm660_l1 / pm660l_l1.
# Works on the V74 recovery: the camera DT comes from the runtime overlay extra/a6l_cam_ovl.ko (loaded first).
#   MODE=check|load|probe|list|bars|live|focus|off   SENSOR=imx576|hi846|s5k3t1   D=bundle dir (default /tmp/camera)
#   optional env: PHY= CSID= W= H= EXP= GAIN=
#   camfix2 runtime knobs (qcom_camss params, no reload): DBG=0|1|2 VFE_MIN=<Hz> CSID_IRQMASK=<0x..>
#   camfix3 knob: WM=3 (default: stock-like RDI write master, every frame + scratch buffer, stock BUFFER_CFG)
#                 WM=1 (every frame, upstream BUFFER_CFG) | WM=0 (upstream frame-drop toggling = camfix2 behaviour)
#   camfix4 knobs: WM=4 simple fixed ping/pong (driver buffers + copy + A6L_WM4_SCAN), +2 stock BUFFER_CFG,
#                 +8 reload WM on bus error, +16 fixed-mode max = frame size;  WMMAX=1|0 (WM MAX address regs,
#                 default 1 = stock fix), CGC=1|0 (VFE 0x3C CGC override, default 1 = stock)
#   MODE=wmloop: one bars capture per entry of WMS (default "4 3 0 4:0:1 4:1:0" = WM[:WMMAX[:CGC]]), same boot
#   camfix5 knobs: VFE_MIN=<Hz> (qcom_camss default now 404000000 = stock; 0 = upstream 120 MHz),
#                 FDUMP=<n> per-event VFE register lines (A6L_VFE0_F ...) for the first n SOF/done/buserr events,
#                 FDUMP_WM=<wm> (default 0), UB=<depth> WM0 unified-buffer depth experiment (0 = upstream 682)
#   MODE=clkloop: one bars capture per VFE clock in VFES (default "120000000 300000000 404000000 480000000"),
#                 WM=${WM:-3}; summary /tmp/cam-<sensor>-clkloop.sum; a6l_vfe_min is set back to VFE_AFTER (404000000)
#   camfix6 knobs: V6=<mask> qcom_camss.a6l_v6 (default 0x0f): 1 stock VBIF QoS, 2 WM0 UB 1904, 4 ICC vote
#                 (ICC_AVG/ICC_PEAK kBps, default 2000000/4000000), 8 bus-error recovery (W1C + WM reload), 16 RDI
#                 line-based WM (stock), 32 frame-based burst 3, 64 stock RDI UB 192, 128 per-SOF progress probe (WM=4)
#                 PDUMP=<n> SOFs the probe logs (default 40)
#   MODE=sweep: one bars capture per entry of VARS (name:v6[:icc_avg:icc_peak]) with WM=${WM:-6}, progress probe on,
#                 table in /tmp/cam-<sensor>-sweep.sum; params go back to the defaults at the end
#   camfix7 knobs: V7=<mask> qcom_camss.a6l_v7 (default 0x01): 1 MMCC/SMMU register diagnostics (read-only),
#                 2 stock mmss SMMU attach-impl-defs (stay set until reboot), 4 hold mnoc_ahb/bimc_smmu_ahb/axi/
#                 camss_micro_ahb during the stream, 8 stock SMMU CB ACTLR 0x70000000 (stays set until reboot)
#                 HTS=<n> imx576_a6l.a6l_hts LINE_LENGTH_PCK override (0 = mode 5544; 11088 = half line rate, 15 fps)
#                 DBGFS=1: clk_summary / pm_genpd_summary / interconnect_summary snapshot 4 s into the capture
#   MODE=sweep (camfix7): VARS entries name:v6:v7:hts[:icc_avg:icc_peak], default = camfix7 matrix (10 captures),
#                 WM=${WM:-6}; table /tmp/cam-<sensor>-sweep.tab (+ v7, hts, SMMU fsr, MMCC summary columns)
#   camfix8 knob: V8=<mask> qcom_camss.a6l_v8 (default 0x01): 1 MMSS AXI / MMPLL4 diagnostics + VBIF write-stick probe,
#                 2 enable axi_clk_src (-> MMPLL4) for the stream, 4 +set axi_clk_src 406 MHz (MMPLL0/2, persists),
#                 8 keep the bit-2 enable until reboot, 16 raw MMIO MMPLL4 enable (persists)
#   MODE=sweep (camfix8): VARS entries name:v6:v7:hts:v8[:icc_avg:icc_peak], default = camfix8 matrix (9 captures,
#                 ~3 min), DBGFS=1 by default; table /tmp/cam-<sensor>-sweep8.tab. Detached-safe: run it with
#                 nohup setsid ... > /tmp/c10-sweep.log 2>&1 & and poll the log for SWEEP_DONE.
#   camfix9 knob: V9=<mask> qcom_camss.a6l_v9 (default 0x01): 1 camera VBIF full dump + write-stick tests on fields
#                 every VBIF has (A6L_V9 VBIF_WR WRITABLE|READONLY) + VBIF clock view, 2 VBIF_CLKON bit0 force-on
#                 during the stream, 4 VBIF_CLK_FORCE_CTRL0/1 all ones during the stream, 8 power VFE1 (shared VBIF)
#                 during the stream, 16 re-apply the stock VBIF settings after 2/4/8 (all restored at stream-off
#                 except 16)
#   MODE=sweep (camfix9): VARS entries name:v6:v7:hts:v8:v9[:icc_avg:icc_peak], default = camfix9 matrix (8 captures,
#                 ~3 min), DBGFS=1; table /tmp/cam-<sensor>-sweep9.tab. The camfix8 matrix is MODE=sweep8.
#                 Detached-safe: nohup setsid ... > /tmp/c11-sweep.log 2>&1 & and poll for SWEEP_DONE.
#   camfix9b knob: STK=<mask> qcom_camss.a6l_stk (default 0) = stock msm_isp47/48 RDI behaviour: 1 RDI reg-update
#                 every frame (re-requested at each RDI REG_UPDATE irq), 2 also at each RDI SOF when none pending,
#                 4 stock bus-error handling (RDI WM 0xC94 ignored: no dump/clear/reload/recovery, IRQ_MASK_1 bit4),
#                 8 stock start order (reg-update, WM reload, stock RDI UB 192 words), 16 re-program the finished
#                 ping/pong address at every WM done. Logs A6L_STK START/STOP (rup_irq rup_req sof_req be_ign pp).
#   MODE=sweep (camfix9b): entries name:v6:v7:hts:v8:v9:stk[:icc_avg:icc_peak]; default = 7 stock-RDI variants
#                 first, then the 8 camfix9 VBIF variants (15 captures, ~3.5 min); table adds stk/rup/be_ign columns.
#   camfix10 knobs: V10=<mask> qcom_camss.a6l_v10 (default 0x01): 1 diagnostics (A6L_V10 IOVA per-page iova->phys vs
#                 CPU page, A6L_V10 F per-frame VFE/WM0/CSID/CSIPHY/SMMU poll, A6L_V10 SUM, A6L_V10 HIST/BARS written
#                 data, A6L_V10 SG), 2 CSID test generator instead of the sensor, 4 fixed slots FORCE_CONTIGUOUS,
#                 8 stock IRQ_MASK_1 (all error bits), 16 fill 0xdeadbeef + slot snapshot at the first bus error,
#                 32 TG 1280x720 max blanking, 64 TG max blanking. TPG=<1..5> qcom_camss.a6l_v10_tpg (1 incrementing,
#                 2 0x55/0xAA, 3 zeros, 4 ones, 5 random)
#   MODE=sweep (camfix10): entries name:v10:tpg:stk (v6=128 probe, v7=1, v8=1, v9=1, WM=6), default 9 captures
#                 (~2.5 min), DBGFS=1, table /tmp/cam-<sensor>-sweep10.tab. camfix9b matrix = MODE=sweep9.
#                 Detached-safe: nohup setsid ... > /tmp/c12-sweep.log 2>&1 & and poll for SWEEP_DONE.
#   camfix11 knobs (sensor -> CSIPHY -> CSID RX): V11=<mask> qcom_camss.a6l_v11 (default 0x01): 1 CSID packet
#                 diagnostics (captured long/short/unmapped headers, ECC/CRC/total stats: A6L_V11 P/SUM/HDR),
#                 2 CSID CORE_CTRL_1 0xF (stock), 4 map DT2 on CID1 (disabled), 8 stock CSID IRQ mask + per-bit event
#                 counts (capped at 4000), 16 map DT2 on CID1 enabled like CID0 (stock imx576 LUT).
#                 SETTLE=<n> a6l_v11_settle (0 formula; stock imx576 14, s5k3t1 19), PHY7=<n> a6l_v11_ctrl7 (-1 mainline
#                 0x02, -2 not written, 0..255), DT0=<dt> a6l_v11_dt0, DT2=<dt> a6l_v11_dt2 (0x36), LASSIGN=<hex>
#                 a6l_v11_lassign, SREGS="addr=val,..." sensor a6l_regs (imx576_a6l / s5k3t1), SRD=0/1 sensor a6l_rd.
#   MODE=sweep (camfix11): entries name:sensor(i|s):v10:v11:settle:ctrl7:tpg, default 13 captures (~3.5 min), both
#                 sensors in one run, table /tmp/cam-sweep11.tab, sum /tmp/cam-sweep11.sum. camfix10 matrix = MODE=sweep10.
#                 Detached-safe: nohup setsid ... > /tmp/c13-sweep.log 2>&1 & and poll for SWEEP_DONE.
#   camfix12 knobs: V12=<mask> qcom_camss.a6l_v12 (default 0x13): 1 diagnostics (CSIPHY clock tree + MMCC raw,
#                 clk_mux + lane readback, CSID MISR + STATS_ECC hi/lo: A6L_V12 ...), 2 stock CSIPHY clocks
#                 (cphy_csidK -> csiphyK -> csiphy_clk_src at PHYCLK, csiK at CSICLK), 4 stock CSID rails
#                 (vdda 1.2 V, vdd_sec 0.925 V), 8 TG isolation (CORE_CTRL_0/1 = 0 with the test generator),
#                 16 clear stale CSID/CSIPHY poll pointers at VFE stop. PHYCLK=<Hz> a6l_v12_phyclk (200000000),
#                 CSICLK=<Hz> a6l_v12_csiclk (310000000).
#   MODE=sweep (camfix12): entries name:sensor(i|s):v11:settle:ctrl7:v12[:phyclk], v10=1, no test-generator row
#                 (the t34 TG row after 12 sensor rows rebooted the phone), default 12 captures (~3.3 min), table
#                 /tmp/cam-sweep12.tab, sum /tmp/cam-sweep12.sum; each row also writes A6L_SWEEP_BEGIN to /dev/kmsg.
#                 camfix11 matrix = MODE=sweep11. MODE=sweeptpg = the TG row alone (fresh boot, before any sensor).
#   camfix11: dmesg slicing uses a unique /dev/kmsg marker (A6L_MARK_...) so relayed lines (A6L_SW ...) that carry
#                 old timestamps can no longer shift the per-capture .dmesg/.out (t33 sweep10 table was shifted).
#   A kernel oops in an earlier run blocks bars/live (reboot first): the camss state is then undefined.
# Markers: "A6L_CAM <STEP>_PASS/_FAIL". Nothing here writes flash/eMMC.
export PATH=/tmp/bin:$PATH
D=${D:-/tmp/camera}; MODE=${MODE:-probe}; SENSOR=${SENSOR:-imx576}
say() { echo "A6L_CAM $*"; }
chmod 755 $D/a6l_camcap 2>/dev/null
# CSID choice: sdm660 has 4 CSIDs but 2 VFEs; with the camfix camss any CSID works (ISPIF routes to VFE0),
# the defaults stay on CSID0/1 so an unpatched camss would still power up.
setsensor() {
case "$SENSOR" in
imx576) PHY=${PHY:-0}; CSID=${CSID:-0}; W=${W:-2880}; H=${H:-2156}; BAYER=RGGB; EXP=${EXP:-2000}; GAIN=${GAIN:-512};;
hi846)  PHY=${PHY:-1}; CSID=${CSID:-0}; W=${W:-1632}; H=${H:-1224}; BAYER=GBRG; EXP=${EXP:-2000}; GAIN=${GAIN:-64};;
s5k3t1) PHY=${PHY:-2}; CSID=${CSID:-1}; W=${W:-2304}; H=${H:-1728}; BAYER=GRBG; EXP=${EXP:-1500}; GAIN=${GAIN:-128};;
*) say "unknown SENSOR=$SENSOR"; exit 2;;
esac
}
setsensor

check() {   # toybox sha256sum has no --ignore-missing: verify line by line
  bad=0; n=0
  while read sum name; do
    [ -n "$name" ] || continue
    if [ ! -f "$D/$name" ]; then say "SHA_MISSING $name"; bad=1; continue; fi
    got=$(sha256sum "$D/$name" | cut -d' ' -f1)
    if [ "$got" = "$sum" ]; then n=$((n+1)); else say "SHA_BAD $name"; bad=1; fi
  done < $D/SHA256SUMS
  [ $bad = 0 ] && say "SHA_PASS $n files" || say "SHA_FAIL"
}

mknodes() {  # recovery has no ueventd: create /dev nodes from sysfs
  for p in /sys/class/video4linux/* /sys/bus/media/devices/*; do
    [ -f $p/dev ] || continue
    n=${p##*/}; mm=$(cat $p/dev)
    [ -c /dev/$n ] || mknod /dev/$n c ${mm%%:*} ${mm##*:}
  done
  ls /dev/media* /dev/video* /dev/v4l-subdev* 2>/dev/null | tr '\n' ' '; echo
}

load() {
  out=$(insmod $D/extra/a6l_cam_ovl.ko 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod a6l_cam_ovl: $out";; esac
  for m in $(cat $D/load-order.txt); do
    out=$(insmod $D/$m 2>&1) || case "$out" in *"File exists"*) ;; *) say "insmod $m: $out";; esac
  done
  sleep 3
  mknodes
}

# camfix5: the kernel ring buffer is full (wraps), so a line count is useless as a mark (t27: every .dmesg was empty).
# Mark = timestamp prefix of the last line; since = lines after that line (whole buffer if it rolled out).
# camfix11: unique marker line written to /dev/kmsg (anchored match: a relayed "A6L_SW ..." copy never matches).
MKN=0
mark() {
  MKN=$((MKN+1)); MK="A6L_MARK_$$_$(date +%s)_$MKN"
  if echo "$MK" > /dev/kmsg 2>/dev/null; then echo "$MK" > /tmp/cam-dmesg-mark; else dmesg | tail -n 1 | cut -d']' -f1 > /tmp/cam-dmesg-mark; fi
}
since() {
  m=$(cat /tmp/cam-dmesg-mark 2>/dev/null); dmesg > /tmp/cam-dmesg-now
  n=""
  case "$m" in
  A6L_MARK_*) n=$(grep -n "^\[[ 0-9.]*\] $m\$" /tmp/cam-dmesg-now | tail -n 1 | cut -d: -f1);;
  ?*) n=$(grep -n -F "$m]" /tmp/cam-dmesg-now | tail -n 1 | cut -d: -f1);;
  esac
  if [ -n "$n" ]; then tail -n +$((n+1)) /tmp/cam-dmesg-now; else cat /tmp/cam-dmesg-now; fi
}

capture() {  # uses MODE (bars|live), TAG (suffix)
  mknodes
  if [ "$MODE" = bars ]; then TP="-t 2"; else TP="-t 0 -e $EXP -g $GAIN"; fi
  OUT=/tmp/cam-$SENSOR-$MODE$TAG.raw; rm -f $OUT
  if dmesg | grep -qE "Internal error: Oops|Unable to handle kernel"; then
    say "PREVIOUS_OOPS_IN_DMESG: reboot the phone (fresh V74 recovery) before capturing"; return 3
  fi
  P=/sys/module/qcom_camss/parameters
  [ -n "$DBG" ] && echo $DBG > $P/a6l_dbg
  [ -n "$VFE_MIN" ] && echo $VFE_MIN > $P/a6l_vfe_min
  [ -n "$CSID_IRQMASK" ] && echo $((CSID_IRQMASK)) > $P/a6l_csid_irqmask
  [ -n "$WM" ] && echo $WM > $P/a6l_wm
  [ -n "$WMMAX" ] && echo $WMMAX > $P/a6l_wmmax
  [ -n "$CGC" ] && echo $CGC > $P/a6l_cgc
  [ -n "$FDUMP" ] && echo $FDUMP > $P/a6l_fdump
  [ -n "$FDUMP_WM" ] && echo $FDUMP_WM > $P/a6l_fdump_wm
  [ -n "$UB" ] && echo $UB > $P/a6l_ub
  [ -n "$V6" ] && echo $((V6)) > $P/a6l_v6
  [ -n "$ICC_AVG" ] && echo $ICC_AVG > $P/a6l_icc_avg
  [ -n "$ICC_PEAK" ] && echo $ICC_PEAK > $P/a6l_icc_peak
  [ -n "$PDUMP" ] && echo $PDUMP > $P/a6l_pdump
  [ -n "$V7" ] && echo $((V7)) > $P/a6l_v7
  [ -n "$V8" ] && echo $((V8)) > $P/a6l_v8
  [ -n "$V9" ] && echo $((V9)) > $P/a6l_v9
  [ -n "$STK" ] && echo $((STK)) > $P/a6l_stk
  [ -n "$V10" ] && echo $((V10)) > $P/a6l_v10
  [ -n "$TPG" ] && echo $((TPG)) > $P/a6l_v10_tpg
  [ -n "$V11" ] && echo $((V11)) > $P/a6l_v11
  [ -n "$SETTLE" ] && echo $((SETTLE)) > $P/a6l_v11_settle
  [ -n "$PHY7" ] && echo $((PHY7)) > $P/a6l_v11_ctrl7
  [ -n "$DT0" ] && echo $((DT0)) > $P/a6l_v11_dt0
  [ -n "$DT2" ] && echo $((DT2)) > $P/a6l_v11_dt2
  [ -n "$LASSIGN" ] && echo $((LASSIGN)) > $P/a6l_v11_lassign
  [ -n "$V12" ] && echo $((V12)) > $P/a6l_v12
  [ -n "$PHYCLK" ] && echo $((PHYCLK)) > $P/a6l_v12_phyclk
  [ -n "$CSICLK" ] && echo $((CSICLK)) > $P/a6l_v12_csiclk
  SP=/sys/module/imx576_a6l/parameters; [ "$SENSOR" = s5k3t1 ] && SP=/sys/module/s5k3t1/parameters
  [ -n "$SREGS" ] && echo "$SREGS" > $SP/a6l_regs
  [ -n "$SRD" ] && echo $SRD > $SP/a6l_rd
  [ -n "$HTS" ] && echo $HTS > /sys/module/imx576_a6l/parameters/a6l_hts
  for f in a6l_wm a6l_wmmax a6l_cgc a6l_dbg a6l_vfe_min a6l_fdump a6l_fdump_wm a6l_ub a6l_v6 a6l_icc_avg a6l_icc_peak a6l_pdump a6l_v7 a6l_v8 a6l_v9 a6l_stk a6l_v10 a6l_v10_tpg a6l_v11 a6l_v11_settle a6l_v11_ctrl7 a6l_v11_dt0 a6l_v11_dt2 a6l_v11_lassign a6l_v12 a6l_v12_phyclk a6l_v12_csiclk a6l_csid_irqmask a6l_phy_fast a6l_csid_fast; do printf '%s=%s ' $f "$(cat $P/$f 2>/dev/null)"; done; echo "a6l_hts=$(cat /sys/module/imx576_a6l/parameters/a6l_hts 2>/dev/null) a6l_regs=$(cat $SP/a6l_regs 2>/dev/null)"
  TO=""; command -v timeout >/dev/null 2>&1 && TO="timeout 60"
  mark
  DBG_OUT=/tmp/cam-$SENSOR-$MODE$TAG.dbgfs; rm -f $DBG_OUT
  if [ "$DBGFS" = 1 ]; then  # camfix7: clock / power-domain / interconnect state while streaming
    grep -q " /sys/kernel/debug " /proc/mounts || mount -t debugfs none /sys/kernel/debug 2>/dev/null
    ( sleep 4; K=/sys/kernel/debug
      echo "== clk_summary"; grep -E "axi|mmpll|smmu|mnoc|vfe|vbif|throttle|camss_ahb|camss_top|micro|ahb_clk_src|bimc|mmssnoc" $K/clk/clk_summary
      echo "== pm_genpd_summary"; cat $K/pm_genpd/pm_genpd_summary
      echo "== interconnect_summary"; grep -E -A3 "mas_vfe|slv_mnoc_bimc|mas_mnoc_bimc|slv_ebi|^ *node" $K/interconnect/interconnect_summary | head -60
    ) > $DBG_OUT 2>&1 &
  fi
  $TO $D/a6l_camcap -s $SENSOR -p $PHY -c $CSID -W $W -H $H $TP -n 4 -o $OUT > /tmp/cam-$SENSOR-$MODE$TAG.log 2>&1
  echo "a6l_camcap rc=$?"
  cat /tmp/cam-$SENSOR-$MODE$TAG.log
  since > /tmp/cam-$SENSOR-$MODE$TAG.dmesg
  grep -E "A6L_|camss|csid|csiphy|ispif|vfe|VFE|$SENSOR|imx576|s5k3t1|hi846|smmu|Oops|Unable to handle" /tmp/cam-$SENSOR-$MODE$TAG.dmesg | tail -160
  grep -qE "Internal error: Oops|Unable to handle kernel" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "KERNEL_OOPS_FAIL (reboot before the next camera test; send /tmp/cam-$SENSOR-$MODE$TAG.dmesg)"
  grep -q "A6L_VFE0 ERR" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "VFE_ERR seen (bus overflow / violation, see A6L_VFE0 ERR)"
  grep -q "A6L_VFE0_STOP" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "DIAG $(grep -o 'A6L_VFE0_STOP.*' /tmp/cam-$SENSOR-$MODE$TAG.dmesg | tail -1)"
  grep -q "A6L_WM[0-9]*_STOP" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "WMDIAG $(grep -o 'A6L_WM[0-9]*_STOP.*' /tmp/cam-$SENSOR-$MODE$TAG.dmesg | tail -1)"
  grep -q "A6L_WM[0-9]*_START" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "WMSTART $(grep -o 'A6L_WM[0-9]*_START.*' /tmp/cam-$SENSOR-$MODE$TAG.dmesg | tail -1)"
  grep -o "A6L_WM4_S[A-Z]*.*" /tmp/cam-$SENSOR-$MODE$TAG.dmesg | while read l; do say "WM4 $l"; done
  say "DONES $(grep -c 'A6L_WM[0-9]* done#' /tmp/cam-$SENSOR-$MODE$TAG.dmesg) (first 8 logged)"
  grep -q "A6L_VFE0 vfe0 rate" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "RATE $(grep -o 'A6L_VFE0 vfe0 rate.*' /tmp/cam-$SENSOR-$MODE$TAG.dmesg | tail -1)"
  grep -q "A6L_VFE0_FRAMES" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "FRAMES $(grep -o 'A6L_VFE0_FRAMES.*' /tmp/cam-$SENSOR-$MODE$TAG.dmesg | tail -1)"
  grep -q "A6L_VFE0 BUSERR" /tmp/cam-$SENSOR-$MODE$TAG.dmesg && say "BUSERR $(grep -o 'A6L_VFE0 BUSERR.*' /tmp/cam-$SENSOR-$MODE$TAG.dmesg | head -2 | tr '\n' ' ')"
  F=/tmp/cam-$SENSOR-$MODE$TAG.dmesg
  grep -q "A6L_VFE0_V6" $F && say "V6 $(grep -o 'A6L_VFE0_V6.*' $F | tail -1)"
  grep -q "A6L_ICC" $F && say "ICC $(grep -o 'A6L_ICC.*' $F | tail -1)"
  grep -o "A6L_AXI.*" $F | head -2 | while read l; do say "AXI $l"; done
  grep -o "A6L_VBIF.*" $F | head -3 | while read l; do say "VBIF $l"; done
  grep -o "A6L_VFE0_RECOVER.*" $F | head -2 | while read l; do say "RECOVER $l"; done
  grep -o "A6L_WM4_P.*" $F | head -${WMP_LINES:-16} | while read l; do say "WMP $l"; done
  # camfix7
  grep -o "A6L_SNS_HTS.*" $F | tail -1 | while read l; do say "HTS $l"; done
  grep -o "A6L_MMCC [A-Z]* .*" $F | grep -E "A6L_MMCC (START|BUSERR|STOP) " | head -9 | while read l; do say "MMCC $l"; done
  grep -o "A6L_SMMU .*" $F | head -${SMMU_LINES:-16} | while read l; do say "SMMU $l"; done
  grep -o "A6L_V7CLK.*" $F | head -5 | while read l; do say "V7CLK $l"; done
  # camfix8
  grep -o "A6L_V8 .*" $F | grep -vE "A6L_V8 (START|STOP|BUSERR) " | head -12 | while read l; do say "V8 $l"; done
  grep -o "A6L_V8 [A-Z]* mmpll4 .*" $F | head -3 | while read l; do say "V8PLL $l"; done
  grep -iE "rcg didn.t update|clk.*stuck|WARNING:" $F | head -3 | while read l; do say "CLKWARN $l"; done
  # camfix9
  grep -o "A6L_V9 .*" $F | grep -vE "A6L_V9 VBIF " | head -14 | while read l; do say "V9 $l"; done
  grep -o "A6L_V9 VBIF [A-Z]* .*" $F | head -4 | while read l; do say "V9VBIF $l"; done
  # camfix9b
  grep -o "A6L_STK .*" $F | head -6 | while read l; do say "STK $l"; done
  # camfix10
  grep -o "A6L_V10 .*" $F | grep -vE "A6L_V10 (F|SNAP) " | head -${V10_LINES:-30} | while read l; do say "V10 $l"; done
  grep -o "A6L_V10 F .*" $F | head -${V10F_LINES:-10} | while read l; do say "V10F $l"; done
  grep -o "A6L_V10 SNAP .*" $F | head -4 | while read l; do say "V10SNAP $l"; done
  # camfix11
  grep -o "A6L_V11 .*" $F | grep -v "A6L_V11 P " | head -${V11_LINES:-40} | while read l; do say "V11 $l"; done
  grep -o "A6L_V11 P .*" $F | head -${V11P_LINES:-10} | while read l; do say "V11P $l"; done
  grep -o "A6L_SNS_\(RD\|REGS\) .*" $F | head -8 | while read l; do say "SNS $l"; done
  grep -o "A6L_CSIPHY[0-9] link_freq.*" $F | tail -1 | while read l; do say "PHYCFG $l"; done
  # camfix12
  grep -o "A6L_V12 .*" $F | grep -vE "A6L_V12 (P |CSIPHY[0-9] STOP LN)" | head -${V12_LINES:-30} | while read l; do say "V12 $l"; done
  grep -o "A6L_V12 P .*" $F | head -8 | while read l; do say "V12P $l"; done
  grep -o "A6L_V12 CSIPHY[0-9] STOP LN.*" $F | head -8 | while read l; do say "V12STOP $l"; done
  grep -iE "Unhandled context fault|Unexpected global fault|arm-smmu.*fault" $F | head -3 | while read l; do say "SMMU_FAULT $l"; done
  [ -s "$DBG_OUT" ] && { sleep 1; say "DBGFS $DBG_OUT"; grep -E "vfe|vbif|mmssnoc|axi_clk_src|bimc_smmu|mnoc_ahb|mas_vfe|slv_ebi|bimc|camss" $DBG_OUT | head -40 | sed 's/^/A6L_CAM DBGFS /'; }
  since | grep -q "SENSOR_STREAMING" && say "SENSOR_TX_PASS (frame counter moving)"
  since | grep -q "SENSOR_NOT_COUNTING" && say "SENSOR_TX_FAIL (frame counter static: sensor not streaming)"
  if [ -s $OUT ]; then
    BPL=$(grep -o "bpl [0-9]*" /tmp/cam-$SENSOR-$MODE$TAG.log | tail -1 | cut -d' ' -f2)
    say "CAPTURE_${SENSOR}_${MODE}${TAG}_PASS $OUT $(wc -c < $OUT) bytes bpl=$BPL"
    say "host: adb pull $OUT && python3 raw10_to_png.py $(basename $OUT) $W $H $BPL $BAYER cam-$SENSOR-$MODE$TAG.png"
  else
    say "CAPTURE_${SENSOR}_${MODE}${TAG}_FAIL"
  fi
}

case "$MODE" in
check) check ;;
load|probe)
  check
  load
  dmesg | grep -iE "A6L_CAM_OVL|cci|camss|csiphy|ispif|imx576|s5k3t1|hi846|gt9769|chip id|supply" | tail -40
  dmesg | grep -q "Sony IMX576 chip id 0x576" && say IMX576_PROBE_PASS || say IMX576_PROBE_FAIL
  dmesg | grep -q "Samsung S5K3T1 chip id 0x3141" && say S5K3T1_PROBE_PASS || say S5K3T1_PROBE_FAIL
  dmesg | grep -q "hi846.*chip id 08 46" && say HI846_PROBE_PASS || say HI846_PROBE_FAIL
  dmesg | grep -q "GT9769 VCM initialised" && say GT9769_PROBE_PASS || say GT9769_PROBE_FAIL
  [ -f /sys/module/qcom_camss/parameters/a6l_v6 ] && say CAMSS_CAMFIX6_PASS || say "CAMSS_CAMFIX6_FAIL (old qcom-camss loaded: reboot)"
  [ -f /sys/module/qcom_camss/parameters/a6l_v7 ] && say CAMSS_CAMFIX7_PASS || say "CAMSS_CAMFIX7_FAIL (old qcom-camss loaded: reboot)"
  [ -f /sys/module/imx576_a6l/parameters/a6l_hts ] && say IMX576_HTS_PASS || say "IMX576_HTS_FAIL (old imx576_a6l loaded: reboot)"
  say "V7_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v7 2>/dev/null)"
  [ -f /sys/module/qcom_camss/parameters/a6l_v8 ] && say CAMSS_CAMFIX8_PASS || say "CAMSS_CAMFIX8_FAIL (old qcom-camss loaded: reboot)"
  say "V8_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v8 2>/dev/null)"
  [ -f /sys/module/qcom_camss/parameters/a6l_v9 ] && say CAMSS_CAMFIX9_PASS || say "CAMSS_CAMFIX9_FAIL (old qcom-camss loaded: reboot)"
  say "V9_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v9 2>/dev/null)"
  [ -f /sys/module/qcom_camss/parameters/a6l_stk ] && say CAMSS_CAMFIX9B_PASS || say "CAMSS_CAMFIX9B_FAIL (old qcom-camss loaded: reboot)"
  say "STK_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_stk 2>/dev/null)"
  [ -f /sys/module/qcom_camss/parameters/a6l_v10 ] && say CAMSS_CAMFIX10_PASS || say "CAMSS_CAMFIX10_FAIL (old qcom-camss loaded: reboot)"
  say "V10_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v10 2>/dev/null) TPG_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v10_tpg 2>/dev/null)"
  [ -f /sys/module/qcom_camss/parameters/a6l_v11 ] && say CAMSS_CAMFIX11_PASS || say "CAMSS_CAMFIX11_FAIL (old qcom-camss loaded: reboot)"
  say "V11_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v11 2>/dev/null) SETTLE_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v11_settle 2>/dev/null) CTRL7_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v11_ctrl7 2>/dev/null) DT2_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v11_dt2 2>/dev/null)"
  [ -f /sys/module/qcom_camss/parameters/a6l_v12 ] && say CAMSS_CAMFIX12_PASS || say "CAMSS_CAMFIX12_FAIL (old qcom-camss loaded: reboot)"
  say "V12_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v12 2>/dev/null) PHYCLK_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v12_phyclk 2>/dev/null) CSICLK_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v12_csiclk 2>/dev/null)"
  [ -f /sys/module/imx576_a6l/parameters/a6l_regs ] && [ -f /sys/module/s5k3t1/parameters/a6l_regs ] && say SENSOR_CAMFIX11_PASS || say "SENSOR_CAMFIX11_FAIL (old imx576_a6l/s5k3t1 loaded: reboot)"
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  say "V6_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v6 2>/dev/null)"
  say "VFE_MIN_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_vfe_min 2>/dev/null)"
  dmesg | grep -q "supply vdda not found" && say CSID_RAILS_FAIL_dummy || say CSID_RAILS_PASS
  $D/a6l_camcap -l | grep -q "msm_ispif0" && say ISPIF_IN_GRAPH_PASS || say ISPIF_IN_GRAPH_FAIL
  say PROBE_DONE
  ;;
list) mknodes; $D/a6l_camcap -l; say LIST_DONE ;;
bars|live) capture ;;
wmloop)
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-wmloop.sum; : > $SUM
  for e in ${WMS:-4 3 0 4:0:1 4:1:0}; do
    WM=${e%%:*}; r=${e#$WM}; r=${r#:}; WMMAX=${r%%:*}; r=${r#$WMMAX}; CGC=${r#:}
    WMMAX=${WMMAX:-1}; CGC=${CGC:-1}; TAG=-wm$WM-m$WMMAX-c$CGC
    say "WMLOOP_BEGIN $TAG"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|WM4|WMDIAG|BUSERR|DIAG|KERNEL_OOPS|WMSTART)' /tmp/cam-$SENSOR-bars$TAG.out | sed "s/^/$TAG /" >> $SUM
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL /tmp/cam-$SENSOR-bars$TAG.out && break; sleep 1
  done
  echo 1 > $P/a6l_wmmax; echo 1 > $P/a6l_cgc; echo ${WM_AFTER:-3} > $P/a6l_wm  # back to the defaults
  say WMLOOP_SUMMARY; cat $SUM; say "WMLOOP_DONE (pull /tmp/cam-$SENSOR-bars-wm*.raw + .dmesg)"
  ;;
clkloop)
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-clkloop.sum; : > $SUM; WM=${WM:-3}
  for v in ${VFES:-120000000 300000000 404000000 480000000}; do
    VFE_MIN=$v; TAG=-vfe$((v/1000000))-wm$WM
    say "CLKLOOP_BEGIN $TAG"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|RATE|WM4|WMDIAG|BUSERR|KERNEL_OOPS)' /tmp/cam-$SENSOR-bars$TAG.out | sed "s/^/$TAG /" >> $SUM
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL /tmp/cam-$SENSOR-bars$TAG.out && break; sleep 1
  done
  echo ${VFE_AFTER:-404000000} > $P/a6l_vfe_min; echo 0 > $P/a6l_fdump  # back to the defaults
  say CLKLOOP_SUMMARY; cat $SUM; say "CLKLOOP_DONE (pull /tmp/cam-$SENSOR-bars-vfe*.raw + .dmesg)"
  ;;
sweep|sweep12|sweeptpg)
  # camfix12: CSIPHY clocks / rails. Entry = name:sensor(i=imx576,s=s5k3t1):v11:settle:ctrl7:v12[:phyclk]
  # (v10=1 diagnostics, v6=128 progress probe, v7=1, v8=1, v9=1, stk 0, WM=6, DBGFS=0, never the test generator).
  # MODE=sweeptpg: the TG row alone (v10=3, v12=0x1b = TG isolation), only as the first camera stream after a boot.
  SMODE=$MODE; mknodes; MODE=bars; SUM=/tmp/cam-sweep12.sum; TAB=/tmp/cam-sweep12.tab; : > $SUM; WM=${WM:-6}; DBGFS=${DBGFS:-0}
  P=/sys/module/qcom_camss/parameters
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  [ -f $P/a6l_v12 ] || { say "SWEEP_ABORT qcom-camss without a6l_v12 loaded (reboot, MODE=probe with camera14)"; exit 3; }
  DEFV="iclk:i:1:0:-1:19 sclk:s:1:0:-1:19 inoclk:i:1:0:-1:17 istk:i:19:14:-1:19 iclk269:i:1:0:-1:19:269333333 ivreg:i:1:0:-1:23 svreg:s:1:0:-1:23 iclk14:i:1:14:-1:19 iclkirq:i:9:0:-1:19 sstk:s:11:19:-1:19 snoclk:s:1:0:-1:17 iclk100:i:1:0:-1:19:100000000"
  [ "$SMODE" = sweeptpg ] && DEFV="itpg:i:1:0:-1:27"
  printf '%-8s %-6s %-3s %-3s %-3s %-4s %-10s %-18s %-5s %-4s %-24s %-14s %-9s %-22s %-40s %s\n' variant sensor v11 set ct7 v12 phyclk sof/done/be/first dones maxl pkts_tot/ecc/crc ecc_hi%/lo% pkts/frm csid_bits_err phy_clk_after capture > $TAB
  for e in ${VARS:-$DEFV}; do
    vn=${e%%:*}; r=${e#*:}; sn=${r%%:*}; r=${r#*:}; x11=${r%%:*}; r=${r#*:}; st=${r%%:*}; r=${r#*:}; c7=${r%%:*}; r=${r#*:}; x12=${r%%:*}
    pc=200000000; case "$r" in *:*) pc=${r#*:};; esac
    case $sn in s) SENSOR=s5k3t1;; *) SENSOR=imx576;; esac
    PHY=; CSID=; W=; H=; EXP=; GAIN=; setsensor; LB=$(( W * 5 / 4 ))
    V6=128; V7=1; HTS=0; V8=1; V9=1; STK=0; V10=1; TPG=1; [ "$SMODE" = sweeptpg ] && V10=3
    V11=$x11; SETTLE=$st; PHY7=$c7; DT0=${DT0:-0}; DT2=${DT2:-54}; LASSIGN=${LASSIGN:-0}; V12=$x12; PHYCLK=$pc; CSICLK=${CSICLK:-310000000}
    ICC_AVG=2000000; ICC_PEAK=4000000; PDUMP=${PDUMP:-40}; TAG=-sw12-$vn
    say "SWEEP_BEGIN $TAG sensor=$SENSOR v10=$V10 v11=$V11 settle=$SETTLE ctrl7=$PHY7 v12=$V12 phyclk=$PHYCLK wm=$WM $(date +%T)"
    echo "A6L_SWEEP_BEGIN $TAG sensor=$SENSOR v10=$V10 v11=$V11 settle=$SETTLE ctrl7=$PHY7 v12=$V12 phyclk=$PHYCLK" > /dev/kmsg 2>/dev/null
    capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|WM4|BUSERR|KERNEL_OOPS|SMMU_FAULT|V10 A6L_V10 (SUM|HIST|TPG)|V11|V11P|V12|V12P|SNS|PHYCFG)' $O | sed "s/^/$TAG /" >> $SUM
    fr=$(grep -o 'A6L_VFE0_FRAMES.*' $O | tail -1 | sed -n 's/.*sof \([0-9]*\) done \([0-9]*\) buserr \([0-9]*\) first_buserr_sof \([0-9]*\).*/\1\/\2\/\3\/\4/p')
    dn=$(grep -o 'DONES [0-9]*' $O | tail -1 | cut -d' ' -f2)
    mx=0; for x in $(grep -o 'A6L_WM4_P .*' $O | sed 's/.*slot0 head [01] lines \([0-9]*\) slot1 head [01] lines \([0-9]*\).*/\1 \2/'); do [ "$x" -gt "$mx" ] && mx=$x; done
    pk=$(grep -o 'A6L_V11 SUM .*' $O | tail -1 | sed -n 's/.*(+\([0-9]*\)) ecc .*(+\([0-9]*\)) crc .*(+\([0-9]*\)) v11.*/\1\/\2\/\3/p')
    eh=$(grep -o 'A6L_V12 SUM .*' $O | tail -1 | sed -n 's/.*hi% \([0-9]*\) lo% \([0-9]*\).*/\1\/\2/p')
    pf=$(grep -o 'A6L_V12 SUM .*' $O | tail -1 | sed -n 's/.*pkts +[0-9]* (\([0-9]*\)\/frame).*/\1/p')
    cb=$(grep -o 'A6L_V10 SUM sof .*' $O | tail -1 | sed 's/.*csid_bits //' | tr ' ' '\n' | grep -vE '^b[0-7]:' | tr '\n' ',' | cut -c1-22)
    ck=$(grep -o 'A6L_V12 CSIPHY[0-9] CLK ON .*' $O | head -1 | sed -n 's/.*| parent \([a-z0-9_]*\) \([0-9]*\) en \([-0-9]*\) | gparent [a-z0-9_]* \([0-9]*\).*/\1=\2,en\3,src\4/p')
    [ -z "$ck" ] && ck=$(grep -o 'A6L_V12 CSIPHY[0-9] CLK PRE .*' $O | head -1 | sed -n 's/.*| parent \([a-z0-9_]*\) \([0-9]*\) en \([-0-9]*\) | gparent [a-z0-9_]* \([0-9]*\).*/pre:\1=\2,en\3,src\4/p')
    cap=FAIL; grep -q "CAPTURE_.*_PASS" $O && cap=PASS
    printf '%-8s %-6s %-3s %-3s %-3s %-4s %-10s %-18s %-5s %-4s %-24s %-14s %-9s %-22s %-40s %s\n' $vn $SENSOR $V11 $SETTLE $PHY7 $V12 $PHYCLK ${fr:-?} ${dn:-?} $mx ${pk:--} ${eh:--} ${pf:--} ${cb:--} ${ck:--} $cap >> $TAB
    echo "A6L_SWEEP_END $TAG capture=$cap sof/done/be/first=${fr:-?} maxl=$mx pkts=${pk:--} ecc_hi/lo%=${eh:--}" > /dev/kmsg 2>/dev/null
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V10_AFTER:-1} > $P/a6l_v10; echo 1 > $P/a6l_v10_tpg; echo ${V6_AFTER:-15} > $P/a6l_v6; echo 0 > $P/a6l_stk
  echo 1 > $P/a6l_v11; echo 0 > $P/a6l_v11_settle; echo -1 > $P/a6l_v11_ctrl7; echo 0 > $P/a6l_v11_dt0; echo 54 > $P/a6l_v11_dt2; echo 0 > $P/a6l_v11_lassign
  echo $((0x13)) > $P/a6l_v12; echo 200000000 > $P/a6l_v12_phyclk; echo 310000000 > $P/a6l_v12_csiclk
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (imx576 frame 2156 lines, s5k3t1 1728; pkts = STATS total/ecc/crc delta; ecc_hi%/lo% = STATS_ECC halves as % of packets (t34: 95/5 imx576, 77/23 s5k3t1); pkts/frm expected ~2158 imx576 / ~1730 s5k3t1; phy_clk_after = csiphyK branch rate/enabled + csiphy_clk_src rate)"; cat $TAB
  say "SWEEP_DONE $(date +%T) (pull /tmp/cam-*-bars-sw12-*.dmesg .out .log + /tmp/cam-sweep12.sum/.tab)"
  echo "A6L_SWEEP_DONE" > /dev/kmsg 2>/dev/null
  ;;
sweep11)
  # camfix11: sensor -> CSIPHY -> CSID RX. Entry = name:sensor(i=imx576,s=s5k3t1):v10:v11:settle:ctrl7:tpg
  # (v6=128 progress probe, v7=1, v8=1, v9=1, stk 0, WM=6, DBGFS=0). Every entry sets all v11 knobs explicitly.
  mknodes; MODE=bars; SUM=/tmp/cam-sweep11.sum; TAB=/tmp/cam-sweep11.tab; : > $SUM; WM=${WM:-6}; DBGFS=${DBGFS:-0}
  P=/sys/module/qcom_camss/parameters
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  [ -f $P/a6l_v11 ] || { say "SWEEP_ABORT qcom-camss without a6l_v11 loaded (reboot, MODE=probe with camera13)"; exit 3; }
  printf '%-9s %-6s %-3s %-3s %-3s %-3s %-3s %-18s %-5s %-4s %-26s %-22s %-40s %-40s %s\n' variant sensor v10 v11 set ct7 tpg sof/done/be/first dones maxl pkts_tot/ecc/crc csid_bits_err long_hdr_top unmapped_top capture > $TAB
  for e in ${VARS:-idiag:i:1:1:0:-1:0 istock:i:1:19:14:-1:0 iirq:i:1:9:0:-1:0 sdiag:s:1:1:0:-1:0 sstock:s:1:11:19:-1:0 ictl1f:i:1:3:0:-1:0 idt36:i:1:5:0:-1:0 iset14:i:1:1:14:-1:0 ip7skip:i:1:1:0:-2:0 ip7zero:i:1:1:0:0:0 iset28:i:1:1:28:-1:0 iset10:i:1:1:10:-1:0 itpg:i:3:1:0:-1:1}; do
    vn=${e%%:*}; r=${e#*:}; sn=${r%%:*}; r=${r#*:}; x10=${r%%:*}; r=${r#*:}; x11=${r%%:*}; r=${r#*:}; st=${r%%:*}; r=${r#*:}; c7=${r%%:*}; tp=${r#*:}
    case $sn in s) SENSOR=s5k3t1;; *) SENSOR=imx576;; esac
    PHY=; CSID=; W=; H=; EXP=; GAIN=; setsensor; LB=$(( W * 5 / 4 ))
    V6=128; V7=1; HTS=0; V8=1; V9=1; STK=0; V10=$x10; TPG=${tp:-1}; [ "$TPG" = 0 ] && TPG=1
    V11=$x11; SETTLE=$st; PHY7=$c7; DT0=${DT0:-0}; DT2=${DT2:-54}; LASSIGN=${LASSIGN:-0}
    ICC_AVG=2000000; ICC_PEAK=4000000; PDUMP=${PDUMP:-40}; TAG=-sw11-$vn
    say "SWEEP_BEGIN $TAG sensor=$SENSOR v10=$V10 v11=$V11 settle=$SETTLE ctrl7=$PHY7 tpg=$TPG wm=$WM $(date +%T)"
    capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|WM4|BUSERR|KERNEL_OOPS|SMMU_FAULT|V10 A6L_V10 (SUM|HIST|TPG)|V11|V11P|SNS|PHYCFG)' $O | sed "s/^/$TAG /" >> $SUM
    fr=$(grep -o 'A6L_VFE0_FRAMES.*' $O | tail -1 | sed -n 's/.*sof \([0-9]*\) done \([0-9]*\) buserr \([0-9]*\) first_buserr_sof \([0-9]*\).*/\1\/\2\/\3\/\4/p')
    dn=$(grep -o 'DONES [0-9]*' $O | tail -1 | cut -d' ' -f2)
    mx=0; for x in $(grep -o 'A6L_WM4_P .*' $O | sed 's/.*slot0 head [01] lines \([0-9]*\) slot1 head [01] lines \([0-9]*\).*/\1 \2/'); do [ "$x" -gt "$mx" ] && mx=$x; done
    pk=$(grep -o 'A6L_V11 SUM .*' $O | tail -1 | sed -n 's/.*(+\([0-9]*\)) ecc .*(+\([0-9]*\)) crc .*(+\([0-9]*\)) v11.*/\1\/\2\/\3/p')
    cb=$(grep -o 'A6L_V10 SUM sof .*' $O | tail -1 | sed 's/.*csid_bits //' | tr ' ' '\n' | grep -vE '^b[0-7]:' | tr '\n' ',' | cut -c1-22)
    lh=$(grep -o 'A6L_V11 HDR long [0-9a-f]* x[0-9]* | A(di,wc,ecc): dt 0x[0-9a-f]* vc [0-9] wc [0-9]*' $O | head -1 | sed 's/A6L_V11 HDR long \([0-9a-f]*\) \(x[0-9]*\) .*dt \(0x[0-9a-f]*\) vc \([0-9]\) wc \([0-9]*\)/\1\2:dt\3,wc\5/')
    uh=$(grep -o 'A6L_V11 HDR unmapped [0-9a-f]* x[0-9]* | A(di,wc,ecc): dt 0x[0-9a-f]* vc [0-9] wc [0-9]*' $O | head -1 | sed 's/A6L_V11 HDR unmapped \([0-9a-f]*\) \(x[0-9]*\) .*dt \(0x[0-9a-f]*\) vc \([0-9]\) wc \([0-9]*\)/\1\2:dt\3,wc\5/')
    cap=FAIL; grep -q "CAPTURE_.*_PASS" $O && cap=PASS
    printf '%-9s %-6s %-3s %-3s %-3s %-3s %-3s %-18s %-5s %-4s %-26s %-22s %-40s %-40s %s\n' $vn $SENSOR $V10 $V11 $SETTLE $PHY7 ${tp:-0} ${fr:-?} ${dn:-?} $mx ${pk:--} ${cb:--} ${lh:--} ${uh:--} $cap >> $TAB
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V10_AFTER:-1} > $P/a6l_v10; echo 1 > $P/a6l_v10_tpg; echo ${V6_AFTER:-15} > $P/a6l_v6; echo 0 > $P/a6l_stk
  echo 1 > $P/a6l_v11; echo 0 > $P/a6l_v11_settle; echo -1 > $P/a6l_v11_ctrl7; echo 0 > $P/a6l_v11_dt0; echo 54 > $P/a6l_v11_dt2; echo 0 > $P/a6l_v11_lassign
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (imx576 frame 2156 lines, s5k3t1 1728; pkts = STATS total/ecc/crc delta over the stream; csid_bits_err = V10 per-frame status bits other than b0-7 SOT/EOT; long/unmapped hdr = most frequent captured header, decode A)"; cat $TAB
  say "SWEEP_DONE $(date +%T) (pull /tmp/cam-*-bars-sw11-*.dmesg .out .log + /tmp/cam-sweep11.sum/.tab)"
  ;;
sweep10)
  # camfix10: CSID test generator split (sensor/CSIPHY/CSID-RX vs ISPIF/VFE/WM/SMMU/DDR) + one-run diagnostics.
  # Entry = name:v10:tpg:stk. Nothing persists between entries (all v10 changes are undone at stream-off).
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-sweep10.sum; TAB=/tmp/cam-$SENSOR-sweep10.tab; : > $SUM; WM=${WM:-6}; DBGFS=${DBGFS:-1}
  P=/sys/module/qcom_camss/parameters; LB=$(( ${W:-2880} * 5 / 4 ))
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  printf '%-9s %-4s %-3s %-3s %-18s %-5s %-11s %-4s %-10s %-24s %-8s %-9s %-14s %-22s %s\n' variant v10 tpg stk sof/done/be/first dones scan_s0/s1 maxl iova0/1 csid_bits phy_or0 fsr_or hist0_ff/oth xirq capture > $TAB
  for e in ${VARS:-tpg:3:1:0 tpgsmall:35:1:0 diag:25:0:0 contig:5:0:0 tpgslow:67:1:0 tpgstk:3:1:5 tpgcontig:7:1:0 tpgaa:11:2:0 diagstk:9:0:5}; do
    vn=${e%%:*}; r=${e#*:}; x10=${r%%:*}; r=${r#*:}; tp=${r%%:*}; stk=${r#*:}
    V6=128; V7=1; HTS=0; V8=1; V9=1; STK=$stk; V10=$x10; TPG=$tp; ICC_AVG=2000000; ICC_PEAK=4000000; PDUMP=${PDUMP:-40}; TAG=-sw10-$vn
    say "SWEEP_BEGIN $TAG v10=$V10 tpg=$TPG stk=$STK wm=$WM $(date +%T)"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|RATE|WM4|WMDIAG|BUSERR|KERNEL_OOPS|SMMU_FAULT|STK|V10|V10F|V10SNAP)' $O | sed "s/^/$TAG /" >> $SUM
    fr=$(grep -o 'A6L_VFE0_FRAMES.*' $O | tail -1 | sed -n 's/.*sof \([0-9]*\) done \([0-9]*\) buserr \([0-9]*\) first_buserr_sof \([0-9]*\).*/\1\/\2\/\3\/\4/p')
    dn=$(grep -o 'DONES [0-9]*' $O | tail -1 | cut -d' ' -f2)
    e0=$(grep -o 'A6L_WM4_SCAN slot 0 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //'); e1=$(grep -o 'A6L_WM4_SCAN slot 1 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //')
    l0=$(( ${e0:-0} / LB )); l1=$(( ${e1:-0} / LB ))
    mx=0; for x in $(grep -o 'A6L_WM4_P .*' $O | sed 's/.*slot0 head [01] lines \([0-9]*\) slot1 head [01] lines \([0-9]*\).*/\1 \2/'); do [ "$x" -gt "$mx" ] && mx=$x; done
    i0=$(grep -o 'A6L_V10 IOVA START slot 0 .* \(OK\|BAD\)' $O | head -1 | sed 's/.* //'); i1=$(grep -o 'A6L_V10 IOVA START slot 1 .* \(OK\|BAD\)' $O | head -1 | sed 's/.* //')
    cb=$(grep -o 'A6L_V10 SUM sof .*' $O | tail -1 | sed 's/.*csid_bits //' | tr ' ' ',')
    po=$(grep -o 'A6L_V10 SUM phy_frames .*' $O | tail -1 | sed 's/.*phy_or \([0-9a-f ]*\) fsr_or.*/\1/' | tr -d ' ' | cut -c1-8)
    fo=$(grep -o 'A6L_V10 SUM phy_frames .* fsr_or [0-9a-f]*' $O | tail -1 | sed 's/.* //')
    hg=$(grep -o 'A6L_V10 HIST slot 0 .* ff [0-9]* zero [0-9]* fill [0-9]* other [0-9]*' $O | tail -1 | sed 's/.* ff \([0-9]*\) zero [0-9]* fill [0-9]* other \([0-9]*\)/\1\/\2/')
    xi=$(grep -o 'A6L_V10 SUM phy_frames .* xirq.*' $O | tail -1 | sed 's/.*xirq//' | tr ' ' ',' | cut -c1-22)
    cap=FAIL; grep -q "CAPTURE_.*_PASS" $O && cap=PASS
    printf '%-9s %-4s %-3s %-3s %-18s %-5s %-11s %-4s %-10s %-24s %-8s %-9s %-14s %-22s %s\n' $vn $V10 $TPG $STK ${fr:-?} ${dn:-?} $l0/$l1 $mx ${i0:--}/${i1:--} ${cb:--} ${po:--} ${fo:--} ${hg:--} ${xi:--} $cap >> $TAB
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V10_AFTER:-1} > $P/a6l_v10; echo 1 > $P/a6l_v10_tpg; echo ${V6_AFTER:-15} > $P/a6l_v6; echo ${STK_AFTER:-0} > $P/a6l_stk
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (frame = $(( ${H:-2156} )) lines; tpg>0 = CSID test generator; iova OK = every page iova->phys == CPU page; hist = 0xffffffff/other words in slot 0)"; cat $TAB
  say "SWEEP_DONE $(date +%T) (pull /tmp/cam-$SENSOR-bars-sw10-*.dmesg .out .log .dbgfs + sweep10.sum/.tab)"
  ;;
sweep9)
  # camfix9: is the camera VBIF (0xca40000) really unwritable, and does forcing its clocks / powering VFE1 change the
  # stream? Entry = name:v6:v7:hts:v8:v9[:icc_avg:icc_peak]. v9 bits 2/4/8 are undone at stream-off; bit 16 (stock
  # VBIF values) stays, so 'post' (diagnostics only) at the end shows what persisted.
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-sweep9.sum; TAB=/tmp/cam-$SENSOR-sweep9.tab; : > $SUM; WM=${WM:-6}; DBGFS=${DBGFS:-1}
  P=/sys/module/qcom_camss/parameters; LB=$(( ${W:-2880} * 5 / 4 ))
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  printf '%-9s %-4s %-3s %-3s %-3s %-3s %-18s %-5s %-11s %-9s %-11s %-4s %-9s %-5s %-8s %-8s %-8s %-8s %s\n' variant v6 v7 v8 v9 stk sof/done/be/first dones scan_s0/s1 maxl/frame rup_i/r/s be_i vbif_wr stick mask_ac mask_d0 clkon_st halt_be capture > $TAB
  # camfix9b: stock-RDI variants first (stk406 moves the MMSS AXI RCG to MMPLL0/2 until reboot, so rup406 and the
  # camfix9 VBIF variants after it run with that AXI source, as camfix8 post406 did), then the camfix9 matrix.
  for e in ${VARS:-srup:0:1:0:1:1:1 sign:0:1:0:1:1:4 srupign:0:1:0:1:1:5 srupsof:0:1:0:1:1:7 stock:1:1:0:1:1:31 stk406:1:1:0:7:1:31 rup406:0:1:0:1:1:5 base:0:1:0:1:1:0 clkon:0:1:0:1:3:0 fctl:0:1:0:1:5:0 clkvbif:1:1:0:1:19:0 vfe1:0:1:0:1:9:0 vfe1vbif:1:1:0:1:25:0 forceall:1:1:0:1:31:0 post:0:1:0:1:1:0}; do
    vn=${e%%:*}; r=${e#*:}; v=${r%%:*}; r=${r#*:}; v7=${r%%:*}; r=${r#*:}; h=${r%%:*}; r=${r#*:}; v8=${r%%:*}; r=${r#*:}; v9=${r%%:*}; r2=${r#$v9}; r2=${r2#:}
    ICC_AVG=2000000; ICC_PEAK=4000000; stk=0
    [ -n "$r2" ] && { stk=${r2%%:*}; r2=${r2#$stk}; r2=${r2#:}; }
    [ -n "$r2" ] && { ICC_AVG=${r2%%:*}; ICC_PEAK=${r2#*:}; }
    V6=$(( v | 128 )); V7=$v7; HTS=$h; V8=$v8; V9=$v9; STK=$stk; PDUMP=${PDUMP:-40}; TAG=-sw9-$vn
    say "SWEEP_BEGIN $TAG v6=$V6 v7=$V7 hts=$HTS v8=$V8 v9=$V9 stk=$STK icc=$ICC_AVG/$ICC_PEAK wm=$WM $(date +%T)"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|RATE|WM4|WMDIAG|BUSERR|KERNEL_OOPS|V6|ICC|AXI|VBIF|RECOVER|HTS|MMCC|SMMU|V7CLK|SMMU_FAULT|V8|V8PLL|CLKWARN|V9|V9VBIF|STK)' $O | sed "s/^/$TAG /" >> $SUM
    rp=$(grep -o 'A6L_STK STOP .* rup_irq [0-9]* rup_req [0-9]* sof_req [0-9]*' $O | tail -1 | sed 's/.*rup_irq \([0-9]*\) rup_req \([0-9]*\) sof_req \([0-9]*\)/\1\/\2\/\3/')
    bi=$(grep -o 'A6L_STK STOP .* be_ign [0-9]*' $O | tail -1 | sed 's/.* //')
    fr=$(grep -o 'A6L_VFE0_FRAMES.*' $O | tail -1 | sed -n 's/.*sof \([0-9]*\) done \([0-9]*\) buserr \([0-9]*\) first_buserr_sof \([0-9]*\).*/\1\/\2\/\3\/\4/p')
    dn=$(grep -o 'DONES [0-9]*' $O | tail -1 | cut -d' ' -f2)
    e0=$(grep -o 'A6L_WM4_SCAN slot 0 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //'); e1=$(grep -o 'A6L_WM4_SCAN slot 1 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //')
    l0=$(( ${e0:-0} / LB )); l1=$(( ${e1:-0} / LB ))
    mx=0; for x in $(grep -o 'A6L_WM4_P .*' $O | sed 's/.*slot0 head [01] lines \([0-9]*\) slot1 head [01] lines \([0-9]*\).*/\1 \2/'); do [ "$x" -gt "$mx" ] && mx=$x; done
    vw=$(grep -o 'A6L_V9 VBIF_WR [A-Z]*' $O | head -1 | sed 's/.* //')
    sk=$(grep -o 'A6L_V9 VBIF_WR [A-Z]* ([0-9]/3' $O | head -1 | sed 's/.*(//')
    ma=$(grep -o 'A6L_V9 MASK ac old [0-9a-f]* ones-> [0-9a-f]*' $O | head -1 | sed 's/.* //')
    md=$(grep -o 'A6L_V9 MASK .* d0 ones-> [0-9a-f]*' $O | head -1 | sed 's/.* //')
    ck=$(grep -o 'A6L_V9 VBIF START .* clkon(4) [0-9a-f]*' $O | head -1 | sed 's/.* //')
    hb=$(grep -o 'A6L_V9 VBIF BUSERR .* halt [0-9a-f]* [0-9a-f]*' $O | head -1 | sed 's/.* //')
    cap=FAIL; grep -q "CAPTURE_.*_PASS" $O && cap=PASS
    printf '%-9s %-4s %-3s %-3s %-3s %-3s %-18s %-5s %-11s %-9s %-11s %-4s %-9s %-5s %-8s %-8s %-8s %-8s %s\n' $vn $V6 $V7 $V8 $V9 $STK ${fr:-?} ${dn:-?} $l0/$l1 $mx ${rp:--} ${bi:--} ${vw:--} ${sk:--} ${ma:--} ${md:--} ${ck:--} ${hb:--} $cap >> $TAB
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V6_AFTER:-15} > $P/a6l_v6; echo ${V7_AFTER:-1} > $P/a6l_v7; echo ${V8_AFTER:-1} > $P/a6l_v8; echo ${V9_AFTER:-1} > $P/a6l_v9; echo ${STK_AFTER:-0} > $P/a6l_stk; echo 2000000 > $P/a6l_icc_avg; echo 4000000 > $P/a6l_icc_peak
  echo 0 > /sys/module/imx576_a6l/parameters/a6l_hts 2>/dev/null
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump  # back to the defaults (v9 bit16 VBIF values stay until reboot)
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (frame = $(( ${H:-2156} )) lines; vbif_wr WRITABLE = known VBIF fields accept HLOS writes; stick n/3; mask_* = all-ones readback)"; cat $TAB
  say "SWEEP_DONE $(date +%T) (pull /tmp/cam-$SENSOR-bars-sw9-*.raw .dmesg .out .dbgfs + sweep9.sum/.tab)"
  ;;
sweep8)
  # camfix8: is the MMSS NoC AXI clock dead (axi_clk_src on MMPLL4 with MMPLL4 off)? Entry =
  # name:v6:v7:hts:v8[:icc_avg:icc_peak]. ORDER MATTERS: v8 bits 4/8/16 persist until reboot, so they come last.
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-sweep8.sum; TAB=/tmp/cam-$SENSOR-sweep8.tab; : > $SUM; WM=${WM:-6}; DBGFS=${DBGFS:-1}
  P=/sys/module/qcom_camss/parameters; LB=$(( ${W:-2880} * 5 / 4 ))
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  printf '%-9s %-4s %-3s %-5s %-3s %-18s %-5s %-11s %-9s %-4s %-6s %-8s %-8s %-9s %s\n' variant v6 v7 hts v8 sof/done/be/first dones scan_s0/s1 maxl/frame rec vbif pll4_st pll4_be axi_cfg capture > $TAB
  for e in ${VARS:-base:0:1:0:1 pll:0:1:0:3 pllvbif:1:1:0:3 pllclk:0:5:0:3 plliccmax:4:1:0:3:6000000:8000000 hold:1:1:0:11 posthold:1:1:0:1 axi406:1:1:0:7 post406:1:1:0:1}; do
    vn=${e%%:*}; r=${e#*:}; v=${r%%:*}; r=${r#*:}; v7=${r%%:*}; r=${r#*:}; h=${r%%:*}; r=${r#*:}; v8=${r%%:*}; r2=${r#$v8}; r2=${r2#:}
    ICC_AVG=2000000; ICC_PEAK=4000000
    [ -n "$r2" ] && { ICC_AVG=${r2%%:*}; ICC_PEAK=${r2#*:}; }
    V6=$(( v | 128 )); V7=$v7; HTS=$h; V8=$v8; PDUMP=${PDUMP:-40}; TAG=-sw8-$vn
    say "SWEEP_BEGIN $TAG v6=$V6 v7=$V7 hts=$HTS v8=$V8 icc=$ICC_AVG/$ICC_PEAK wm=$WM $(date +%T)"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|RATE|WM4|WMDIAG|BUSERR|KERNEL_OOPS|V6|ICC|AXI|VBIF|RECOVER|HTS|MMCC|SMMU|V7CLK|SMMU_FAULT|V8|V8PLL|CLKWARN)' $O | sed "s/^/$TAG /" >> $SUM
    fr=$(grep -o 'A6L_VFE0_FRAMES.*' $O | tail -1 | sed -n 's/.*sof \([0-9]*\) done \([0-9]*\) buserr \([0-9]*\) first_buserr_sof \([0-9]*\).*/\1\/\2\/\3\/\4/p')
    dn=$(grep -o 'DONES [0-9]*' $O | tail -1 | cut -d' ' -f2)
    e0=$(grep -o 'A6L_WM4_SCAN slot 0 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //'); e1=$(grep -o 'A6L_WM4_SCAN slot 1 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //')
    l0=$(( ${e0:-0} / LB )); l1=$(( ${e1:-0} / LB ))
    mx=0; for x in $(grep -o 'A6L_WM4_P .*' $O | sed 's/.*slot0 head [01] lines \([0-9]*\) slot1 head [01] lines \([0-9]*\).*/\1 \2/'); do [ "$x" -gt "$mx" ] && mx=$x; done
    rec=$(grep -o 'A6L_VFE0_V6 .* rec [0-9]*' $O | tail -1 | sed 's/.* rec //')
    vb=$(grep -o 'A6L_V8 VBIF_STICK .* -> [a-zA-Z]*' $O | head -1 | sed 's/.* -> //')
    ps=$(grep -o 'A6L_V8 START mmpll4 mode [0-9a-f]*' $O | head -1 | sed 's/.* mode //')
    pb=$(grep -o 'A6L_V8 BUSERR mmpll4 mode [0-9a-f]*' $O | head -1 | sed 's/.* mode //')
    ax=$(grep -o 'A6L_V8 START mmpll4 .* axi rcg [0-9a-f]*/[0-9a-f]*' $O | head -1 | sed 's/.*axi rcg [0-9a-f]*\///')
    cap=FAIL; grep -q "CAPTURE_.*_PASS" $O && cap=PASS
    printf '%-9s %-4s %-3s %-5s %-3s %-18s %-5s %-11s %-9s %-4s %-6s %-8s %-8s %-9s %s\n' $vn $V6 $V7 $HTS $V8 ${fr:-?} ${dn:-?} $l0/$l1 $mx ${rec:-?} ${vb:--} ${ps:--} ${pb:--} ${ax:--} $cap >> $TAB
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V6_AFTER:-15} > $P/a6l_v6; echo ${V7_AFTER:-1} > $P/a6l_v7; echo ${V8_AFTER:-1} > $P/a6l_v8; echo 2000000 > $P/a6l_icc_avg; echo 4000000 > $P/a6l_icc_peak
  echo 0 > /sys/module/imx576_a6l/parameters/a6l_hts 2>/dev/null
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump  # back to the defaults (v8 4/8/16 effects stay until reboot)
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (frame = $(( ${H:-2156} )) lines; vbif ok = VBIF core clocked; pll4 mode c0000007 = on+locked)"; cat $TAB
  say "SWEEP_DONE $(date +%T) (pull /tmp/cam-$SENSOR-bars-sw8-*.raw .dmesg .out .dbgfs + sweep8.sum/.tab)"
  ;;
sweep7)
  # camfix7: one attended session -> which stock difference (SMMU impl-defs, NoC/SMMU clocks, ACTLR, bandwidth,
  # line rate) lets the RDI WM finish frames. Entry = name:v6:v7:hts[:icc_avg:icc_peak] (kBps).
  # (vn, not n: since() overwrites n, which garbled the camfix6 table's variant column)
  # ORDER MATTERS: v7 bits 2 and 8 (impl-defs, ACTLR) stay programmed until reboot, so they come last.
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-sweep.sum; TAB=/tmp/cam-$SENSOR-sweep.tab; : > $SUM; WM=${WM:-6}
  P=/sys/module/qcom_camss/parameters; LB=$(( ${W:-2880} * 5 / 4 ))
  printf '%-9s %-4s %-3s %-5s %-18s %-5s %-11s %-9s %-4s %-9s %s\n' variant v6 v7 hts sof/done/be/first dones scan_s0/s1 maxl/frame rec smmu_fsr capture > $TAB
  for e in ${VARS:-base:0:1:0 hts2:0:1:11088 hts4:0:1:22176 clk:0:5:0 iccmax:4:1:0:6000000:8000000 clkicc:4:5:0:6000000:8000000 smmu:0:3:0 smmuall:0:15:0 smmuhts2:0:15:11088 all:7:15:0:6000000:8000000}; do
    vn=${e%%:*}; r=${e#*:}; v=${r%%:*}; r=${r#*:}; v7=${r%%:*}; r=${r#*:}; h=${r%%:*}; r2=${r#$h}; r2=${r2#:}
    ICC_AVG=2000000; ICC_PEAK=4000000
    [ -n "$r2" ] && { ICC_AVG=${r2%%:*}; ICC_PEAK=${r2#*:}; }
    V6=$(( v | 128 )); V7=$v7; HTS=$h; PDUMP=${PDUMP:-40}; TAG=-sw7-$vn
    say "SWEEP_BEGIN $TAG v6=$V6 v7=$V7 hts=$HTS icc=$ICC_AVG/$ICC_PEAK wm=$WM"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|RATE|WM4|WMDIAG|BUSERR|KERNEL_OOPS|V6|ICC|AXI|VBIF|RECOVER|HTS|MMCC|SMMU|V7CLK|SMMU_FAULT)' $O | sed "s/^/$TAG /" >> $SUM
    fr=$(grep -o 'A6L_VFE0_FRAMES.*' $O | tail -1 | sed -n 's/.*sof \([0-9]*\) done \([0-9]*\) buserr \([0-9]*\) first_buserr_sof \([0-9]*\).*/\1\/\2\/\3\/\4/p')
    dn=$(grep -o 'DONES [0-9]*' $O | tail -1 | cut -d' ' -f2)
    e0=$(grep -o 'A6L_WM4_SCAN slot 0 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //'); e1=$(grep -o 'A6L_WM4_SCAN slot 1 .* end 0x[0-9a-f]*' $O | tail -1 | sed 's/.* end //')
    l0=$(( ${e0:-0} / LB )); l1=$(( ${e1:-0} / LB ))
    mx=0; for x in $(grep -o 'A6L_WM4_P .*' $O | sed 's/.*slot0 head [01] lines \([0-9]*\) slot1 head [01] lines \([0-9]*\).*/\1 \2/'); do [ "$x" -gt "$mx" ] && mx=$x; done
    rec=$(grep -o 'A6L_VFE0_V6 .* rec [0-9]*' $O | tail -1 | sed 's/.* rec //')
    fsr=$(grep -o 'A6L_SMMU BUSERR cb[0-9]* .* fsr [0-9a-f]*' $O | head -1 | sed 's/.* fsr //'); [ -z "$fsr" ] && fsr=$(grep -o 'A6L_SMMU STOP cb[0-9]* .* fsr [0-9a-f]*' $O | head -1 | sed 's/.* fsr /s:/')
    cap=FAIL; grep -q "CAPTURE_.*_PASS" $O && cap=PASS
    printf '%-9s %-4s %-3s %-5s %-18s %-5s %-11s %-9s %-4s %-9s %s\n' $vn $V6 $V7 $HTS ${fr:-?} ${dn:-?} $l0/$l1 $mx ${rec:-?} ${fsr:--} $cap >> $TAB
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V6_AFTER:-15} > $P/a6l_v6; echo ${V7_AFTER:-1} > $P/a6l_v7; echo 2000000 > $P/a6l_icc_avg; echo 4000000 > $P/a6l_icc_peak
  echo 0 > /sys/module/imx576_a6l/parameters/a6l_hts 2>/dev/null
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump  # back to the defaults (impl-defs / ACTLR stay until reboot)
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (frame = $(( ${H:-2156} )) lines)"; cat $TAB
  say "SWEEP_DONE (pull /tmp/cam-$SENSOR-bars-sw7-*.raw .dmesg .out + sweep.sum/.tab)"
  ;;
focus)
  mknodes
  $D/a6l_camcap -s gt9769 -f ${STEPS:-0,256,512,768,1023,512,0} && say FOCUS_DONE ask Pierre: lens movement heard/seen? || say FOCUS_FAIL
  ;;
off)
  for m in imx576_a6l s5k3t1 hi846 gt9769; do rmmod $m 2>/dev/null; done
  say "OFF_DONE sensors removed (rail gpio51 released by runtime PM)"
  ;;
*) say "unknown MODE=$MODE"; exit 2;;
esac
