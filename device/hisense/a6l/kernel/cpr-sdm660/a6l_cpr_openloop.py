#!/usr/bin/env python3
"""Compute the SDM660 CPRh OPEN-LOOP voltages the A6L would get (agent rest 24 Sep; stock input: agent power 26 Sep).

Inputs (any combination; at least one of DUMP / --stock-log):
  DUMP           raw fuse rows: a copy of /sys/bus/nvmem/devices/qfprom0/nvmem (mainline qfprom@780000, rows at +0x4000)
                 or, with --offset 0, a dump that starts at 0x784000 (row 0), e.g. a6l-fuserows.bin from
                 run-power.sh MODE=fuserows (only rows 38 and 65..71 are filled; the rest are zero and never read).
  --stock-log F  output of a6l-stock-cpr-read.sh (rooted STOCK Android): boot-log lines
                 "apc0: apc0_pwrcl_corner: speed bin = N", "CPR fusing revision = N", "fused   LowSVS: open-loop= 644000 uV",
                 "fused LowSVS: quot[ 7]=1234, quot_offset[ 7]=  0" and (CORNERS=1) the per-corner debugfs table.
                 The fused open-loop voltage = ref + signmag6(init fuse) * 10 mV, so the init fuses are recovered from it:
                 NO qfprom access is needed at all.
  --emit-dump O  write a synthetic 72-row dump (--offset 0 layout) holding the fields recovered from --stock-log
                 (speed bin, fusing rev, init voltages, quotients, RO select; quot_offset only if integral), for comparing
                 with a MODE=fuserows read or for feeding the mainline tables offline.
  --mainline F   output of run-power.sh MODE=cpr-check on the CPR kernel (lines "thread0 ... corner 2 - uV=[a b c] ...
                 freq=f", mainline drivers/pmdomain/qcom/cpr3.c debugfs qcom_cpr3/threadN; thread0 = pwrcl, thread1 =
                 perfcl): every mainline corner voltage must equal the expected open-loop (+-4 mV) and stay <= the stock
                 ceiling -> A6L_CPR_MAINLINE_MATCH, else A6L_CPR_MAINLINE_MISMATCH (stop the bring-up).
Formula = downstream cprh-kbss/cpr3 (msm-4.4): V_fc = ref + signmag6(init_fuse)*10mV + oloop_fuse_adj[fc];
corner voltages interpolated by frequency between fuse-corner fmax corners, then clamped to stock
per-corner [floor, ceiling] and floor >= ceiling - max_range, rounded UP to the 4 mV step.
Tables: stock kernel ELF (sdm660_kbss_*), stock DT cprh-ctrl@179c8000/@179c4000.
Exit status: 0 = sane (and matching stock when stock data is given), 1 = out of range or stock mismatch."""
import argparse, re, struct, sys

INIT = [[(67,34,39),(67,28,33),(71,3,8),(67,22,27),(67,16,21)],
        [(69,17,22),(69,23,28),(69,11,16),(69,5,10),(70,42,47)]]
QUOT = [[(68,12,23),(68,0,11),(71,9,20),(67,52,63),(67,40,51)],
        [((69,53,63),(70,0,0)),(70,1,12),(69,41,52),(69,29,40),(70,48,59)]]
ROSEL = [[(67,12,15),(67,8,11),(65,56,59),(67,4,7),(67,0,3)],
         [((68,61,63),(69,0,0)),(69,1,4),(68,57,60),(68,53,56),(66,14,17)]]
QOFF = [[None,(68,38,44),(71,21,27),(68,31,37),(68,24,30)],
        [None,(70,27,33),(70,20,26),(70,13,19),((70,60,63),(71,0,2))]]
