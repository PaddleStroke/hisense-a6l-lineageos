"""A6L "preserve userdata" update layout (agent update-keepdata, 29 Sep 2026). Pure data + checks: no USB, no adb, no phone.

The update replaces ONLY boot, dtbo, vendor and system of a phone that already runs a kit-installed LineageOS build, and
never touches userdata, metadata, persist, modem EFS (modemst1/modemst2/fsg/fsc), misc, recovery, vbmeta, devinfo or the
GPT. Three independent layers enforce that:
  1. names: UPDATE_WRITABLE is a partition-NAME allowlist, NEVER_WRITE a denylist; both are checked against each other;
  2. geometry: every planned write must start at the start of one UPDATE_WRITABLE partition, stay inside it and overlap no
     other partition of the layout (check_update_plan) - the layout itself is the 14 Sep GPT (RomFlashLayoutV1, checked
     against the live GPT by the engine before anything else);
  3. transport: the EDL worker's UpdateGuard and the adb transport re-run check_update_plan on the exact range they send.
All geometry comes from RomFlashLayoutV1.PARTITIONS at call time (so the mini-eMMC tests can scale it down).

wipe-data mode (user-build-20261012/wipe-mode, 12 Oct 2026; for the test-keys <-> release-keys switch): the same plan
plus EXACTLY two extra ranges of zeroes, the first MiB of metadata and of userdata (the ranges of
RomFlashLayoutV1.install_writes), so fs_mgr formats /metadata and /data ('formattable') on the first boot. Only with
wipe=True, which the callers set from an explicit --wipe-data flag. Everything else stays NEVER_WRITE; userdata/metadata
stay NEVER_WRITE as partitions: only those two exact (start, 2048) ranges, with their -zero labels, are let through.
"""
import RomFlashLayoutV1 as L

UPDATE_WRITABLE = ('boot', 'dtbo', 'vendor', 'system')
NEVER_WRITE = ('userdata', 'metadata', 'persist', 'modemst1', 'modemst2', 'fsg', 'fsc', 'misc', 'recovery', 'vbmeta',
               'devinfo')
assert not set(UPDATE_WRITABLE) & set(NEVER_WRITE)

# Full partitions saved before an update (= what the update overwrites; a rollback writes them back byte for byte).
UPDATE_BACKUP = UPDATE_WRITABLE


def invariant_regions(wipe=False):
    """Regions hashed before the first write and again after the last readback: they must be byte-identical (proof that
    the update left user data, EFS, persist, BCB, recovery, AVB state and the partition table alone). userdata is too
    big to hash over EDL every time: its first 64 MiB (ext4 superblock, group descriptors, journal start) and last MiB.
    wipe=True: the same regions, except that metadata and userdata-head start right AFTER their zeroed first MiB (the
    zeroed MiB itself is checked by its own readback against the hash of zeroes)."""
    ud_start, ud_len = L.PARTITIONS['userdata']
    r = {'gpt-primary': L.GPT_PRIMARY, 'gpt-tail': L.GPT_TAIL}
    for n in ('misc', 'metadata', 'persist', 'modemst1', 'modemst2', 'fsg', 'fsc', 'recovery', 'vbmeta', 'devinfo'):
        r[n] = L.PARTITIONS[n]
    r['userdata-head'] = (ud_start, min(L.USERDATA_HEAD_BACKUP, ud_len))
    r['userdata-tail'] = (ud_start + ud_len - L.USERDATA_TAIL_BACKUP, L.USERDATA_TAIL_BACKUP)
    if wipe:
        md_start, md_len = r.pop('metadata')
        r['metadata-after-zero'] = (md_start + L.METADATA_HEAD_ZERO, md_len - L.METADATA_HEAD_ZERO)
        hd_start, hd_len = r.pop('userdata-head')
        r['userdata-head-after-zero'] = (hd_start + L.USERDATA_HEAD_ZERO, hd_len - L.USERDATA_HEAD_ZERO)
        if min(r['metadata-after-zero'][1], r['userdata-head-after-zero'][1]) <= 0:
            raise ValueError('layout too small for the wipe invariants')
    return r


# ---- wipe-data mode -------------------------------------------------------------------------------------------------
WIPE_ZERO = {'metadata-zero': 'metadata', 'userdata-zero': 'userdata'}   # label -> partition (first MiB only)


