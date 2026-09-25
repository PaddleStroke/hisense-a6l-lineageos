#!/usr/bin/env python3
"""Compute the SDM660 CPRh OPEN-LOOP voltages the A6L would get, from a read-only qfprom dump.
Input: raw copy of /sys/bus/nvmem/devices/qfprom0/nvmem (mainline qfprom@780000, fuse rows at +0x4000),
       or --offset 0 for a dump that starts at 0x784000.
Formula = downstream cprh-kbss/cpr3 (msm-4.4): V_fc = ref + signmag6(init_fuse)*10mV + oloop_fuse_adj[fc];
corner voltages interpolated by frequency between fuse-corner fmax corners, then clamped to stock
per-corner [floor, ceiling] and floor >= ceiling - max_range, rounded UP to the 4 mV step.
Tables: stock kernel ELF (sdm660_kbss_*), stock DT cprh-ctrl@179c8000/@179c4000."""
import argparse, struct, sys

INIT = [[(67,34,39),(67,28,33),(71,3,8),(67,22,27),(67,16,21)],
        [(69,17,22),(69,23,28),(69,11,16),(69,5,10),(70,42,47)]]
REF = [[644000,724000,788000,868000,1068000],[724000,788000,868000,988000,1068000]]
OADJ = [[-4000,4000,7000,19000,-8000],[16000,27000,39000,39000,20000]]
SPEED_BIN = (38,29,31); FUSE_REV = (71,28,30)
# per-corner (1-based) data, speed bin 0/1/4 layout (bin 3 differs only in pwrcl corner 7 = 1612.8 MHz)
CL = {
 'pwrcl': dict(freq=[300000000,633600000,902400000,1113600000,1401600000,1536000000,1747200000,1843200000],
               ceil=[724000,724000,724000,788000,868000,1068000,1068000,1068000],
               floor=[588000,588000,596000,652000,712000,744000,784000,844000],
               rng=[32000,32000,32000,40000,44000,40000,40000,40000], fmax=[2,3,4,5,8]),
 'perfcl': dict(freq=[300000000,1113600000,1401600000,1747200000,1958400000,2150400000,2208000000],
               ceil=[724000,724000,788000,868000,988000,988000,1068000],
               floor=[588000,596000,652000,712000,744000,784000,844000],
               rng=[40000,40000,40000,40000,66000,66000,40000], fmax=[2,3,4,6,7]),
}
STEP = 4000

def bits(buf, base, row, s, e):
    v = struct.unpack_from('<Q', buf, base + row * 8)[0]
    return (v >> s) & ((1 << (e - s + 1)) - 1)

def signmag(v, n):
    return -(v & ((1 << (n - 1)) - 1)) if v & (1 << (n - 1)) else v

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump'); ap.add_argument('--offset', type=lambda x: int(x, 0), default=0x4000)
    a = ap.parse_args()
    buf = open(a.dump, 'rb').read()
    if len(buf) < a.offset + 72 * 8:
        sys.exit(f"A6L_CPR_FAIL dump too short ({len(buf)} bytes)")
    print(f"speed_bin={bits(buf, a.offset, *SPEED_BIN)} cpr_fusing_rev={bits(buf, a.offset, *FUSE_REV)}")
    ok = True
    for ci, name in enumerate(('pwrcl', 'perfcl')):
        c = CL[name]
        fcv = []
        for fc in range(5):
            raw = bits(buf, a.offset, *INIT[ci][fc])
            v = REF[ci][fc] + signmag(raw, 6) * 10000 + OADJ[ci][fc]
            fcv.append(v)
            print(f"  {name} FC{fc} init_fuse=0x{raw:02x} ({signmag(raw,6):+d} steps) -> {v} uV")
        n = len(c['freq']); volts = [0] * n
        prev_c, prev_v = None, None
        for fc, fm in enumerate(c['fmax']):
            i1 = fm - 1
            for k in range((prev_c + 1) if prev_c is not None else 0, i1 + 1):
                if prev_c is None or k == i1:
                    volts[k] = fcv[fc]
                else:
                    f0, f1 = c['freq'][prev_c], c['freq'][i1]
                    volts[k] = prev_v + (fcv[fc] - prev_v) * (c['freq'][k] - f0) // (f1 - f0)
            prev_c, prev_v = i1, fcv[fc]
        for k in range(n):
            olr = -(-volts[k] // STEP) * STEP
            # qcom,cpr-scaled-open-loop-voltage-as-ceiling (stock DT): ceiling = min(ceiling, open-loop)
            ceil = max(min(c['ceil'][k], olr), c['floor'][k])
            flo = max(c['floor'][k], ceil - c['rng'][k])
            v = min(max(volts[k], flo), ceil)
            v = -(-v // STEP) * STEP
            clamp = '' if v == -(-volts[k] // STEP) * STEP else f' (raw {volts[k]} clamped)'
            print(f"  {name} corner{k+1} {c['freq'][k]/1e6:7.1f} MHz -> open-loop {v} uV [floor {flo}, ceil {ceil}]{clamp}")
            if not (500000 <= v <= 1100000):
                ok = False
    print("A6L_CPR_OPENLOOP_" + ("SANE" if ok else "OUT_OF_RANGE"))

if __name__ == '__main__':
    main()
