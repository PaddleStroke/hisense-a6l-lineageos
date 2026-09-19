"""Exercise the actual read verifier on sparse host fixtures, never phone storage."""
import hashlib
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'device/hisense/a6l/diagnostic'
ARCHIVE = ROOT / 'firmware/extracted/storage-read-v37-20260917'
parser = argparse.ArgumentParser()
parser.add_argument('--attempt', type=int, default=1, choices=[1, 2])
args = parser.parse_args()
OUT = Path('/home/a6l/kernel/test-storage-read-v37' + ('' if args.attempt == 1 else '-r2'))
if args.attempt == 2:
    old = ARCHIVE / 'host-r1'
    old.mkdir(exist_ok=False)
    for p in ARCHIVE.iterdir():
        if p.is_file() and (p.suffix == '.log' or p.name in ['host-tests.json', 'main.c', 'storage_read_fixture.h']):
            (old / p.name).write_bytes(p.read_bytes())
OUT.mkdir(exist_ok=False)
manifest = json.loads((ARCHIVE / 'read-manifest.json').read_text())
disk = OUT / 'fixture.bin'
with disk.open('xb') as f:
    f.truncate(manifest['disk_bytes'])
    with (ROOT / 'firmware/raw-backup-20260914/emmc-firmware-prefix.bin').open('rb') as backup:
        for r in manifest['ranges']:
            if r['name'] == 'gpt-tail':
                data = (ROOT / 'firmware/raw-backup-20260914/emmc-gpt-tail.bin').read_bytes()
            else:
                backup.seek(r['offset'])
                data = backup.read(r['bytes'])
            assert hashlib.sha256(data).hexdigest() == r['sha256']
            f.seek(r['offset'])
            f.write(data)
device = OUT / 'c0c4000.mmc/mmc_host/mmc1/mmc1:0001'
device.mkdir(parents=True)
block = OUT / 'sysblock'
block.mkdir()
(block / 'device').symlink_to(device)
(block / 'dev').write_text('179:0\n')
(device / 'name').write_text('hDEaP3\n')
(device / 'manfid').write_text('0x000090\n')
header = '''#define READ_DEVICE "%s"
#define SYS_BLOCK "%s"
static int fixture_uname(struct utsname *u) {
    memset(u, 0, sizeof(*u));
    strcpy(u->release, getenv("BAD_KERNEL") ? "wrong" : "7.2.3-a6l-probe+");
    strcpy(u->machine, "aarch64");
    return 0;
}
static ssize_t fixture_pread(int fd, void *p, size_t n, off_t at) {
    if (getenv("SHORT_READ") && at == 343932928) return 0;
    return pread(fd, p, n, at);
}
#define uname fixture_uname
#define pread fixture_pread
''' % (disk, block)
(OUT / 'storage_read_fixture.h').write_text(header)
(OUT / 'main.c').write_text('''#include <stdio.h>
#include <stdarg.h>
int a6l_storage_verify(void (*emit)(const char *, ...));
static void emit(const char *f, ...) { va_list a; va_start(a,f); vprintf(f,a); va_end(a); }
int main(void) { return a6l_storage_verify(emit); }
''')
subprocess.run(['gcc', '-D_GNU_SOURCE', '-DA6L_STORAGE_HOST_TEST', '-O2', '-Wall', '-Wextra', '-Werror',
                '-Wno-deprecated-declarations', '-I', str(OUT), str(SOURCE / 'storage_read.c'),
                str(OUT / 'main.c'), '-lcrypto', '-o', str(OUT / 'verify')], check=True)
report = {'scope': 'Same reader source on sparse regular-file host fixture; production block geometry and real SDM660 I/O are not emulated',
          'source_sha256': hashlib.sha256((SOURCE / 'storage_read.c').read_bytes()).hexdigest(),
          'ranges_sha256': hashlib.sha256((SOURCE / 'storage_read_ranges.h').read_bytes()).hexdigest(), 'cases': {}}


def run(name, expected, marker, extra=None):
    env = dict(os.environ, **(extra or {}))
    p = subprocess.run([str(OUT / 'verify')], env=env, capture_output=True, text=True, timeout=30)
    (OUT / (name + '.log')).write_text(p.stdout + p.stderr)
    assert p.returncode == expected and marker in p.stdout, (name, p.returncode, p.stdout[-2000:])
    if expected:
        assert 'A6L_STORAGE_READ_PASS' not in p.stdout
    else:
        assert p.stdout.count('match=1') == 10
    report['cases'][name] = dict(passed=True, exit_code=p.returncode)


try:
    run('valid', 0, 'A6L_STORAGE_READ_PASS regions=5 passes=2')
    run('wrong-kernel', 1, 'gate=kernel', {'BAD_KERNEL': '1'})
    (device / 'name').write_text('wrong\n')
    run('wrong-product', 1, 'gate=product')
    (device / 'name').write_text('hDEaP3\n')
    (device / 'manfid').write_text('0x000015\n')
    run('wrong-manufacturer', 1, 'gate=manufacturer')
    (device / 'manfid').write_text('0x000090\n')
    (block / 'dev').write_text('179:64\n')
    run('wrong-device', 1, 'gate=controller')
    (block / 'dev').write_text('179:0\n')
    with disk.open('r+b') as f:
        f.truncate(manifest['disk_bytes'] + 512)
    run('wrong-capacity', 1, 'gate=capacity')
    with disk.open('r+b') as f:
        f.truncate(manifest['disk_bytes'])
        f.seek(343932928 + 1024)
        original = f.read(1)
        f.seek(343932928 + 1024)
        f.write(bytes([original[0] ^ 1]))
    run('corruption', 1, 'match=0')
    with disk.open('r+b') as f:
        f.seek(343932928 + 1024)
        f.write(original)
    run('short-read', 1, 'gate=read', {'SHORT_READ': '1'})
    run('valid-restored', 0, 'A6L_STORAGE_READ_PASS regions=5 passes=2')
    # Syscall tracing verifies the live host reader opens its device read-only,
    # with direct I/O, and issues no write/ioctl against that file descriptor.
    trace = OUT / 'syscalls.log'
    p = subprocess.run(['strace', '-yy', '-e', 'trace=openat,write,pwrite64,ioctl,pread64',
                        '-o', str(trace), str(OUT / 'verify')], capture_output=True, text=True, timeout=30)
    assert p.returncode == 0, p.stderr
    lines = trace.read_text().splitlines()
    opens = [x for x in lines if str(disk) in x and 'openat(' in x]
    assert len(opens) == 1 and 'O_RDONLY' in opens[0] and 'O_DIRECT' in opens[0]
    assert not any(str(disk) in x and x.startswith(('write(', 'pwrite64(', 'ioctl(')) for x in lines)
    report['read_only_syscall_trace'] = True
    report['passed'] = True
finally:
    for p in OUT.iterdir():
        if p.is_file() and p.suffix in ('.log', '.c', '.h'):
            (ARCHIVE / p.name).write_bytes(p.read_bytes())
    (ARCHIVE / 'host-tests.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