REF = [[644000,724000,788000,868000,1068000],[724000,788000,868000,988000,1068000]]
OADJ = [[-4000,4000,7000,19000,-8000],[16000,27000,39000,39000,20000]]
FC_NAMES = [['LowSVS','SVS','SVSPLUS','NOM','TURBO_L1'], ['SVS','SVSPLUS','NOM','TURBO','TURBO_L2']]
SPEED_BIN = (38,29,31); FUSE_REV = (71,28,30)
FUSE_ROWS = [38] + list(range(65, 72))          # every row the stock kernel (and the mainline nvmem cells) read
QOFF_SCALE = 5                                  # cprh-kbss prints quot_offset * CPRH_KBSS_QUOT_OFFSET_SCALE
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
NROWS = 72

def _parts(p):
    return p if isinstance(p[0], tuple) else (p,)

def bits(buf, base, p):
    v, shift = 0, 0
    for row, s, e in _parts(p):
        w = e - s + 1
        r = struct.unpack_from('<Q', buf, base + row * 8)[0]
        v |= ((r >> s) & ((1 << w) - 1)) << shift
        shift += w
    return v

def put_bits(rows, p, val):
    for row, s, e in _parts(p):
        w = e - s + 1
        rows[row] = (rows[row] & ~(((1 << w) - 1) << s)) | ((val & ((1 << w) - 1)) << s)
        val >>= w

def signmag(v, n):
    return -(v & ((1 << (n - 1)) - 1)) if v & (1 << (n - 1)) else v

def to_signmag(steps, n):
    if abs(steps) >= (1 << (n - 1)):
        raise ValueError(f"{steps} does not fit a {n}-bit sign-magnitude fuse")
    return ((1 << (n - 1)) | -steps) if steps < 0 else steps

