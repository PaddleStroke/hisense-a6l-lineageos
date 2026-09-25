"""A6L rom-v1 install / restore engine (agent flash, 24 Sep 2026). Device-independent: the same code runs against the
real Firehose connection (FirehoseDevice, laptop worker) and against a sparse virtual eMMC file (FileDevice, offline
tests). No retries: any failed check or transfer stops the run and leaves the phone in EDL for review.

install: GPT/identity checks -> backup (read) every region it writes + the regions the ROM may write -> independent
         second read of the small regions / stock-hash match for system+vendor+dtbo -> write boot, dtbo, vendor, system,
         userdata head zero -> full readback of every written range -> power off.
restore: backup manifest hash check -> GPT check -> write back boot, dtbo, vendor, system (full partitions) and the
         ROM-writable regions that changed -> zero userdata head + stock FDE footer -> full readback -> reset.
"""
import hashlib
import json
import os
from pathlib import Path

import RomFlashLayoutV1 as L

CHUNK = 1 << 20


class StopRun(Exception):
    pass


class FileDevice:
    """Offline stand-in for the phone: a (sparse) file with the real eMMC geometry."""
    def __init__(self, path, fail_program_at=None):
        self.path = Path(path)
        self.log = []
        self.fail_program_at = fail_program_at

    def read(self, start, sectors, out_path):
        self.log.append(('read', start, sectors))
        with open(self.path, 'rb') as src, open(out_path, 'wb') as dst:
            src.seek(start * L.SECTOR)
            left = sectors * L.SECTOR
            while left:
                b = src.read(min(CHUNK * 16, left))
                if not b:
                    raise StopRun('virtual disk short read')
                dst.write(b)
                left -= len(b)

    def program(self, start, sectors, stream):
        self.log.append(('program', start, sectors))
        if self.fail_program_at is not None and len([x for x in self.log if x[0] == 'program']) == self.fail_program_at:
            raise StopRun('injected transfer failure')
        with open(self.path, 'r+b') as dst:
            dst.seek(start * L.SECTOR)
            left = sectors * L.SECTOR
            for b in stream:
                if len(b) > left:
                    raise StopRun('payload longer than the programmed range')
                dst.write(b)
                left -= len(b)
            if left:
                raise StopRun('payload shorter than the programmed range')

    def power(self, value):
        self.log.append(('power', value))


def file_stream(path, nbytes):
    with open(path, 'rb') as f:
        left = nbytes
        while left:
            b = f.read(min(CHUNK, left))
            if not b:
                raise StopRun('payload file shorter than expected: %s' % path)
            yield b
            left -= len(b)


def zero_stream(nbytes):
    z = bytes(CHUNK)
    left = nbytes
    while left:
        n = min(CHUNK, left)
        yield z[:n]
        left -= n


class Session:
    def __init__(self, device, capture, report, save):
        self.dev, self.capture, self.report, self.save = device, Path(capture), report, save

    def read_region(self, label, start, sectors, suffix=''):
        out = self.capture / f'{label}{suffix}.bin'
        if out.exists():
            raise StopRun('refusing to overwrite capture file ' + out.name)
        self.dev.read(start, sectors, out)
        if out.stat().st_size != sectors * L.SECTOR:
            raise StopRun('read length differs: ' + label)
        digest = L.sha256_file(out)
        self.report.setdefault('reads', {})[label + suffix] = {'start_sector': start, 'sectors': sectors, 'sha256': digest}
        self.save()
        return out, digest

    def program(self, label, start, sectors, stream, expected_sha):
        entry = {'start_sector': start, 'sectors': sectors, 'expected_sha256': expected_sha, 'attempted': True, 'acknowledged': False}
        self.report.setdefault('writes', {})[label] = entry
        self.save()
        self.dev.program(start, sectors, stream)
        entry['acknowledged'] = True
        self.save()

    def verify_written(self, label, start, sectors, expected_sha):
        out, digest = self.read_region(label, start, sectors, '-readback')
        os.remove(out)   # keep the capture small; the hash is recorded
        ok = digest == expected_sha
        self.report['writes'][label]['readback_sha256'] = digest
        self.report['writes'][label]['readback_verified'] = ok
        self.save()
        if not ok:
            raise StopRun('readback differs from the written payload: ' + label)


def payload_sha(path, nbytes):
    h = hashlib.sha256()
    for b in file_stream(path, nbytes):
        h.update(b)
    return h.hexdigest()


def zero_sha(nbytes):
    h = hashlib.sha256()
    for b in zero_stream(nbytes):
        h.update(b)
    return h.hexdigest()


def check_disk_identity(s, kit_expected):
    """GPT primary+tail must equal the verified spare backup; layout parsed from the GPT must match the plan."""
    p, _ = s.read_region('gpt-primary', *L.GPT_PRIMARY)
    t, _ = s.read_region('gpt-tail', *L.GPT_TAIL)
    if kit_expected is not None:
        if p.read_bytes() != kit_expected['primary'] or t.read_bytes() != kit_expected['tail']:
            raise StopRun('GPT differs from the verified spare backup')
    L.check_layout_against_gpt(p.read_bytes())
    s.report['gpt_verified'] = True
    s.save()


