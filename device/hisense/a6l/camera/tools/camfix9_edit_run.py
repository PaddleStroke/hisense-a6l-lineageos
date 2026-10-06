#!/usr/bin/env python3
import sys
p = sys.argv[1]
s = open(p).read()

def sub1(old, new):
    global s
    n = s.count(old)
    assert n == 1, f"anchor {n}x: {old[:80]!r}"
    s = s.replace(old, new)

sub1("# A6L camera attended test, camfix8 revision (30 Sep 2026, bundle camera10 = firmware/extracted/camera-20260930-camfix8).\n",
     "# A6L camera attended test, camfix9 revision (30 Sep 2026, bundle camera11 = firmware/extracted/camera-20260930-camfix9).\n")
sub1("#   A kernel oops in an earlier run blocks bars/live (reboot first): the camss state is then undefined.\n",
"""#   camfix9 knob: V9=<mask> qcom_camss.a6l_v9 (default 0x01): 1 camera VBIF full dump + write-stick tests on fields
#                 every VBIF has (A6L_V9 VBIF_WR WRITABLE|READONLY) + VBIF clock view, 2 VBIF_CLKON bit0 force-on
#                 during the stream, 4 VBIF_CLK_FORCE_CTRL0/1 all ones during the stream, 8 power VFE1 (shared VBIF)
#                 during the stream, 16 re-apply the stock VBIF settings after 2/4/8 (all restored at stream-off
#                 except 16)
#   MODE=sweep (camfix9): VARS entries name:v6:v7:hts:v8:v9[:icc_avg:icc_peak], default = camfix9 matrix (8 captures,
#                 ~3 min), DBGFS=1; table /tmp/cam-<sensor>-sweep9.tab. The camfix8 matrix is MODE=sweep8.
#                 Detached-safe: nohup setsid ... > /tmp/c11-sweep.log 2>&1 & and poll for SWEEP_DONE.
#   A kernel oops in an earlier run blocks bars/live (reboot first): the camss state is then undefined.
""")
sub1("  [ -n \"$V8\" ] && echo $((V8)) > $P/a6l_v8\n",
     "  [ -n \"$V8\" ] && echo $((V8)) > $P/a6l_v8\n  [ -n \"$V9\" ] && echo $((V9)) > $P/a6l_v9\n")
sub1("a6l_v7 a6l_v8 a6l_csid_irqmask", "a6l_v7 a6l_v8 a6l_v9 a6l_csid_irqmask")
sub1("  grep -iE \"Unhandled context fault|",
     "  # camfix9\n"
     "  grep -o \"A6L_V9 .*\" $F | grep -vE \"A6L_V9 VBIF \" | head -14 | while read l; do say \"V9 $l\"; done\n"
     "  grep -o \"A6L_V9 VBIF [A-Z]* .*\" $F | head -4 | while read l; do say \"V9VBIF $l\"; done\n"
     "  grep -iE \"Unhandled context fault|")
sub1("  say \"V8_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v8 2>/dev/null)\"\n",
     "  say \"V8_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v8 2>/dev/null)\"\n"
     "  [ -f /sys/module/qcom_camss/parameters/a6l_v9 ] && say CAMSS_CAMFIX9_PASS || say \"CAMSS_CAMFIX9_FAIL (old qcom-camss loaded: reboot)\"\n"
     "  say \"V9_DEFAULT $(cat /sys/module/qcom_camss/parameters/a6l_v9 2>/dev/null)\"\n")

