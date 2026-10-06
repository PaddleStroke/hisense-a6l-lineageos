"""A6L "preserve userdata" update engine (agent update-keepdata, 29 Sep 2026). Device-independent like RomFlashEngineV1:
the same code runs over EDL/Firehose (Write-LaptopRomUpdateV1.py), over adb in the V74 diagnostic recovery
(RomUpdateAdbV1.AdbRecoveryDevice) and against a virtual eMMC file (tests). No retries: any failed check stops the run.

modes
  backup-only  rehearsal: every check of an update (GPT, vbmeta/devinfo, BCB, predecessor = the installed build,
               data compatibility of the new build, plan allowlist), full backup of boot/dtbo/vendor/system (+ second
               read of boot/dtbo), invariant-region hashes; NO program; optional full userdata copy; power reset.
  update       verify installed-image identity and compatibility, then write boot, dtbo, vendor, system ONLY (never userdata/metadata/persist/EFS/misc/recovery/
               vbmeta/devinfo/GPT), full readback of every written range, re-hash the invariant regions (must be
               byte-identical to the pre-write hashes), power off.
               r6b (30 Sep 2026): only the partitions that CHANGED are written: a partition whose new image is byte-identical
               to the installed (previous-kit) image, and whose head was just read back as that image, is left untouched
               (report 'unchanged_skipped'); --reflash writes all four again.
  rollback     from an update capture: write the four saved partitions back (keeps data), readback, invariants, reset.
Development CLI updates default to hash-only verification: no old partition snapshots are saved.
The engine API keeps its snapshot default for existing callers. Explicit snapshot/backup-only modes
retain partition copies; rollback requires such a snapshot. Restore a hash-only development update
by reflashing its retained previous image kit. Every mode writes backup-manifest.json and the report
into its own capture directory; nothing is ever overwritten.
"""
import hashlib
import json
import os
from pathlib import Path

import RomFlashLayoutV1 as L
import RomFlashEngineV1 as E
import RomUpdateLayoutV1 as U

StopRun = E.StopRun


def _checked(writes):
    try:
        U.check_update_plan(writes)
        if hasattr(L, 'check_plan'):   # kits staged before bug hunt round 2 (kit-r5) lack it; check_update_plan is stricter
            L.check_plan(writes)
    except ValueError as e:
        raise StopRun(str(e))


def load_payloads(images):
    """images: {'boot','dtbo','system','vendor'} -> (path, nbytes, sha256). Hash check before any device access."""
    if set(images) != set(U.UPDATE_WRITABLE):
        raise StopRun('update needs exactly boot, dtbo, vendor, system payloads')
    for name, (path, nbytes, digest) in images.items():
        if Path(path).stat().st_size != nbytes or E.payload_sha(path, nbytes) != digest:
            raise StopRun('payload hash/size differs before any device access: ' + name)


def identify(backup_files, prev_images, new_images):
    """Which build is on the phone: compare the head of each saved partition with the image lengths of the previous
    kit (the build that must be installed) and of the new kit. -> {'boot': 'prev'|'new'|'other', ...}"""
    state = {}
    for name in U.UPDATE_WRITABLE:
        f = backup_files[name]
        got = set()
        for tag, imgs in (('prev', prev_images), ('new', new_images)):
            _, nbytes, digest = imgs[name]
            if L.sha256_file(f, nbytes) == digest:
                got.add(tag)
        state[name] = 'prev' if 'prev' in got else ('new' if 'new' in got else 'other')
    return state


def _pre_checks(s, kit_expected):
    E.check_disk_identity(s, kit_expected)
    _, vb = s.read_region('vbmeta-check', *L.PARTITIONS['vbmeta'])
    if vb != L.VBMETA_STOCK:
        raise StopRun('vbmeta differs from the bootloader-tested stock vbmeta')
    _, di = s.read_region('devinfo-check', *L.PARTITIONS['devinfo'])
    if di != L.DEVINFO_UNLOCKED:
        raise StopRun('devinfo differs from the verified unlocked baseline')
    bcb_path, _ = s.read_region('misc-check', *L.PARTITIONS['misc'])
    bcb = bcb_path.read_bytes()[:4096]
    if hashlib.sha256(bcb).hexdigest() not in (L.EMPTY_BCB, L.BOOTLOADER_BCB):
        raise StopRun('unknown boot message in misc (pending recovery command?); not updating over it')
    for n in ('vbmeta-check', 'devinfo-check', 'misc-check'):
        os.remove(s.capture / f'{n}.bin')


def hash_invariants(s, suffix):
    out = {}
    for label, (start, sectors) in U.invariant_regions().items():
        out[label] = s.hash_region('inv-' + label, start, sectors, suffix)
    return out


