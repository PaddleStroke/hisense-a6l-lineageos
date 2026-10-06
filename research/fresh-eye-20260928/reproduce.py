#!/usr/bin/env python3
"""WSL/Linux, entirely offline. Snapshot active inputs; modify/build only work/.
No ADB, relay submission, modem access, ROM build or active-source edits.
Run from repo root: python3 research/fresh-eye-20260928/reproduce.py
"""
from pathlib import Path
import difflib
import hashlib
import json
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
WORK = HERE / 'work'
WORK.mkdir(exist_ok=True)
log = []

def run(args, expected=0):
    p = subprocess.run([str(x) for x in args], capture_output=True, text=True)
    text = p.stdout + p.stderr
    print(text, end='', flush=True)
    log.append('$ ' + ' '.join(map(str, args)) + '\n' + text)
    (HERE / 'results.txt').write_text('\n'.join(log))
    assert p.returncode == expected, (args, p.returncode, expected)
    return text

radio_rel = Path('device/hisense/a6l/radio')
original = WORK / 'original'
candidate = WORK / 'candidate'
for d in (original, candidate):
    shutil.copytree(ROOT / radio_rel / 'qmi', d / 'qmi', dirs_exist_ok=True)
    shutil.copytree(ROOT / radio_rel / 'tests', d / 'tests', dirs_exist_ok=True)

manifest = {}
for p in (original / 'qmi').rglob('*'):
    if p.is_file():
        manifest[str(radio_rel / p.relative_to(original))] = hashlib.sha256(p.read_bytes()).hexdigest()

def replace_once(rel, old, new):
    p = candidate / rel
    s = p.read_text()
    assert s.count(old) == 1, (rel, old)
    p.write_text(s.replace(old, new))

replace_once('qmi/src/ims.cc',
    '        Reader r(*v);\n        std::string t = r.str8();\n        if (r.good()) s.errorText = t;',
    '        // Top-level string TLV: its length is the TLV length, with no inner prefix.\n'
    '        s.errorText.assign(v->begin(), v->end());')
for svc in ('IMSS', 'IMSA'):
    old = f'        rep("A6L_IMSDCM_{svc} bind sub=" + std::to_string(*sub) + ": " + r.describe());'
    replace_once('qmi/src/ims_setup.cc', old, old + '\n        if (!o.bound) return o;  // Do not use an unbound/default subscription after a failed bind.')
replace_once('tests/volte2_tests.cc', '    ri.str8(0x12, "Forbidden");',
    "    ri.raw(0x12, {'F', 'o', 'r', 'b', 'i', 'd', 'd', 'e', 'n'}); // top-level QMI string")

patch = ''
for rel in ['qmi/src/ims.cc', 'qmi/src/ims_setup.cc', 'tests/volte2_tests.cc']:
    patch += ''.join(difflib.unified_diff((original / rel).read_text().splitlines(True),
                    (candidate / rel).read_text().splitlines(True),
                    fromfile='a/' + str(radio_rel / rel), tofile='b/' + str(radio_rel / rel)))
(HERE / 'ims-review.patch').write_text(patch)

common = ['g++', '-std=c++17', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
          '-fsanitize=address,undefined']
for tree in (original, candidate):
    sources = sorted((tree / 'qmi/src').glob('*.cc'))
    args = common + ['-I' + str(tree / 'qmi/include'), '-I' + str(tree / 'tests')]
    binary = tree / 'review-tests'
    run(args + sources + [tree / 'tests/fake_modem.cc', HERE / 'ims_review_tests.cc', '-o', binary])
    run([binary], expected=1 if tree == original else 0)
    if tree == candidate:
        for suite in ('volte2', 'volte5'):
            binary = tree / (suite + '-tests')
            run(args + sources + [tree / 'tests/fake_modem.cc', tree / f'tests/{suite}_tests.cc', '-o', binary])
            run([binary])

