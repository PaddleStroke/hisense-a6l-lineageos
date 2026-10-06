#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# eink-round4: the mirror captures the next page while a6l_epdd still drives (--dry simulates the drive time) and sends it
# at the reply. Checks a --dry stock-mode log: pipelined decisions happen, the next command follows the reply at once
# (before: capture + resize after every reply, ~240 ms on the phone), and never two commands are outstanding.
# usage: pipeline_check.py MIRROR_LOG
import re, sys
done_t = None; gaps = []; staged = 0; outstanding = 0; bad = 0
for line in open(sys.argv[1]):
    m = re.match(r'\[(\d+\.\d+)\] A6L_MIRROR (.*)', line)
    if line.startswith('A6L_MIRROR CMD frame'):
        outstanding += 1; bad += outstanding > 1; continue
    if not m: continue
    t, s = float(m.group(1)), m.group(2)
    if s.startswith('done (simulated)'): outstanding -= 1; done_t = t
    elif 'policy:' in s:
        staged += 'staged' in s
        if done_t is not None: gaps.append(t - done_t); done_t = None
pipelined = [g for g in gaps if g < 0.02]
ok = staged >= 5 and len(pipelined) >= 5 and bad == 0
print('PIPELINE_TEST %s staged=%d gaps_ms=%s overlapping_cmds=%d' % ('PASS' if ok else 'FAIL', staged, [round(g * 1000) for g in gaps], bad))
sys.exit(0 if ok else 1)