def wipe_zero_ranges():
    """label -> (start, sectors): the first MiB of metadata and of userdata, the RomFlashLayoutV1.install_writes ranges."""
    return {'metadata-zero': (L.PARTITIONS['metadata'][0], L.METADATA_HEAD_ZERO),
            'userdata-zero': (L.PARTITIONS['userdata'][0], L.USERDATA_HEAD_ZERO)}


def userdata_full():
    return L.PARTITIONS['userdata']


def update_writes(sizes):
    """sizes: {'boot','dtbo','vendor','system'} payload bytes -> ordered [(label, start, sectors)]. No zero regions:
    nothing is formatted, userdata/metadata keep their bytes."""
    out = []
    for name in ('boot', 'dtbo', 'vendor', 'system'):
        start, length = L.PARTITIONS[name]
        n = L.sectors_for(sizes[name])
        if not 0 < n <= length:
            raise ValueError(f'{name} payload does not fit its partition')
        out.append((name, start, n))
    check_update_plan(out)
    return out


def wipe_writes(sizes):
    """update_writes(sizes) + the two zero ranges (metadata first, then userdata), checked with wipe=True. The engine
    writes and reads back the images before it writes the zeroes."""
    out = update_writes(sizes) + [(label, start, sectors) for label, (start, sectors) in wipe_zero_ranges().items()]
    check_update_plan(out, wipe=True)
    return out


def rollback_writes():
    """Rollback of an update (keeps data): the four full partitions from the update backup."""
    out = [(n, *L.PARTITIONS[n]) for n in UPDATE_BACKUP]
    check_update_plan(out)
    return out


def partition_at(start, sectors):
    """Names of every layout partition that the range [start, start+sectors) overlaps."""
    end = start + sectors
    return [n for n, (ps, pn) in L.PARTITIONS.items() if start < ps + pn and ps < end]


def check_update_plan(writes, wipe=False):
    """Raises ValueError unless every write: is non-empty, starts exactly at the start of ONE UPDATE_WRITABLE partition,
    stays inside it, overlaps no other partition (above all none of NEVER_WRITE) and does not touch the GPT regions; and
    no partition is written twice. Labels must name the partition they write (they are informative elsewhere; here they
    must agree with the geometry).
    wipe=True additionally lets through exactly the wipe_zero_ranges(): label 'metadata-zero'/'userdata-zero' AND exactly
    that (start, sectors). Any other range in metadata/userdata (longer, shifted, other label) is still refused."""
    seen = set()
    gpt = [L.GPT_PRIMARY, L.GPT_TAIL]
    zero = wipe_zero_ranges() if wipe else {}
    for label, start, sectors in writes:
        start, sectors = int(start), int(sectors)
        if sectors <= 0 or start < 0:
            raise ValueError(f'empty/negative planned write: {label}')
        hit = partition_at(start, sectors)
        if len(hit) != 1:
            raise ValueError(f'write {label} {start}+{sectors} overlaps {hit or "no partition"}')
        name = hit[0]
        if zero and (label in zero or name in WIPE_ZERO.values()):
            if label not in zero or zero[label] != (start, sectors) or WIPE_ZERO[label] != name:
                raise ValueError(f'wipe write {label} {start}+{sectors} is not exactly the first MiB of metadata/userdata')
            for gs, gn in gpt:
                if start < gs + gn and gs < start + sectors:
                    raise ValueError(f'write {label} overlaps the GPT')
            if label in seen:
                raise ValueError(f'{label} written twice')
            seen.add(label)
            continue
        if name in NEVER_WRITE or name not in UPDATE_WRITABLE:
            raise ValueError(f'write {label} would touch {name}: not in the update allowlist {UPDATE_WRITABLE}')
        ps, pn = L.PARTITIONS[name]
        if start != ps or start + sectors > ps + pn:
            raise ValueError(f'write {label} does not start at the start of {name} or leaves it')
        if label != name:
            raise ValueError(f'write label {label} does not match the partition it writes ({name})')
        for gs, gn in gpt:
            if start < gs + gn and gs < start + sectors:
                raise ValueError(f'write {label} overlaps the GPT')
        if name in seen:
            raise ValueError(f'{name} written twice')
        seen.add(name)
    return True