new_sweep = r'''sweep|sweep9)
  # camfix9: is the camera VBIF (0xca40000) really unwritable, and does forcing its clocks / powering VFE1 change the
  # stream? Entry = name:v6:v7:hts:v8:v9[:icc_avg:icc_peak]. v9 bits 2/4/8 are undone at stream-off; bit 16 (stock
  # VBIF values) stays, so 'post' (diagnostics only) at the end shows what persisted.
  mknodes; MODE=bars; SUM=/tmp/cam-$SENSOR-sweep9.sum; TAB=/tmp/cam-$SENSOR-sweep9.tab; : > $SUM; WM=${WM:-6}; DBGFS=${DBGFS:-1}
  P=/sys/module/qcom_camss/parameters; LB=$(( ${W:-2880} * 5 / 4 ))
  grep -q clk_ignore_unused /proc/cmdline && say "CMDLINE clk_ignore_unused=yes" || say "CMDLINE clk_ignore_unused=NO"
  printf '%-9s %-4s %-3s %-3s %-3s %-18s %-5s %-11s %-9s %-9s %-5s %-8s %-8s %-8s %-8s %s\n' variant v6 v7 v8 v9 sof/done/be/first dones scan_s0/s1 maxl/frame vbif_wr stick mask_ac mask_d0 clkon_st halt_be capture > $TAB
  for e in ${VARS:-base:0:1:0:1:1 clkon:0:1:0:1:3 fctl:0:1:0:1:5 clkvbif:1:1:0:1:19 vfe1:0:1:0:1:9 vfe1vbif:1:1:0:1:25 forceall:1:1:0:1:31 post:0:1:0:1:1}; do
    vn=${e%%:*}; r=${e#*:}; v=${r%%:*}; r=${r#*:}; v7=${r%%:*}; r=${r#*:}; h=${r%%:*}; r=${r#*:}; v8=${r%%:*}; r=${r#*:}; v9=${r%%:*}; r2=${r#$v9}; r2=${r2#:}
    ICC_AVG=2000000; ICC_PEAK=4000000
    [ -n "$r2" ] && { ICC_AVG=${r2%%:*}; ICC_PEAK=${r2#*:}; }
    V6=$(( v | 128 )); V7=$v7; HTS=$h; V8=$v8; V9=$v9; PDUMP=${PDUMP:-40}; TAG=-sw9-$vn
    say "SWEEP_BEGIN $TAG v6=$V6 v7=$V7 hts=$HTS v8=$V8 v9=$V9 icc=$ICC_AVG/$ICC_PEAK wm=$WM $(date +%T)"; capture > /tmp/cam-$SENSOR-bars$TAG.out 2>&1; rc=$?; cat /tmp/cam-$SENSOR-bars$TAG.out
    O=/tmp/cam-$SENSOR-bars$TAG.out
    grep -E 'A6L_CAM (CAPTURE|DONES|FRAMES|RATE|WM4|WMDIAG|BUSERR|KERNEL_OOPS|V6|ICC|AXI|VBIF|RECOVER|HTS|MMCC|SMMU|V7CLK|SMMU_FAULT|V8|V8PLL|CLKWARN|V9|V9VBIF)' $O | sed "s/^/$TAG /" >> $SUM
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
    printf '%-9s %-4s %-3s %-3s %-3s %-18s %-5s %-11s %-9s %-9s %-5s %-8s %-8s %-8s %-8s %s\n' $vn $V6 $V7 $V8 $V9 ${fr:-?} ${dn:-?} $l0/$l1 $mx ${vw:--} ${sk:--} ${ma:--} ${md:--} ${ck:--} ${hb:--} $cap >> $TAB
    [ $rc = 3 ] && break; grep -q KERNEL_OOPS_FAIL $O && break; sleep 1
  done
  echo ${V6_AFTER:-15} > $P/a6l_v6; echo ${V7_AFTER:-1} > $P/a6l_v7; echo ${V8_AFTER:-1} > $P/a6l_v8; echo ${V9_AFTER:-1} > $P/a6l_v9; echo 2000000 > $P/a6l_icc_avg; echo 4000000 > $P/a6l_icc_peak
  echo 0 > /sys/module/imx576_a6l/parameters/a6l_hts 2>/dev/null
  echo ${WM_AFTER:-3} > $P/a6l_wm; echo 0 > $P/a6l_fdump  # back to the defaults (v9 bit16 VBIF values stay until reboot)
  say SWEEP_SUMMARY; cat $SUM; say "SWEEP_TABLE (frame = $(( ${H:-2156} )) lines; vbif_wr WRITABLE = known VBIF fields accept HLOS writes; stick n/3; mask_* = all-ones readback)"; cat $TAB
  say "SWEEP_DONE $(date +%T) (pull /tmp/cam-$SENSOR-bars-sw9-*.raw .dmesg .out .dbgfs + sweep9.sum/.tab)"
  ;;
sweep8)
'''
sub1("sweep|sweep8)\n", new_sweep)
open(p, 'w').write(s)
print("RUN_EDIT_OK")