# Reconstruct exactly the untested camfix6 source from the saved camfix5 snapshot.
cam = WORK / 'camfix6'
shutil.copytree(ROOT / '.relay/outbox/cam28/src5', cam, dirs_exist_ok=True)
script = ROOT / 'device/hisense/a6l/kernel/camera/patches/camfix6_patch.py'
manifest[str(script.relative_to(ROOT))] = hashlib.sha256(script.read_bytes()).hexdigest()
run(['python3', script, cam])
s = (cam / 'camss-vfe-4-8.c').read_text()
start = s.index('static void vfe_wm_frame_based(')
end = s.index('\n}\n', start) + 3
defines = '\n'.join(x for x in s.splitlines() if x.startswith('#define VFE_0_BUS_IMAGE_MASTER_n_WR_')
                    and any(k in x for k in ('ADDR_CFG(n)', 'FRM_BASED_SHIFT', 'IMAGE_SIZE(n)', 'BUFFER_CFG(n)')))
harness = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef uint32_t u32; typedef uint8_t u8;
static unsigned a6l_wm, a6l_v6;
struct vfe_device { unsigned char *base; };
static u32 readl_relaxed(void *p) { return *(u32*)p; }
static void writel_relaxed(u32 v, void *p) { *(u32*)p = v; }
static void vfe_reg_set(struct vfe_device *v, u32 o, u32 m) { writel_relaxed(readl_relaxed(v->base+o)|m,v->base+o); }
static void vfe_reg_clr(struct vfe_device *v, u32 o, u32 m) { writel_relaxed(readl_relaxed(v->base+o)&~m,v->base+o); }
'''
harness += defines + '\n' + s[start:end] + r'''
int main(void) {
    uint32_t regs[1024]; struct vfe_device v = { (unsigned char*)regs };
    for (unsigned wm = 4; wm <= 6; wm += 2) {
        for (unsigned burst = 0; burst <= 32; burst += 32) {
            memset(regs, 0, sizeof(regs)); a6l_wm = wm; a6l_v6 = burst | 128;
            vfe_wm_frame_based(&v, 0, 1);
            printf("WM=%u V6=%u BUFFER_CFG=%u\n", wm, a6l_v6, regs[0xc0/4]);
            if (regs[0xc0/4] != (wm == 4 ? 0u : burst ? 3u : 2u)) return 1;
        }
    }
    puts("REPRODUCED: burst3 is a no-op with sweep default WM=4; WM=6 exercises it.");
}
'''
(WORK / 'camera-register-test.c').write_text(harness)
run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', WORK / 'camera-register-test.c', '-o', WORK / 'camera-register-test'])
run([WORK / 'camera-register-test'])

# Run only wait_reg(), extracted verbatim. No source script top-level/device actions.
sh = (ROOT / radio_rel / 'tools/volte5-test.sh').read_text()
func = sh[sh.index('wait_reg() {'):sh.index('\nsummary() {')]
(WORK / 'stale-registration.txt').write_text('A6L_IMSDCM_IMSA REGISTERED (query)\n'
    'A6L_IMSDCM_IMSA_IND reg status=not-registered\nA6L_IMSDCM_IMSA GONE (service 33 withdrawn)\n')
probe = WORK / 'stale-registration-test.sh'
probe.write_text('cd "$(dirname "$0")"\nDLOG=stale-registration.txt\nLOG=stale-test-output.txt\n'
                 'log() { echo "$*"; }; mask() { cat; }; nap() { :; };\n' + func +
                 '\nwait_reg 0\n[ "$REGD" = 1 ] || exit 1\n'
                 'echo "REPRODUCED: a lost registration and withdrawn service still satisfy wait_reg"\n')
run(['sh', probe])
(HERE / 'source-hashes.json').write_text(json.dumps(manifest, indent=2) + '\n')
print('Offline review complete. Candidate patch NOT applied to active sources.')