# ---- data compatibility (pure; inputs are the rom-update-compat.json of the installed and of the new build) ----------
COMPAT_KEYS = ('system_fingerprint', 'sdk', 'security_patch', 'vendor_security_patch', 'platform_cert_sha256', 'data_fs',
               'data_encryption', 'build_type')


def compat_problems(prev, new):
    """Reasons why /data of `prev` must not be booted by `new` (empty list = compatible). Rules:
    - same platform signing certificate (test-keys -> release-keys changes every system app signature: wipe needed);
    - same /data filesystem and encryption mode (plain ext4 -> FBE, or FBE policy change, needs a wipe);
    - SDK and security patch levels must not go down (keystore/keymint keys are bound to the patch level; a downgrade
      makes them unusable, and PackageManager does not support SDK downgrades)."""
    out = []
    for k in COMPAT_KEYS:
        if k not in prev or k not in new or prev[k] in (None, '') or new[k] in (None, ''):
            out.append(f'compat field missing: {k}')
    if out:
        return out
    if prev['platform_cert_sha256'] != new['platform_cert_sha256']:
        out.append('platform signing certificate differs (test-keys/release-keys switch needs the wipe install)')
    if prev['data_fs'] != new['data_fs'] or prev['data_encryption'] != new['data_encryption']:
        out.append('/data filesystem or encryption differs: %s/%s -> %s/%s' % (prev['data_fs'], prev['data_encryption'],
                                                                              new['data_fs'], new['data_encryption']))
    if int(new['sdk']) < int(prev['sdk']):
        out.append('SDK level goes down: %s -> %s' % (prev['sdk'], new['sdk']))
    if new['security_patch'] < prev['security_patch']:
        out.append('security patch goes down: %s -> %s' % (prev['security_patch'], new['security_patch']))
    if new['vendor_security_patch'] < prev['vendor_security_patch']:
        out.append('vendor security patch goes down: %s -> %s' % (prev['vendor_security_patch'], new['vendor_security_patch']))
    return out


# ---- wipe-data compatibility ------------------------------------------------------------------------------------------
# With --wipe-data /data and /metadata are formatted at the first boot, so the ONLY problems of compat_problems() that are
# accepted are the two a test-keys <-> release-keys switch is expected to produce. Everything else still refuses: a
# missing field, an SDK or security-patch downgrade, any problem text not listed here.
WIPE_ACCEPTED_PROBLEMS = ('platform signing certificate differs ', '/data filesystem or encryption differs: ')
WIPE_BUILD_TYPES = ('user', 'userdebug')


def wipe_compat(prev, new):
    """-> (refusals, waived). waived = the compat_problems() accepted because /data is wiped, plus the informational
    build type / tags change (user <-> userdebug, release-keys <-> test-keys). Refusals besides the remaining
    compat_problems(): the new build must be user or userdebug, plain ext4 without FBE (the wipe only zeroes heads;
    FBE was never trialled) and its fstab must mark /data and /metadata 'formattable' (else nothing would format the
    zeroed partitions and the first boot would fail to mount them)."""
    refusals, waived = [], []
    for p in compat_problems(prev, new):
        (waived if p.startswith(WIPE_ACCEPTED_PROBLEMS) else refusals).append(p)
    for k in ('build_type', 'build_tags'):
        if prev.get(k) != new.get(k):
            waived.append('%s differs: %s -> %s' % (k, prev.get(k), new.get(k)))
    if new.get('build_type') not in WIPE_BUILD_TYPES:
        refusals.append('wipe-data: new build type %r is not one of %s' % (new.get('build_type'), WIPE_BUILD_TYPES))
    if (new.get('data_fs'), new.get('data_encryption')) != ('ext4', 'none'):
        refusals.append('wipe-data: new /data must be plain ext4 without encryption (got %s/%s)'
                        % (new.get('data_fs'), new.get('data_encryption')))
    if new.get('data_formattable') is not True or new.get('metadata_formattable') is not True:
        refusals.append('wipe-data: the new fstab does not mark both /data and /metadata formattable '
                        '(data_formattable=%r metadata_formattable=%r)' % (new.get('data_formattable'), new.get('metadata_formattable')))
    return refusals, waived
