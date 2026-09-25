#!/usr/bin/env python3
"""audio3 (24 Sep 2026): offline check of the A6L sound-card dai-links in a merged DTB.
Prints the link order, the q6asm-dais child order, and for every MultiMedia front end which q6asm DAI it gets
 - under the stock 7.2 rule (no of_xlate in q6asm-dai: <&q6asmdai N> = N-th child in DT order), and
 - under a6l-q6asm-dai-xlate-v75.patch (<&q6asmdai N> = the child whose reg == N).
Exit 1 if a link named MultiMedia<k> does not get DAI MultiMedia<k> under the selected rule (--rule index|reg|both),
if q6routing platforms come before the first FE link, or (--expect-tfa) if the speaker link/amp node is missing.
usage: check-audio-dt-links.py merged.dtb [--rule both] [--expect-tfa] [--expect-no-tfa]"""
import subprocess, sys, re
a = sys.argv[1:]
dtb = a[0]; rule = 'both'
if '--rule' in a: rule = a[a.index('--rule') + 1]
dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', dtb], capture_output=True, text=True, check=True).stdout
lines = dts.splitlines()
def block(start_pat):
    for i, l in enumerate(lines):
        if re.search(start_pat, l):
            # walk back to the node line that opened this block
            j = i
            while not lines[j].rstrip().endswith('{'): j -= 1
            depth = 0; out = []
            for l2 in lines[j:]:
                out.append(l2); depth += l2.count('{') - l2.count('}')
                if depth == 0: break
            return out
    return []
asm = block(r'compatible = "qcom,q6asm-dais"')
ph = re.search(r'phandle = <(0x[0-9a-f]+)>', '\n'.join(asm)); asm_ph = int(ph.group(1), 16) if ph else None
children = [int(m.group(1), 16) for m in re.finditer(r'^\s+reg = <(0x[0-9a-f]+)>;', '\n'.join(asm[1:]), re.M)]
rt = block(r'compatible = "qcom,q6adm-routing"'); rph = re.search(r'phandle = <(0x[0-9a-f]+)>', '\n'.join(rt))
rt_ph = int(rph.group(1), 16) if rph else None
snd = block(r'compatible = "qcom,sdm660-sndcard"')
print(f'q6asm-dais phandle={asm_ph:#x} children(reg) in DT order: {children}')
links = []; cur = None; sub = None
for l in snd[1:]:
    s = l.strip()
    m = re.match(r'([\w@-]+) \{$', s)
    if m and cur is None: cur = {'node': m.group(1), 'name': None, 'cpu': None, 'platform': None, 'codec': None}; continue
    if m and cur is not None: sub = m.group(1); continue
    if s.startswith('link-name'): cur['name'] = re.search(r'"(.*)"', s).group(1)
    if s.startswith('sound-dai') and sub: cur[sub] = [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', s)]
    if s == '};':
        if sub: sub = None
        elif cur: links.append(cur); cur = None
bad = 0; first_fe = None; first_rt = None
for i, k in enumerate(links):
    cpu = k['cpu'] or []
    desc = f"link {i}: {k['name']!r:28} node={k['node']}"
    if cpu and cpu[0] == asm_ph:
        n = cpu[1]; first_fe = i if first_fe is None else first_fe
        by_index = f'MultiMedia{children[n] + 1}' if n < len(children) else 'NONE(-EINVAL)'
        by_reg = f'MultiMedia{n + 1}' if n in children else 'NONE(-EINVAL)'
        desc += f'  FE <&q6asmdai {n}> -> stock(index): {by_index}   patched(reg): {by_reg}'
        want = k['name']
        if rule in ('index', 'both') and by_index != want: desc += '   <-- WRONG (index rule)'; bad += 1
        if rule in ('reg', 'both') and by_reg != want: desc += '   <-- WRONG (reg rule)'; bad += 1
    else:
        if k['platform'] and k['platform'][0] == rt_ph and first_rt is None: first_rt = i
        desc += f"  BE cpu={['%#x' % c for c in cpu]} codec={k['codec']}"
    print(desc)
if first_fe is None or (first_rt is not None and first_rt < first_fe):
    print('ORDER_FAIL: a q6routing BE link comes before the first q6asm FE link (card -19)'); bad += 1
tfa = 'nxp,tfa98xx' in dts; spk = any(k['name'] == 'Speaker Playback' for k in links)
print(f'tfa98xx node: {tfa}, Speaker Playback link: {spk}')
if '--expect-tfa' in a and not (tfa and spk): print('TFA_FAIL'); bad += 1
if '--expect-no-tfa' in a and spk: print('TFA_UNEXPECTED'); bad += 1
print('A6L_DT_CHECK', 'PASS' if bad == 0 else f'FAIL ({bad})', f'rule={rule}')
sys.exit(1 if bad else 0)