def run_update(dev, capture, images, prev_images, prev_compat, new_compat, kit_expected, report, save, mode='update',
               userdata_full=False, reflash=False, power_after=None, backup_policy='snapshot'):
    """images/prev_images: {'boot','dtbo','system','vendor'} -> (path, nbytes, sha256) of the new kit / of the build that
    must be on the phone (prev_images paths are only used for their (nbytes, sha256); the files need not exist)."""
    if mode not in ('update', 'backup-only'):
        raise StopRun('unknown mode ' + repr(mode))
    if backup_policy not in ('snapshot', 'hash-only'):
        raise StopRun('unknown backup policy')
    if backup_policy == 'hash-only' and (mode != 'update' or userdata_full):
        raise StopRun('hash-only is for development updates without a userdata backup')
    s = E.Session(dev, capture, report, save)
    report['mode'] = mode
    report['backup_policy'] = backup_policy
    # ---- offline checks (no device access) ----
    load_payloads(images)
    problems = U.compat_problems(prev_compat, new_compat)
    report['compat'] = {'prev': prev_compat.get('system_fingerprint'), 'new': new_compat.get('system_fingerprint'),
                        'problems': problems}
    save()
    if problems:
        raise StopRun('new build cannot boot the existing /data: ' + '; '.join(problems))
    writes = U.update_writes({k: v[1] for k, v in images.items()})
    _checked(writes)
    report['plan'] = [list(w) for w in writes] if mode == 'update' else []
    report['plan_if_update'] = [list(w) for w in writes]
    save()
    # ---- device: identity + state (reads only) ----
    _pre_checks(s, kit_expected)
    inv_before = hash_invariants(s, '-before')
    report['invariants_before'] = inv_before
    save()
    backups, files = {}, {}
    head_hashes = {}
    if backup_policy == 'snapshot':
        for name in U.UPDATE_BACKUP:
            path, digest = s.read_region(name, *L.PARTITIONS[name])
            backups[name], files[name] = digest, path
            print('backup ' + name + ' ' + digest[:16], flush=True)
        for name in ('boot', 'dtbo'):
            out, digest = s.read_region(name, *L.PARTITIONS[name], '-second-read')
            os.remove(out)
            if digest != backups[name]:
                raise StopRun('second read differs (unstable transfer?): ' + name)
        state = identify(files, prev_images, images)
    else:
        state = {}
        for name in U.UPDATE_WRITABLE:
            got, cache = set(), {}
            for tag, kit in (('prev', prev_images), ('new', images)):
                _, nbytes, expected = kit[name]
                if nbytes <= 0 or nbytes % L.SECTOR or nbytes > L.PARTITIONS[name][1] * L.SECTOR:
                    raise StopRun('invalid installed-image length: ' + name)
                if nbytes not in cache:
                    cache[nbytes] = s.hash_region(name+'-head-'+str(nbytes), L.PARTITIONS[name][0], nbytes // L.SECTOR)
                if cache[nbytes] == expected:
                    got.add(tag)
            state[name] = 'prev' if 'prev' in got else ('new' if 'new' in got else 'other')
            head_hashes[name] = cache
            print('installed image checked (no snapshot) ' + name, flush=True)
    report['predecessor'] = state
    save()
    # r6b: a partition identical in both kits identifies as 'prev'; it says nothing about which build is installed
    same = {n for n in U.UPDATE_WRITABLE if images[n][1:] == prev_images[n][1:]}
    if set(v for n, v in state.items() if n not in same) == {'new'} and not reflash:
        raise StopRun('the new build is already installed (use --reflash to write it again)')
    if not reflash and set(state.values()) != {'prev'}:
        raise StopRun('installed build is not the expected previous kit build: %s (stock phones and unknown builds '
                      'need the wipe install)' % state)
    if reflash and not set(state.values()) <= {'prev', 'new'}:
        raise StopRun('--reflash only over the previous or the new kit build: %s' % state)
    if userdata_full:
        path, digest = s.read_region('userdata-full', *U.userdata_full())
        backups['userdata-full'] = digest
        print('backup userdata-full ' + digest[:16], flush=True)
    manifest = {'kind': 'rom-update' if backup_policy == 'snapshot' else 'rom-update-hashes',
                'backup_policy': backup_policy, 'backups': backups, 'image_head_hashes': head_hashes,
                'layout': {n: list(L.PARTITIONS[n]) for n in U.UPDATE_BACKUP},
                'predecessor': state, 'prev_compat': prev_compat, 'new_compat': new_compat,
                'invariants_before': inv_before}
    if userdata_full:
        manifest['layout']['userdata-full'] = list(U.userdata_full())
    (Path(capture) / 'backup-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    report['backup_complete'] = backup_policy == 'snapshot'
    report['predecessor_verified'] = True
    save()
    if mode == 'backup-only':
        inv_after = hash_invariants(s, '-after')
        report['invariants_unchanged'] = inv_after == inv_before
        save()
        if inv_after != inv_before:
            raise StopRun('invariant regions changed during a read-only session: %s' % [k for k in inv_after if inv_after[k] != inv_before[k]])
        report['rehearsal_passed'] = True
        save()
        dev.power(power_after or 'reset')
        report['power'] = power_after or 'reset'
        save()
        return
    # ---- writes: boot, dtbo, vendor, system only; r6b: only those that change ----
    unchanged = [n for n in U.UPDATE_WRITABLE if not reflash and state[n] == 'prev'
                 and images[n][1:] == prev_images[n][1:]]
    writes = [w for w in writes if w[0] not in unchanged]
    report['unchanged_skipped'] = unchanged
    report['plan'] = [list(w) for w in writes]
    save()
    for n in unchanged:
        print('unchanged (not written) ' + n, flush=True)
    for label, start, sectors in writes:
        U.check_update_plan([(label, start, sectors)])
        path, nbytes, digest = images[label]
        s.program(label, start, sectors, E.file_stream(path, nbytes), digest)
        print('written ' + label, flush=True)
    for label, start, sectors in writes:
        s.verify_written(label, start, sectors, images[label][2])
        print('readback ok ' + label, flush=True)
    report['readback_verified'] = True
    save()
    inv_after = hash_invariants(s, '-after')
    report['invariants_after'] = inv_after
    report['invariants_unchanged'] = inv_after == inv_before
    save()
    if inv_after != inv_before:
        raise StopRun('protected regions changed: %s (do not boot; inspect)' % [k for k in inv_after if inv_after[k] != inv_before[k]])
    dev.power(power_after or 'off')
    report['power'] = power_after or 'off'
    save()


def run_rollback(dev, capture, update_capture, kit_expected, report, save, power_after=None):
    """Write back the four partitions saved by an `update` run (keeps data). Allowed only when every partition holds
    either the update's new image (readback-verified) or the saved bytes, or it was attempted but not verified by that
    update (failed transfer)."""
    s = E.Session(dev, capture, report, save)
    report['mode'] = 'rollback'
    src = Path(update_capture)
    manifest = json.loads((src / 'backup-manifest.json').read_text())
    if manifest.get('kind') != 'rom-update':
        raise StopRun('capture has no partition snapshots; reflash the retained previous ROM kit')
    urep = json.loads((src / 'report.json').read_text())
    if manifest.get('kind') != 'rom-update' or urep.get('mode') != 'update':
        raise StopRun('not an update capture')
    if manifest.get('layout', {}).get('boot') is None or any(manifest['layout'][n] != list(L.PARTITIONS[n]) for n in U.UPDATE_BACKUP):
        raise StopRun('capture layout differs from this tool\'s layout; not rolling back')
    for n in U.UPDATE_BACKUP:
        f = src / f'{n}.bin'
        if f.stat().st_size != L.PARTITIONS[n][1] * L.SECTOR or L.sha256_file(f) != manifest['backups'][n]:
            raise StopRun('saved partition differs from its manifest: ' + n)
    report['backup_files_verified'] = True
    save()
    writes = U.rollback_writes()
    _checked(writes)
    report['plan'] = [list(w) for w in writes]
    save()
    _pre_checks(s, kit_expected)
    inv_before = hash_invariants(s, '-before')
    report['invariants_before'] = inv_before
    if inv_before != manifest.get('invariants_before'):
        report['invariants_differ_from_update'] = [k for k in inv_before if inv_before[k] != manifest.get('invariants_before', {}).get(k)]
    save()
    state = {}
    for name in U.UPDATE_BACKUP:
        path, digest = s.read_region(name, *L.PARTITIONS[name], '-now')
        w = urep.get('writes', {}).get(name, {})
        head = L.sha256_file(path, w['sectors'] * L.SECTOR) if w.get('sectors') else None
        os.remove(path)
        if digest == manifest['backups'][name]:
            state[name] = 'saved'
        elif w.get('attempted') and head == w.get('expected_sha256'):
            state[name] = 'update'
        elif w.get('attempted') and not w.get('readback_verified'):
            state[name] = 'failed-write'
        else:
            state[name] = 'other'
    report['state'] = state
    save()
    if 'other' in state.values():
        raise StopRun('partition content is neither the saved nor the update build: %s' % state)
    for label, start, sectors in writes:
        U.check_update_plan([(label, start, sectors)])
        s.program(label, start, sectors, E.file_stream(src / f'{label}.bin', sectors * L.SECTOR), manifest['backups'][label])
        print('restored ' + label, flush=True)
    for label, start, sectors in writes:
        s.verify_written(label, start, sectors, manifest['backups'][label])
        print('readback ok ' + label, flush=True)
    report['readback_verified'] = True
    save()
    inv_after = hash_invariants(s, '-after')
    report['invariants_unchanged'] = inv_after == inv_before
    save()
    if inv_after != inv_before:
        raise StopRun('protected regions changed during the rollback: %s' % [k for k in inv_after if inv_after[k] != inv_before[k]])
    dev.power(power_after or 'reset')
    report['power'] = power_after or 'reset'
    save()
