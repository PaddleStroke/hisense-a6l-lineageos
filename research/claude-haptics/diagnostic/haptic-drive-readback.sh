#!/system/bin/sh
# =====================================================================
#  A6L PM660 haptics drive-path diagnostic  —  NOT EXECUTED ON HARDWARE
# =====================================================================
# Author: claude-haptics offline investigation, 2026-09-18.
# Runs on the phone LATER, by the main agent, over authenticated ADB in the
# 7.2.3-a6l-probe+ diagnostic RAM environment. It is READ-CENTRIC: it observes
# what the haptic block is actually programmed to, so we can tell WHY the V46
# 100 ms pulse produced no perceptible output. One attended run learns several
# things. It does NOT install the candidate patch and does NOT exceed stock
# operating limits (vmax <= 3200 mV, ilim as stock, pulse <= 150 ms).
#
# WHY: the port runs the pmi8998 mainline qcom-spmi-haptics driver, in DIRECT
# mode, on a PM660. Source review (EVIDENCE.md) shows it never programs the
# internal-PWM carrier (0x56/0x58), leaves current limit at 400 mA (stock 800),
# hardcodes auto-res ZXD_EOP (stock qwd) with no drive-freq/clk-trim
# correction, and the V46 test requested only ~1276 mV (stock 3200). This
# reads the live registers to see which of those actually took effect.
#
# SAFETY / STOP CONDITIONS:
#   * Abort if uname is not 7.2.3-a6l-probe+.
#   * Read-only by default. The optional POKE stage (Stage 4) is OFF unless
#     A6L_ALLOW_REG_POKE=1 is exported; it writes only stock-legal values and
#     is bounded. If STATUS_1 (0x0A) ever shows SC_FLAG set, STOP: short circuit.
#   * Never loop a pulse longer than 150 ms. No amplitude above stock 3200 mV.
#   * On any unexpected dmesg "Short circuit" / "disabling haptics": STOP,
#     collect logs, return to stock. Do not retry.
#
# CLEANUP: rmmod qcom_spmi_haptics; remove /tmp nodes; return to stock Android.
# ---------------------------------------------------------------------
set -u
OUT=/tmp/a6l-haptic-diag
BASE=c000                 # vibrator@c000 on pmic@1 (sid 1)
MOD=/tmp/a6l-controls-v46/qcom-spmi-haptics.ko   # EXISTING R4 module, unchanged
PROBE=/tmp/a6l-controls-v46/a6l_haptic_probe
mkdir -p $OUT

echo "== 0. environment gate =="
uname -r | tee $OUT/uname.txt
case "$(uname -r)" in *a6l-probe+*) : ;; *) echo "REFUSE: wrong kernel"; exit 2;; esac