def run_install(dev, capture, images, pins, kit_expected, report, save, allow_nonstock=False):
    """images: {'boot','dtbo','system','vendor'} -> (path, nbytes, sha256). pins: expected stock hashes for
    system/vendor/dtbo (full partitions)."""
    s = Session(dev, capture, report, save)
    for name, (path, nbytes, digest) in images.items():
        if payload_sha(path, nbytes) != digest:
            raise StopRun('payload hash differs before any USB write: ' + name)
    writes = L.install_writes({k: v[1] for k, v in images.items()})
    report['plan'] = [list(w) for w in writes]
    save()
    check_disk_identity(s, kit_expected)
    # --- pre-write state checks (read-only) ---
    _, vb = s.read_region('vbmeta', *L.PARTITIONS['vbmeta'])
    if vb != L.VBMETA_STOCK:
        raise StopRun('vbmeta differs from the bootloader-tested stock vbmeta')
    _, di = s.read_region('devinfo', *L.PARTITIONS['devinfo'])
    if di != L.DEVINFO_UNLOCKED:
        raise StopRun('devinfo differs from the verified unlocked baseline')
    # --- backups ---
    backups = {}
    for label, (start, sectors) in L.backup_regions().items():
        if label in ('vbmeta', 'devinfo'):
            backups[label] = report['reads'][label]['sha256']
            continue
        _, digest = s.read_region(label, start, sectors)
        backups[label] = digest
        print('backup ' + label + ' ' + digest[:16], flush=True)
    rec = backups['recovery']
    report['recovery_state'] = L.RECOVERY_KNOWN.get(rec, 'unknown')
    bcb = (Path(capture) / 'misc.bin').read_bytes()[:4096]
    if hashlib.sha256(bcb).hexdigest() not in (L.EMPTY_BCB, L.BOOTLOADER_BCB):
        raise StopRun('unknown boot message in misc; not installing over it')
    # independent verification of the backups: second read for the small regions, stock pins for the big ones
    for label in ('boot', 'dtbo', 'misc', 'metadata', 'modemst1', 'modemst2', 'fsg', 'fsc', 'persist', 'userdata-head', 'userdata-tail'):
        start, sectors = L.backup_regions()[label]
        out, digest = s.read_region(label, start, sectors, '-second-read')
        os.remove(out)
        if digest != backups[label]:
            raise StopRun('second read differs (unstable transfer?): ' + label)
    state = {}
    for label in ('system', 'vendor', 'dtbo'):
        if backups[label] == pins.get(label):
            state[label] = 'stock'
        elif backups[label] == pins.get('rom-' + label):
            state[label] = 'rom-v1'
        else:
            state[label] = 'other'
    report['predecessor'] = state
    if not allow_nonstock and set(state.values()) != {'stock'}:
        raise StopRun('system/vendor/dtbo are not the verified stock contents: %s (use --allow-nonstock only after review)' % state)
    manifest = {'backups': backups, 'layout': {k: list(v) for k, v in L.backup_regions().items()}, 'predecessor': state}
    (Path(capture) / 'backup-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    report['backup_complete'] = True
    save()
    # --- writes ---
    expected = {}
    for label, start, sectors in writes:
        if label.endswith('-zero'):
            expected[label] = zero_sha(sectors * L.SECTOR)
            s.program(label, start, sectors, zero_stream(sectors * L.SECTOR), expected[label])
        else:
            path, nbytes, digest = images[label]
            expected[label] = digest
            s.program(label, start, sectors, file_stream(path, nbytes), digest)
        print('written ' + label, flush=True)
    for label, start, sectors in writes:
        s.verify_written(label, start, sectors, expected[label])
        print('readback ok ' + label, flush=True)
    report['readback_verified'] = True
    save()
    dev.power('off')
    report['power'] = 'off'
    save()


def run_restore(dev, capture, backup_dir, kit_expected, report, save, userdata='zero'):
    """backup_dir: the install capture's edl directory (backup-manifest.json + <label>.bin)."""
    s = Session(dev, capture, report, save)
    backup_dir = Path(backup_dir)
    manifest = json.loads((backup_dir / 'backup-manifest.json').read_text())
    for label, digest in manifest['backups'].items():
        if label in ('vbmeta', 'devinfo'):
            continue
        if L.sha256_file(backup_dir / f'{label}.bin') != digest:
            raise StopRun('backup file differs from its manifest: ' + label)
    report['backup_files_verified'] = True
    save()
    check_disk_identity(s, kit_expected)
    changed = set()
    for label in L.ROM_MAY_WRITE:
        _, digest = s.read_region(label, *L.PARTITIONS[label], '-now')
        if digest != manifest['backups'][label]:
            changed.add(label)
    report['rom_changed'] = sorted(changed)
    writes = L.restore_writes(changed)
    if userdata == 'original':
        writes = [w for w in writes if not w[0].startswith('userdata')]
        ud = manifest['layout']
        writes += [('userdata-head', *ud['userdata-head']), ('userdata-tail', *ud['userdata-tail'])]
    report['plan'] = [list(w) for w in writes]
    save()
    expected = {}
    for label, start, sectors in writes:
        if label.endswith('-zero'):
            expected[label] = zero_sha(sectors * L.SECTOR)
            s.program(label, start, sectors, zero_stream(sectors * L.SECTOR), expected[label])
        else:
            expected[label] = manifest['backups'][label]
            s.program(label, start, sectors, file_stream(backup_dir / f'{label}.bin', sectors * L.SECTOR), expected[label])
        print('restored ' + label, flush=True)
    for label, start, sectors in writes:
        s.verify_written(label, start, sectors, expected[label])
        print('readback ok ' + label, flush=True)
    report['readback_verified'] = True
    save()
    dev.power('reset')
    report['power'] = 'reset'
    save()
