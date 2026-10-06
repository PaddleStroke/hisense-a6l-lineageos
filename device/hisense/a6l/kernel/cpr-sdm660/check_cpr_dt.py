#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# check_cpr_dt.py (cpufreq-watchdog agent, 29 Sep 2026): offline cross-check of a merged CPR/OSM cpufreq DTB
# (rom-v2 + a6l-cpufreq-f2a or a6l-cpufreq-v75) against the STOCK SDM660 DT (msm-4.4 clk-cpu-osm LUT + cprh-kbss maps).
# Read-only, host only. Checks, per cluster:
#  1. every OPP frequency is a row of the stock OSM LUT in EVERY speed bin (0/1/3/4) with the same pll_override and
#     spare-data; qcom,pll-div <=> freq_data bits 25:24; L-val*19.2 MHz == freq for PLL-sourced rows;
#  2. the OPP's required CPRh level == the stock LUT virtual corner;
#  3. each CPRh level's qcom,opp-fuse-level <pwr perf> == the fuse corner the stock qcom,cpr-corner-fmax-map gives;
#  4. highest OPP <= --max-pwr/--max-perf (kHz): the conservative first-boot cap;
#  5. cpu@1xx -> perfcl table / freq-domain 1 / apc_cprh 1, cpu@0xx -> pwrcl / 0 / 0;
#  6. every CPR nvmem cell lies in qfprom rows 38 and 65..71 (the rows the stock kernel reads; rows 0..37 reset the phone);
#  7. cpufreq@179c1000 / power-controller@179c8000 present with the sdm660 compatibles.
# Prints A6L_CPR_DT_CHECK PASS or lists every FAIL.
import argparse, re, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--stock', required=True); ap.add_argument('--dtb', required=True)
ap.add_argument('--max-pwr', type=int, required=True); ap.add_argument('--max-perf', type=int, required=True)
a = ap.parse_args()
fails = []
def fail(m): fails.append(m); print('FAIL', m)

st = open(a.stock, errors='replace').read()
def cells(name):
    m = re.search(r'\b' + re.escape(name) + r'\s*=\s*<([^>]*)>', st)
    return [int(x, 16) for x in m.group(1).split()] if m else None
LUT = {}
for cl in ('pwrcl', 'perfcl'):
    bins = {}
    for b in (0, 1, 3, 4):
        v = cells(f'qcom,{cl}-speedbin{b}-v0'); assert v and len(v) % 5 == 0, (cl, b)
        bins[b] = {v[i]: tuple(v[i + 1:i + 5]) for i in range(0, len(v), 5)}
    LUT[cl] = bins
# CPRh fmax maps (first speed-bin group; all non-zero groups must be equal)
FMAX = {}
for ctrl, cl in (('cprh-ctrl@179c8000', 'pwrcl'), ('cprh-ctrl@179c4000', 'perfcl')):
    i = st.index(ctrl + ' {'); seg = st[i:i + 6000]
    v = [int(x, 16) for x in re.search(r'qcom,cpr-corner-fmax-map = <([^>]*)>', seg).group(1).split()]
    fc = int(re.search(r'qcom,cpr-fuse-corners = <([^>]*)>', seg).group(1), 16)
    groups = [tuple(v[j:j + fc]) for j in range(0, len(v), fc) if any(v[j:j + fc])]
    if len(set(groups)) != 1: fail(f'{cl}: stock fmax-map differs between speed bins {set(groups)}')
    FMAX[cl] = groups[0]
def fuse_corner(cl, corner):   # 1-based corner -> 1-based fuse corner
    for k, fmax in enumerate(FMAX[cl]):
        if corner <= fmax: return k + 1
    return None

def fg(*args, t=None):
    cmd = ['fdtget'] + (['-t', t] if t else []) + [a.dtb] + list(args)
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None
def sym(s): return fg('/__symbols__', s)
def ints(node, prop, t='u'):
    v = fg(node, prop, t=t); return None if v is None else [int(x, 16 if t == 'x' else 10) for x in v.split()]
def subnodes(node): v = subprocess.run(['fdtget', '-l', a.dtb, node], capture_output=True, text=True).stdout.split(); return v

cprh = sym('cprh_opp_table'); assert cprh, 'no cprh_opp_table symbol'
LEVEL_BY_PH = {}; FUSE_LEVEL = {}
for n in subnodes(cprh):
    p = f'{cprh}/{n}'; ph = ints(p, 'phandle'); lv = ints(p, 'opp-level')[0]
    if ph: LEVEL_BY_PH[ph[0]] = lv
    FUSE_LEVEL[lv] = ints(p, 'qcom,opp-fuse-level')