# Locate a debugfs read path for the pmic@1 (sid 1) SPMI regmap.
# Two common shapes; the agent keeps whichever exists.
echo "== 1. locate register access =="
ls -d /sys/kernel/debug/regmap/*vibrator* /sys/kernel/debug/regmap/*pmic@1* \
      /sys/kernel/debug/regmap/1-* 2>/dev/null | tee $OUT/regmap-paths.txt
ls -d /sys/kernel/debug/spmi/* 2>/dev/null | tee $OUT/spmi-paths.txt

# Helper: dump the haptic register window from a regmap 'registers' file.
dump_regmap() {  # $1 = regmap dir
    if [ -r "$1/registers" ]; then
        # regmap 'registers' prints "ADDR: VV" lines; keep c000..c0ff.
        grep -iE "^c0[0-9a-f][0-9a-f]:" "$1/registers"
    fi
}

echo "== 2. STAGE 1: load module, create node, read BASELINE programmed state =="
cat /proc/modules | grep -q qcom_spmi_haptics || insmod "$MOD"
# (Re)create input node exactly as V46 did — reuse the staged node creator.
[ -e /dev/input/event3 ] || echo "NOTE: ensure spmi_haptics input node exists (Create-V46InputNodes.py)"
RMAP=$(ls -d /sys/kernel/debug/regmap/*vibrator* /sys/kernel/debug/regmap/1-* 2>/dev/null | head -1)
echo "regmap dir = $RMAP" | tee $OUT/chosen-regmap.txt
dump_regmap "$RMAP" | tee $OUT/regs-baseline.txt
echo "-- interpret key offsets (add $BASE): 46=EN 48=EN_CTL2 4B=AUTO_RES_CTRL"
echo "   4E=SEL(play-mode) 4F=LRA_AUTO_RES 51=VMAX 52=ILIM 54/55=RATE"
echo "   56=INT_PWM 58=PWM_CAP 5C=BRAKE 0A=STATUS_1 0B/0C=AUTO_RES_LO/HI 70=PLAY"

echo "== 3. STAGE 2: fire the UNCHANGED V46 pulse and sample STATUS during play =="
dmesg -c >/dev/null 2>&1 || true
# Launch the bounded 100 ms / ~1276 mV pulse in the background, then poll.
"$PROBE" --pulse /dev/input/event3 >$OUT/probe-lowv.txt 2>&1 &
PP=$!
i=0
: > $OUT/status-during-lowv.txt
while [ $i -lt 12 ]; do
    dump_regmap "$RMAP" | grep -iE "^c0(0a|46|4f|51|70):" >> $OUT/status-during-lowv.txt
    echo "---" >> $OUT/status-during-lowv.txt
    i=$((i+1))
done
wait $PP
dmesg | tee $OUT/dmesg-lowv.txt | grep -iE "haptic|short|auto.?res" || true
dump_regmap "$RMAP" > $OUT/regs-after-lowv.txt

echo "== 4. STAGE 3: bounded voltage discriminator (<= stock 3200 mV) =="
# Distinguishes 'voltage too low' from 'drive path broken'. This is a MEASUREMENT
# at the stock operating point, NOT a fix and NOT above stock. Requires a probe
# variant a6l_haptic_probe_strong built with STRENGTH=22528 (-> magnitude 88 ->
# ~3132 mV, just under stock qcom,vmax-mv=3200) and LENGTH_MS<=150. If that binary is not
# staged, SKIP this stage rather than exceeding bounds by other means.
STRONG=/tmp/a6l-controls-v46/a6l_haptic_probe_strong
if [ -x "$STRONG" ]; then
    dmesg -c >/dev/null 2>&1 || true
    "$STRONG" --pulse /dev/input/event3 | tee $OUT/probe-strong.txt
    dump_regmap "$RMAP" | grep -iE "^c0(0a|51|70):" | tee $OUT/regs-during-strong.txt
    dmesg | tee $OUT/dmesg-strong.txt | grep -iE "haptic|short|auto.?res" || true
    echo "ASK USER: any vibration felt on the STRONG (3200 mV) pulse? record yes/no"
else
    echo "SKIP Stage 3: strong probe not staged (do not substitute an unbounded write)"
fi

echo "== 5. STAGE 4 (OPTIONAL, gated): live register poke to test INT_PWM/ILIM =="
# Tests the internal-PWM + current-limit hypothesis WITHOUT building the patch:
# set INT_PWM(56)=1 and PWM_CAP(58)=1 (505 kHz) and ILIM(52)=1 (800 mA) via the
# regmap debug write file, then re-fire the UNCHANGED low-voltage pulse. All
# values are stock-legal. OFF unless explicitly allowed.
if [ "${A6L_ALLOW_REG_POKE:-0}" = "1" ] && [ -w "$RMAP/registers" ]; then
    # regmap 'registers' write syntax is "ADDR VALUE" (hex). Agent confirms the
    # write interface before use; if unsure, DO NOT poke.
    echo "c056 01" > "$RMAP/registers" 2>>$OUT/poke-err.txt
    echo "c058 01" > "$RMAP/registers" 2>>$OUT/poke-err.txt
    echo "c052 01" > "$RMAP/registers" 2>>$OUT/poke-err.txt
    dump_regmap "$RMAP" | grep -iE "^c0(52|56|58):" | tee $OUT/regs-after-poke.txt
    dmesg -c >/dev/null 2>&1 || true
    "$PROBE" --pulse /dev/input/event3 | tee $OUT/probe-after-poke.txt
    dmesg | grep -iE "haptic|short" | tee $OUT/dmesg-after-poke.txt || true
    echo "ASK USER: any vibration felt AFTER the poke (still ~1276 mV)? record yes/no"
else
    echo "Stage 4 disabled (set A6L_ALLOW_REG_POKE=1 and confirm a writable regmap to enable)"
fi

echo "== 6. cleanup =="
rmmod qcom_spmi_haptics 2>/dev/null || true
echo "Collected under $OUT ; copy back with adb pull, hash, then return to stock."