def corners(init_raw):
    """init_raw[cluster][fc] (6-bit fuse values) -> {name: [(freq, v, floor, ceil, raw)]}"""
    out = {}
    for ci, name in enumerate(('pwrcl', 'perfcl')):
        c = CL[name]
        fcv = [REF[ci][fc] + signmag(init_raw[ci][fc], 6) * 10000 + OADJ[ci][fc] for fc in range(5)]
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
        rows = []
        for k in range(n):
            olr = -(-volts[k] // STEP) * STEP
            # qcom,cpr-scaled-open-loop-voltage-as-ceiling (stock DT): ceiling = min(ceiling, open-loop)
            ceil = max(min(c['ceil'][k], olr), c['floor'][k])
            flo = max(c['floor'][k], ceil - c['rng'][k])
            v = min(max(volts[k], flo), ceil)
            v = -(-v // STEP) * STEP
            rows.append((c['freq'][k], v, flo, ceil, volts[k]))
        out[name] = (fcv, rows)
    return out

def parse_stock(path):
    """-> dict(speed_bin, rev, fused[ci][fc] uV, adj[ci][fc] uV, quot/rosel/qoff[ci][fc], corners[ci]{idx: (ol,floor,ceil)})"""
    st = dict(speed_bin=None, rev=None, fused=[[None]*5 for _ in range(2)], adj=[[None]*5 for _ in range(2)],
              quot=[[None]*5 for _ in range(2)], rosel=[[None]*5 for _ in range(2)], qoff=[[None]*5 for _ in range(2)],
              corners=[{}, {}])
    def cl(line):
        if re.search(r'apc0|pwrcl', line): return 0
        if re.search(r'apc1|perfcl', line): return 1
        return None
    for line in open(path, errors='replace'):
        ci = cl(line)
        if ci is None:
            continue
        m = re.search(r'speed bin = (\d+)', line)
        if m: st['speed_bin'] = int(m.group(1)); continue
        m = re.search(r'CPR fusing revision = (\d+)', line)
        if m: st['rev'] = int(m.group(1)); continue
        m = re.search(r'fused\s+(\S+): open-loop=\s*(-?\d+) uV', line)
        if m and m.group(1) in FC_NAMES[ci]:
            st['fused'][ci][FC_NAMES[ci].index(m.group(1))] = int(m.group(2)); continue
        m = re.search(r'fuse_corner\[(\d+)\] open-loop=\s*(-?\d+) uV', line)
        if m and int(m.group(1)) < 5:
            st['adj'][ci][int(m.group(1))] = int(m.group(2)); continue
        m = re.search(r'fused\s+(\S+): quot\[\s*(\d+)\]=\s*(\d+)(?:, quot_offset\[\s*\d+\]=\s*(\d+))?', line)
        if m and m.group(1) in FC_NAMES[ci]:
            fc = FC_NAMES[ci].index(m.group(1))
            st['rosel'][ci][fc] = int(m.group(2)); st['quot'][ci][fc] = int(m.group(3))
            if m.group(4) is not None: st['qoff'][ci][fc] = int(m.group(4))
            continue
        m = re.search(r'A6L_STOCK CORNER \S+ (\d+) open_loop=(\d+) floor=(\d+) ceiling=(\d+)', line)
        if m:
            st['corners'][ci][int(m.group(1))] = tuple(int(m.group(i)) for i in (2, 3, 4))
    return st

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dump', nargs='?')
    ap.add_argument('--offset', type=lambda x: int(x, 0), default=0x4000)
    ap.add_argument('--stock-log'); ap.add_argument('--emit-dump'); ap.add_argument('--mainline')
    a = ap.parse_args()
    if not a.dump and not a.stock_log:
        ap.error('give a fuse DUMP and/or --stock-log')
    ok = True
    init_dump = init_stock = None
    if a.dump:
        buf = open(a.dump, 'rb').read()
        if len(buf) < a.offset + NROWS * 8:
            sys.exit(f"A6L_CPR_FAIL dump too short ({len(buf)} bytes)")
        if not any(struct.unpack_from('<Q', buf, a.offset + r * 8)[0] for r in FUSE_ROWS):
            sys.exit("A6L_CPR_FAIL all CPR fuse rows are zero (wrong --offset or empty read)")
        init_dump = [[bits(buf, a.offset, INIT[ci][fc]) for fc in range(5)] for ci in range(2)]
        print(f"[dump] speed_bin={bits(buf, a.offset, SPEED_BIN)} cpr_fusing_rev={bits(buf, a.offset, FUSE_REV)}")
        for ci, name in enumerate(('pwrcl', 'perfcl')):
            print(f"[dump]  {name} quot={[bits(buf, a.offset, QUOT[ci][fc]) for fc in range(5)]} "
                  f"ro_sel={[bits(buf, a.offset, ROSEL[ci][fc]) for fc in range(5)]}")
    st = None
    if a.stock_log:
        st = parse_stock(a.stock_log)
        print(f"[stock] speed_bin={st['speed_bin']} cpr_fusing_rev={st['rev']}")
        miss = [(ci, fc) for ci in range(2) for fc in range(5) if st['fused'][ci][fc] is None]
        if miss:
            sys.exit(f"A6L_CPR_FAIL stock log lacks 'fused <corner>: open-loop' for {miss} "
                     "(boot log rotated: reboot stock and run a6l-stock-cpr-read.sh within 2 minutes)")
        init_stock = [[0]*5 for _ in range(2)]
        for ci in range(2):
            for fc in range(5):
                d = st['fused'][ci][fc] - REF[ci][fc]
                if d % 10000:
                    print(f"[stock]  WARN {FC_NAMES[ci][fc]} fused {st['fused'][ci][fc]} - ref {REF[ci][fc]} is not a 10 mV multiple "
                          "(ref table mismatch for this speed bin?)"); ok = False
                init_stock[ci][fc] = to_signmag(round(d / 10000), 6)
            print(f"[stock]  {('pwrcl','perfcl')[ci]} init fuses (from fused open-loop) = "
                  + ' '.join(f"{FC_NAMES[ci][fc]}:{signmag(init_stock[ci][fc],6):+d}" for fc in range(5)))
        if init_dump is not None:
            same = init_dump == init_stock
            print(f"A6L_CPR_DUMP_VS_STOCK {'MATCH' if same else 'MISMATCH'} dump={init_dump} stock={init_stock}")
            ok &= same
        if a.emit_dump:
            rows = [0] * NROWS
            put_bits(rows, SPEED_BIN, st['speed_bin'] or 0); put_bits(rows, FUSE_REV, st['rev'] or 0)
            for ci in range(2):
                for fc in range(5):
                    put_bits(rows, INIT[ci][fc], init_stock[ci][fc])
                    if st['quot'][ci][fc] is not None: put_bits(rows, QUOT[ci][fc], st['quot'][ci][fc])
                    if st['rosel'][ci][fc] is not None: put_bits(rows, ROSEL[ci][fc], st['rosel'][ci][fc])
                    q = st['qoff'][ci][fc]
                    if QOFF[ci][fc] and q is not None and q % QOFF_SCALE == 0: put_bits(rows, QOFF[ci][fc], q // QOFF_SCALE)
            open(a.emit_dump, 'wb').write(b''.join(struct.pack('<Q', r) for r in rows))
            print(f"[stock] synthetic dump written: {a.emit_dump} ({NROWS*8} bytes, --offset 0 layout)")
    init = init_stock if init_stock is not None else init_dump
    res = corners(init)
    for ci, name in enumerate(('pwrcl', 'perfcl')):
        fcv, rows = res[name]
        for fc in range(5):
            line = f"  {name} FC{fc} {FC_NAMES[ci][fc]:8s} init_fuse=0x{init[ci][fc]:02x} ({signmag(init[ci][fc],6):+d} steps) -> {fcv[fc]} uV"
            if st and st['adj'][ci][fc] is not None:
                line += f" | stock adjusted {st['adj'][ci][fc]} {'ok' if abs(st['adj'][ci][fc]-fcv[fc]) <= 4000 else 'DIFF'}"
                ok &= abs(st['adj'][ci][fc] - fcv[fc]) <= 4000
            print(line)
        for k, (f, v, flo, ceil, raw) in enumerate(rows):
            clamp = '' if v == -(-raw // STEP) * STEP else f' (raw {raw} clamped)'
            line = f"  {name} corner{k+1} {f/1e6:7.1f} MHz -> open-loop {v} uV [floor {flo}, ceil {ceil}]{clamp}"
            sc = st['corners'][ci].get(k + 1) if st else None
            if sc:
                good = abs(sc[0] - v) <= 4000
                line += f" | stock {sc[0]} [{sc[1]}, {sc[2]}] {'ok' if good else 'DIFF'}"
                ok &= good
            print(line)
            if not (500000 <= v <= 1100000):
                ok = False
    if a.mainline:
        seen, mok = 0, True
        for line in open(a.mainline, errors='replace'):
            mm = re.search(r'thread(\d)\b.*?corner (\d+) - uV=\[(-?\d+) (-?\d+) (-?\d+)\] quot=(-?\d+) freq=(\d+)', line)
            if not mm:
                continue
            ci, uv, freq = int(mm.group(1)), int(mm.group(4)), int(mm.group(7))
            if ci > 1:
                continue
            name = ('pwrcl', 'perfcl')[ci]
            exp = [r for r in res[name][1] if r[0] == freq]
            if not exp:
                print(f"  [mainline] {name} corner {mm.group(2)} freq {freq}: not a stock frequency -> MISMATCH"); mok = False; continue
            f, v, flo, ceil, raw = exp[0]
            good = abs(uv - v) <= 4000 and uv <= ceil and uv >= 500000
            print(f"  [mainline] {name} {freq/1e6:7.1f} MHz uV={uv} [min {mm.group(3)} max {mm.group(5)}] expected {v} [floor {flo}, ceil {ceil}] {'ok' if good else 'DIFF'}")
            mok &= good; seen += 1
        if not seen:
            print("  [mainline] no 'corner N - uV=[...]' lines found"); mok = False
        print("A6L_CPR_MAINLINE_" + ("MATCH" if mok else "MISMATCH") + f" corners={seen}")
        ok &= mok
    if st and not any(st['corners']):
        print("  (no per-corner stock table: run a6l-stock-cpr-read.sh with CORNERS=1 for the corner cross-check)")
    print("A6L_CPR_OPENLOOP_" + ("SANE" if ok else "OUT_OF_RANGE_OR_MISMATCH"))
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