print(f'cprh levels {sorted(FUSE_LEVEL)}')
for lv, (fp, fq) in sorted(FUSE_LEVEL.items()):
    ep = fuse_corner('pwrcl', lv) if lv <= 8 else None; eq = fuse_corner('perfcl', lv) if lv <= 7 else None
    ok = (ep is None or fp == ep) and (eq is None or fq == eq)
    print(f'  level {lv}: fuse-level <{fp} {fq}> stock <{ep} {eq}> {"ok" if ok else "DIFF"}')
    if not ok: fail(f'cprh level {lv} fuse-level <{fp} {fq}> != stock <{ep} {eq}>')

caps = {'pwrcl': a.max_pwr, 'perfcl': a.max_perf}
for cl in ('pwrcl', 'perfcl'):
    t = sym(f'{cl}_opp_table'); assert t, cl
    hi = 0
    for n in subnodes(t):
        p = f'{t}/{n}'; hz = ints(p, 'opp-hz'); hz = (hz[0] << 32) | hz[1]
        khz = hz // 1000; hi = max(hi, khz)
        rows = [LUT[cl][b].get(hz) for b in (0, 1, 3, 4)]
        if any(r is None for r in rows) or len(set(rows)) != 1:
            fail(f'{cl} {khz} kHz not identical in all stock speed bins: {rows}'); continue
        fdata, ovr, spare, corner = rows[0]
        req = ints(p, 'required-opps'); lv = LEVEL_BY_PH.get(req[0]) if req else None
        o = ints(p, 'qcom,pll-override'); s = ints(p, 'qcom,spare-data'); d = ints(p, 'qcom,pll-div')
        src, div, lval = (fdata >> 26) & 3, (fdata >> 24) & 3, fdata & 0xff
        errs = []
        if not o or o[0] != ovr: errs.append(f'pll-override {o} != stock {ovr:#x}')
        if not s or s[0] != spare: errs.append(f'spare {s} != stock {spare}')
        if (d[0] if d else 0) != div: errs.append(f'pll-div {d} != stock div {div}')
        if lv != corner: errs.append(f'cprh level {lv} != stock corner {corner}')
        if src == 1 and lval * 19200000 != hz: errs.append(f'lval {lval} * 19.2 MHz != {hz}')
        print(f'  {cl} {khz:>8} kHz corner {corner} lvl {lv} ovr {ovr:#x} spare {spare} div {div} src {src} lval {lval}: {"ok" if not errs else "; ".join(errs)}')
        for e in errs: fail(f'{cl} {khz}: {e}')
    print(f'  {cl} max {hi} kHz (cap {caps[cl]})')
    if hi > caps[cl]: fail(f'{cl} max {hi} kHz > cap {caps[cl]}')

cf = '/soc@0/cpufreq@179c1000'; pc = '/soc@0/power-controller@179c8000'
if 'qcom,sdm660-cpufreq-hw' not in (fg(cf, 'compatible') or ''): fail('cpufreq@179c1000 missing or wrong compatible')
if 'qcom,sdm660-cprh' not in (fg(pc, 'compatible') or ''): fail('power-controller@179c8000 missing or wrong compatible')
cfph = ints(cf, 'phandle'); pcph = ints(pc, 'phandle')
tabs = {cl: ints(sym(f'{cl}_opp_table'), 'phandle')[0] for cl in ('pwrcl', 'perfcl')}
for c in subnodes('/cpus'):
    if not c.startswith('cpu@'): continue
    p = f'/cpus/{c}'; reg = ints(p, 'reg'); mp = reg[-1]
    cl, dom = ('perfcl', 1) if mp & 0x100 else ('pwrcl', 0)
    got = (ints(p, 'operating-points-v2'), ints(p, 'qcom,freq-domain'), ints(p, 'power-domains'))
    exp = ([tabs[cl]], [cfph[0], dom], [pcph[0], dom])
    ok = got == exp; print(f'  {c} mpidr {mp:#x} -> {cl} domain {dom}: {"ok" if ok else f"DIFF {got} != {exp}"}')
    if not ok: fail(f'{c} links {got} != {exp}')

qf = sym('qfprom'); allowed = [(0x4130, 0x4138), (0x4208, 0x4240)]
ncell = 0
for n in subnodes(qf):
    if not re.match(r'(cpr-|quot|qoff|ivolt|rosel)', n): continue
    off, ln = ints(f'{qf}/{n}', 'reg'); ncell += 1
    if not any(lo <= off and off + ln <= hi for lo, hi in allowed): fail(f'nvmem cell {n} {off:#x}+{ln} outside rows 38/65..71')
print(f'  {ncell} CPR nvmem cells, all in qfprom rows 38 and 65..71' if ncell else '  no CPR nvmem cells found')
if ncell != 40: fail(f'expected 40 CPR nvmem cells, found {ncell}')
print('A6L_CPR_DT_CHECK', 'PASS' if not fails else f'FAIL ({len(fails)})')
sys.exit(1 if fails else 0)
