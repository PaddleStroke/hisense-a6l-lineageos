#!/usr/bin/env python3
"""kvoice (24 Sep 2026): offline check of the A6L call-audio DT in a MERGED dtb.
Checks: APR has qcom,q6mvm(reg 9, with a qcom,q6voice-dais child, #sound-dai-cells=1), qcom,q6cvs(0xa), qcom,q6cvp(0xb);
the sdm660 sound card has a "VoiceMMode1" link whose cpu is <&q6voicedai 1>; link 0 is MultiMedia1; every FE
(cpu = q6asm-dais or q6voice-dais) comes before the first BE (cpu = q6afe-dais).
usage: check-voice-dt-links.py merged.dtb   -> prints A6L_VOICE_DT_CHECK PASS|FAIL; exit 0/1"""
import subprocess, sys, re
dts = subprocess.run(['dtc', '-q', '-I', 'dtb', '-O', 'dts', sys.argv[1]], capture_output=True, text=True, check=True).stdout
L = dts.splitlines()
def blocks(pat):
    res = []
    for i, l in enumerate(L):
        if re.search(pat, l):
            j = i
            while not L[j].rstrip().endswith('{'): j -= 1
            d = 0; out = []
            for l2 in L[j:]:
                out.append(l2); d += l2.count('{') - l2.count('}')
                if d == 0: break
            res.append(out)
    return res
def phandle(b):
    # phandle of the block's own node = first phandle at depth 1
    d = 0
    for l in b:
        if d == 1:
            m = re.match(r'\s*phandle = <(0x[0-9a-f]+)>;', l)
            if m: return int(m.group(1), 16)
        d += l.count('{') - l.count('}')
    return None
def prop(b, name):
    d = 0
    for l in b:
        if d == 1:
            m = re.match(r'\s*%s = (.*);' % re.escape(name), l)
            if m: return m.group(1)
        d += l.count('{') - l.count('}')
    return None
bad = []
def one(pat, what):
    b = blocks(pat)
    if len(b) != 1: bad.append(f'{what}: found {len(b)} nodes'); return None
    return b[0]
mvm = one(r'compatible = "qcom,q6mvm"', 'q6mvm'); cvs = one(r'compatible = "qcom,q6cvs"', 'q6cvs'); cvp = one(r'compatible = "qcom,q6cvp"', 'q6cvp')
for b, want, n in ((mvm, 9, 'q6mvm'), (cvs, 10, 'q6cvs'), (cvp, 11, 'q6cvp')):
    if b:
        r = prop(b, 'reg'); print(f'{n}: {b[0].strip()} reg={r} pd={prop(b, "qcom,protection-domain")}')
        if r is None or int(re.findall(r'0x[0-9a-f]+', r)[0], 16) != want: bad.append(f'{n} reg {r} != {want}')
vd = one(r'compatible = "qcom,q6voice-dais"', 'q6voice-dais')
vd_ph = phandle(vd) if vd else None
if vd and (prop(vd, '#sound-dai-cells') or '').strip('<>') not in ('0x01', '1'): bad.append('q6voice-dais #sound-dai-cells != 1')
if mvm and vd and vd[0].strip() not in '\n'.join(mvm): bad.append('q6voice-dais is not a child of q6mvm')
asm = one(r'compatible = "qcom,q6asm-dais"', 'q6asm-dais'); afe = one(r'compatible = "qcom,q6afe-dais"', 'q6afe-dais')
asm_ph = phandle(asm) if asm else None; afe_ph = phandle(afe) if afe else None
snd = one(r'compatible = "qcom,sdm660-sndcard"', 'sound')
links = []; cur = None; sub = None
for l in (snd or [])[1:]:
    s = l.strip(); m = re.match(r'([\w@-]+) \{$', s)
    if m and cur is None: cur = {'node': m.group(1), 'name': None}; continue
    if m and cur is not None: sub = m.group(1); continue
    if s.startswith('link-name'): cur['name'] = re.search(r'"(.*)"', s).group(1)
    if s.startswith('sound-dai') and sub: cur[sub] = [int(x, 16) for x in re.findall(r'0x[0-9a-f]+', s)]
    if s == '};':
        if sub: sub = None
        elif cur: links.append(cur); cur = None
first_be = None; last_fe = None; voice = None
for i, k in enumerate(links):
    cpu = k.get('cpu', [])
    kind = 'FE-asm' if cpu[:1] == [asm_ph] else 'FE-voice' if cpu[:1] == [vd_ph] else 'BE' if cpu[:1] == [afe_ph] else '??'
    print(f"link {i}: {k['name']!r:26} node={k['node']:28} {kind} cpu-args={cpu[1:]}")
    if kind.startswith('FE'): last_fe = i
    if kind == 'BE' and first_be is None: first_be = i
    if k['name'] == 'VoiceMMode1':
        voice = i
        if kind != 'FE-voice' or cpu[1:] != [1]: bad.append('VoiceMMode1 link cpu is not <&q6voicedai 1>')
if voice is None: bad.append('no VoiceMMode1 link')
if not links or links[0]['name'] != 'MultiMedia1': bad.append('link 0 is not MultiMedia1 (HAL expects pcmC0D0 = MultiMedia1)')
if first_be is not None and last_fe is not None and first_be < last_fe: bad.append('a BE link precedes an FE link')
for b in bad: print('  FAIL:', b)
print('A6L_VOICE_DT_CHECK', 'PASS' if not bad else f'FAIL ({len(bad)})', f'voice_pcm_device={voice}')
sys.exit(1 if bad else 0)
