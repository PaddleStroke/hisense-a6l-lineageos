#!/usr/bin/env python3
"""Offline self-test for a6l_cpr_openloop.py stock-log path (agent power 26 Sep): synthetic fuses -> synthetic stock
log (same printk formats as msm-4.4 cprh-kbss) -> recovered fuses must match; emitted dump must round-trip."""
import os, random, struct, subprocess, sys, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import a6l_cpr_openloop as m
random.seed(660)
d = tempfile.mkdtemp()
fails = 0
for trial in range(20):
    rows = [0] * m.NROWS
    rows[5] = random.getrandbits(64)            # junk outside the CPR rows must be ignored
    sb, rev = random.choice([0, 1, 3, 4]), random.randint(0, 7)
    m.put_bits(rows, m.SPEED_BIN, sb); m.put_bits(rows, m.FUSE_REV, rev)
    init = [[m.to_signmag(random.randint(-4, 4), 6) for _ in range(5)] for _ in range(2)]
    quot = [[random.randint(600, 1800) for _ in range(5)] for _ in range(2)]
    ros = [[random.randint(0, 7) for _ in range(5)] for _ in range(2)]
    for ci in range(2):
        for fc in range(5):
            m.put_bits(rows, m.INIT[ci][fc], init[ci][fc]); m.put_bits(rows, m.QUOT[ci][fc], quot[ci][fc])
            m.put_bits(rows, m.ROSEL[ci][fc], ros[ci][fc])
    dump = os.path.join(d, f'f{trial}.bin'); open(dump, 'wb').write(b''.join(struct.pack('<Q', r) for r in rows))
    res = m.corners(init)
    log = [f"[    1.0] apc0: apc0_pwrcl_corner: speed bin = {sb}", f"[    1.0] apc0: apc0_pwrcl_corner: CPR fusing revision = {rev}",
           f"[    1.0] apc1: apc1_perfcl_corner: speed bin = {sb}"]
    for ci, (ctl, vr) in enumerate((('apc0', 'apc0_pwrcl_corner'), ('apc1', 'apc1_perfcl_corner'))):
        for fc in range(5):
            v = m.REF[ci][fc] + m.signmag(init[ci][fc], 6) * 10000
            log.append(f"[    1.1] {ctl}: {vr}: fused {m.FC_NAMES[ci][fc]:>8s}: open-loop={v:7d} uV")
            log.append(f"[    1.1] {ctl}: {vr}: fused {m.FC_NAMES[ci][fc]:>8s}: quot[{ros[ci][fc]:2d}]={quot[ci][fc]:4d}, quot_offset[{ros[ci][fc]:2d}]=   0")
        for k, (f, v, flo, ceil, raw) in enumerate(res[('pwrcl', 'perfcl')[ci]][1]):
            log.append(f"A6L_STOCK CORNER {ctl}/thread0/{vr} {k+1} open_loop={v} floor={flo} ceiling={ceil} last={v} quots=0")
    lp = os.path.join(d, f's{trial}.txt'); open(lp, 'w').write('\n'.join(log) + '\n')
    emit = os.path.join(d, f'e{trial}.bin')
    r = subprocess.run([sys.executable, m.__file__, dump, '--offset', '0', '--stock-log', lp, '--emit-dump', emit], capture_output=True, text=True)
    ok = r.returncode == 0 and 'A6L_CPR_DUMP_VS_STOCK MATCH' in r.stdout and 'A6L_CPR_OPENLOOP_SANE' in r.stdout
    e = open(emit, 'rb').read()
    for p in [m.SPEED_BIN, m.FUSE_REV] + [t[ci][fc] for t in (m.INIT, m.QUOT, m.ROSEL) for ci in range(2) for fc in range(5)]:
        if m.bits(e, 0, p) != m.bits(open(dump, 'rb').read(), 0, p):
            ok = False
    if not ok:
        fails += 1; print(r.stdout[-1500:], r.stderr[-500:])
# a mismatching corner must be reported
open(lp, 'a').write("A6L_STOCK CORNER apc0/thread0/apc0_pwrcl_corner 3 open_loop=900000 floor=596000 ceiling=900000 last=0 quots=0\n")
r = subprocess.run([sys.executable, m.__file__, '--stock-log', lp], capture_output=True, text=True)
if r.returncode == 0 or 'DIFF' not in r.stdout: fails += 1; print('mismatch not detected')
# a rotated log must fail cleanly
open(lp, 'w').write("nothing\n")
r = subprocess.run([sys.executable, m.__file__, '--stock-log', lp], capture_output=True, text=True)
if r.returncode == 0 or 'boot log rotated' not in (r.stdout + r.stderr): fails += 1; print('rotated log not detected')
print('A6L_CPR_STOCK_SELFTEST ' + ('PASS' if not fails else f'FAIL {fails}'))
sys.exit(1 if fails else 0)
