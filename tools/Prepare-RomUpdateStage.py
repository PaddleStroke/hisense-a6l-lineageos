#!/usr/bin/env python3
"""Add the "preserve userdata" update to a staged rom kit (agent update-keepdata, 29 Sep 2026). Offline, WSL. No phone.
usage: Prepare-RomUpdateStage.py <new kit dir> <previous kit dir> [--wipe-data]
  <kit dir> = /home/a6l/rom-v2/kit-<tag>/rom-v2 (made by Prepare-RomV2Stage.py: images/ + rom-v1 tools).
  The previous kit = the build that is INSTALLED on the phone (its images/ must still hold the erofs files).
Adds (never overwrites: refuses when <new kit>/update exists):
  <kit>/{RomUpdateLayoutV1,RomUpdateEngineV1,RomUpdateAdbV1}.py, Write-LaptopRomUpdateV1.py, Run-LaptopRomUpdate-v1.py,
  Launch-RomUpdateV1.py
  <kit>/update/rom-update-compat.json (new build), prev-rom-v1-pins.json + prev-rom-update-compat.json (installed build),
  rom-update-tools.json (hashes of the 9 tools the update uses), SHA256SUMS
Then runs the worker dry run for backup-only and update. Prints A6L_UPDATE_STAGE_PASS / A6L_UPDATE_STAGE_FAIL.
stage-rom-v2-kit-laptop.sh copies the whole kit dir (update/ included) and checks every file on the laptop.
--wipe-data (user-build-20261012/wipe-mode): also writes update/wipe-data.json (pinned in SHA256SUMS; the worker refuses
  --wipe-data on a kit without it), and the dry runs are backup-only and update with --wipe-data --transport adb-recovery.
  The keep-data update dry run is still run and its result printed (for a test-keys -> release-keys kit it must fail).
"""
import hashlib, importlib.util, json, shutil, subprocess, sys
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
NEW_TOOLS = ['RomUpdateLayoutV1.py', 'RomUpdateEngineV1.py', 'RomUpdateAdbV1.py', 'Write-LaptopRomUpdateV1.py',
             'Run-LaptopRomUpdate-v1.py', 'Launch-RomUpdateV1.py']
USED_V1 = ['RomFlashLayoutV1.py', 'RomFlashEngineV1.py', 'Write-LaptopRomV1.py']


def sha(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 22), b''):
            h.update(b)
    return h.hexdigest()


def main():
    argv = sys.argv[1:]
    wipe = '--wipe-data' in argv
    argv = [x for x in argv if x != '--wipe-data']
    if len(argv) != 2:
        sys.exit(__doc__)
    kit, prev = Path(argv[0]), Path(argv[1])
    prev_images = prev / 'images' if (prev / 'images').is_dir() else prev
    upd = kit / 'update'
    if upd.exists():
        sys.exit('A6L_UPDATE_STAGE_FAIL %s exists (never overwritten; stage a new kit dir)' % upd)
    for t in USED_V1:
        if not (kit / t).is_file():
            sys.exit('A6L_UPDATE_STAGE_FAIL kit lacks ' + t)
        if sha(kit / t) != sha(TOOLS / t) and (kit / t).read_bytes() != (TOOLS / t).read_bytes().replace(b'\r\n', b'\n'):
            print('NOTE kit %s differs from tools/%s (kit keeps its own copy; pinned as staged)' % (t, t))
    spec = importlib.util.spec_from_file_location('compat', TOOLS / 'Make-RomUpdateCompat.py')
    c = importlib.util.module_from_spec(spec); spec.loader.exec_module(c)
    new_c = c.describe(kit / 'images')
    prev_c = c.describe(prev_images)
    for t in NEW_TOOLS:
        dst = kit / t
        if dst.exists():
            sys.exit('A6L_UPDATE_STAGE_FAIL %s exists' % dst)
        dst.write_bytes((TOOLS / t).read_bytes().replace(b'\r\n', b'\n'))
    upd.mkdir()
    (upd / 'rom-update-compat.json').write_text(json.dumps(new_c, indent=2) + '\n')
    (upd / 'prev-rom-update-compat.json').write_text(json.dumps(prev_c, indent=2) + '\n')
    shutil.copyfile(prev_images / 'rom-v1-pins.json', upd / 'prev-rom-v1-pins.json')
    (upd / 'rom-update-tools.json').write_text(json.dumps({'files': {t: sha(kit / t) for t in NEW_TOOLS + USED_V1}}, indent=2) + '\n')
    if wipe:
        (upd / 'wipe-data.json').write_text(json.dumps({
            'wipe_data': True, 'prev': prev_c['system_fingerprint'], 'new': new_c['system_fingerprint'],
            'why': 'test-keys <-> release-keys switch: /data of the installed build cannot be booted by the new one',
            'zeroes': 'first MiB of metadata and of userdata (fs_mgr formattable -> formatted at the first boot)'}, indent=2) + '\n')
    files = sorted(p.name for p in upd.iterdir() if p.name != 'SHA256SUMS')
    (upd / 'SHA256SUMS').write_text(''.join(f'{sha(upd / f)}  {f}\n' for f in files))
    ok = True
    extra = ['--wipe-data', '--transport', 'adb-recovery'] if wipe else []
    for mode in ('backup-only', 'update'):
        r = subprocess.run([sys.executable, str(kit / 'Write-LaptopRomUpdateV1.py'), '--mode', mode, '--dry-run'] + extra,
                           capture_output=True, text=True)
        print(r.stdout, end='')
        ok &= r.returncode == 0
    if wipe:
        r = subprocess.run([sys.executable, str(kit / 'Write-LaptopRomUpdateV1.py'), '--mode', 'update', '--dry-run'],
                           capture_output=True, text=True)
        print(r.stdout, end='')
        print('A6L_UPDATE_STAGE_KEEPDATA_DRYRUN %s (expected FAIL for a test-keys <-> release-keys kit)'
              % ('PASS' if r.returncode == 0 else 'FAIL'))
    print('A6L_UPDATE_STAGE_%s%s prev=%s new=%s' % ('PASS' if ok else 'FAIL', ' wipe-data' if wipe else '',
                                                     prev_c['build_date_utc'], new_c['build_date_utc']))
    return 0 if ok else 1


if __name__ == '__main__':
    raise SystemExit(main())
