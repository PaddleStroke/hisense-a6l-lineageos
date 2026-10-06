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
"""
import RomFlashLayoutV1 as L

UPDATE_WRITABLE = ('boot', 'dtbo', 'vendor', 'system')
NEVER_WRITE = ('userdata', 'metadata', 'persist', 'modemst1', 'modemst2', 'fsg', 'fsc', 'misc', 'recovery', 'vbmeta',
               'devinfo')
assert not set(UPDATE_WRITABLE) & set(NEVER_WRITE)

# Full partitions saved before an update (= what the update overwrites; a rollback writes them back byte for byte).
UPDATE_BACKUP = UPDATE_WRITABLE


def invariant_regions():
    """Regions hashed before the first write and again after the last readback: they must be byte-identical (proof that
    the update left user data, EFS, persist, BCB, recovery, AVB state and the partition table alone). userdata is too
    big to hash over EDL every time: its first 64 MiB (ext4 superblock, group descriptors, journal start) and last MiB."""
    ud_start, ud_len = L.PARTITIONS['userdata']
    r = {'gpt-primary': L.GPT_PRIMARY, 'gpt-tail': L.GPT_TAIL}
    for n in ('misc', 'metadata', 'persist', 'modemst1', 'modemst2', 'fsg', 'fsc', 'recovery', 'vbmeta', 'devinfo'):
        r[n] = L.PARTITIONS[n]
    r['userdata-head'] = (ud_start, min(L.USERDATA_HEAD_BACKUP, ud_len))
    r['userdata-tail'] = (ud_start + ud_len - L.USERDATA_TAIL_BACKUP, L.USERDATA_TAIL_BACKUP)
    return r


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


def rollback_writes():
    """Rollback of an update (keeps data): the four full partitions from the update backup."""
    out = [(n, *L.PARTITIONS[n]) for n in UPDATE_BACKUP]
    check_update_plan(out)
    return out


def partition_at(start, sectors):
    """Names of every layout partition that the range [start, start+sectors) overlaps."""
    end = start + sectors
    return [n for n, (ps, pn) in L.PARTITIONS.items() if start < ps + pn and ps < end]


def check_update_plan(writes):
    """Raises ValueError unless every write: is non-empty, starts exactly at the start of ONE UPDATE_WRITABLE partition,
    stays inside it, overlaps no other partition (above all none of NEVER_WRITE) and does not touch the GPT regions; and
    no partition is written twice. Labels must name the partition they write (they are informative elsewhere; here they
    must agree with the geometry)."""
    seen = set()
    gpt = [L.GPT_PRIMARY, L.GPT_TAIL]
    for label, start, sectors in writes:
        start, sectors = int(start), int(sectors)
        if sectors <= 0 or start < 0:
            raise ValueError(f'empty/negative planned write: {label}')
        hit = partition_at(start, sectors)
        if len(hit) != 1:
            raise ValueError(f'write {label} {start}+{sectors} overlaps {hit or "no partition"}')
        name = hit[0]
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
