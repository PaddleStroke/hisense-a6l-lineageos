"""A6L rom-v1 flash layout and plan (agent flash, 24 Sep 2026). Pure data + checks: no USB, no phone access.

Geometry = the verified 14 Sep GPT (firmware/raw-backup-20260914/partition-map.json, 512-byte sectors, one LUN).
Non-A/B. The installer WRITES only: boot, dtbo, system, vendor, and the first MiB of metadata and of userdata (zeroes, so
fs_mgr formats /metadata and /data on the first LineageOS boot). Everything it writes, plus the partitions the running ROM may write
(misc BCB, modem EFS, persist), is backed up first, in the same EDL session.
"""
import hashlib
import struct
import zlib

SECTOR = 512
DISK_BYTES = 125074145280
# name: (start_sector, num_sectors)
PARTITIONS = {
    'fsg': (149504, 4096), 'dtbo': (1572864, 16384), 'boot': (671744, 131072), 'recovery': (917504, 131072),
    'fsc': (1048576, 2), 'modemst1': (1048600, 4096), 'modemst2': (1052696, 4096),
    'system': (1589248, 12582912), 'vendor': (14172160, 2252800), 'persist': (17039360, 65536),
    'misc': (17104896, 2048), 'devinfo': (495616, 8), 'vbmeta': (569944, 128),
    'userdata': (20322304, 223963092),
    'metadata': (1472536, 20480),   # 10 MiB; added by agent gnss 24 Sep: Android 17 needs /metadata (aconfig storage)
}
GPT_PRIMARY = (0, 2048)            # 1 MiB (protective MBR + header + entries), compared with the kit's expected-primary.bin
GPT_TAIL = (244285396, 44)         # 22 KiB secondary entries + header, compared with expected-tail.bin
USERDATA_HEAD_ZERO = 2048          # 1 MiB of zeroes written at the start of userdata by the installer
USERDATA_HEAD_BACKUP = 131072      # 64 MiB saved before that
USERDATA_TAIL_BACKUP = 2048        # last 1 MiB saved (stock FDE crypto footer = last 16 KiB)
USERDATA_FOOTER = 32               # last 16 KiB (stock forceencrypt=footer)
METADATA_HEAD_ZERO = 2048          # 1 MiB of zeroes at the start of metadata -> fs_mgr formats /metadata (ext4) on the first boot

# Known contents (sha256 of the full partition) from the 14 Sep verified backup / later installs.
STOCK = {
    'system': None, 'vendor': None, 'dtbo': None,   # filled from firmware-verification.json by Prepare-RomV1.py
}
VBMETA_STOCK = 'e48632a03179e68b3c059905f613d321b3b37aa789ba72a02b322dffde4f7350'
DEVINFO_UNLOCKED = '7d6a4855f19d498a092ff0ffb44a69e114cd915d4d49acb2640035cf70e854ed'
RECOVERY_KNOWN = {
    '24ede49b0f34b10f216617cb007c757e495fa2df1b6cd1ddd0afd63f9819da62': 'v74-diagnostic',
    '417164b78c3bc7c43eba3039caab240194b5a69000b78737c50c3503f398bcf9': 'v71-diagnostic',
    '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621': 'stock',
}
EMPTY_BCB = 'ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7'   # 4 KiB of zeroes
BOOTLOADER_BCB = '8ac9baa0ce2f52dda6debef8f8ffb4fcd8e85d44bf05882dbcb552dd06b46881'

# Regions backed up by install (name -> (start_sector, sectors)). Order = read order.
def backup_regions():
    ud_start, ud_len = PARTITIONS['userdata']
    r = {n: PARTITIONS[n] for n in ('boot', 'dtbo', 'misc', 'metadata', 'modemst1', 'modemst2', 'fsg', 'fsc',
                                     'persist', 'recovery', 'vbmeta', 'devinfo', 'vendor', 'system')}
    r['userdata-head'] = (ud_start, USERDATA_HEAD_BACKUP)
    r['userdata-tail'] = (ud_start + ud_len - USERDATA_TAIL_BACKUP, USERDATA_TAIL_BACKUP)
    return r

# Regions the ROM itself may change while it runs (restore compares them and writes back only if different).
ROM_MAY_WRITE = ('misc', 'metadata', 'modemst1', 'modemst2', 'fsg', 'fsc', 'persist')


def sectors_for(nbytes):
    if nbytes % SECTOR:
        raise ValueError('payload is not sector aligned')
    return nbytes // SECTOR


def install_writes(sizes):
    """sizes: {'boot': n, 'dtbo': n, 'system': n, 'vendor': n} payload byte counts -> ordered list of (label, start, sectors).
    Every write starts at a partition start and stays inside it."""
    out = []
    for name in ('boot', 'dtbo', 'vendor', 'system'):
        start, length = PARTITIONS[name]
        n = sectors_for(sizes[name])
        if not 0 < n <= length:
            raise ValueError(f'{name} payload does not fit its partition')
        out.append((name, start, n))
    out.append(('metadata-zero', PARTITIONS['metadata'][0], METADATA_HEAD_ZERO))
    out.append(('userdata-zero', PARTITIONS['userdata'][0], USERDATA_HEAD_ZERO))
    return out


def restore_writes(have_rom_changes):
    """Restore: full partitions from the install backup; userdata head+footer zeroed (see docs). have_rom_changes: set of
    ROM_MAY_WRITE names whose live bytes differ from the backup."""
    out = [(n, *PARTITIONS[n]) for n in ('boot', 'dtbo', 'vendor', 'system')]
    out += [(n, *PARTITIONS[n]) for n in ROM_MAY_WRITE if n in have_rom_changes]
    ud_start, ud_len = PARTITIONS['userdata']
    out.append(('userdata-zero', ud_start, USERDATA_HEAD_ZERO))
    out.append(('userdata-footer-zero', ud_start + ud_len - USERDATA_FOOTER, USERDATA_FOOTER))
    return out


def check_layout_against_gpt(primary):
    """Parse the primary GPT (1 MiB read at LBA 0) and require every PARTITIONS entry to match by name and geometry."""
    if len(primary) != GPT_PRIMARY[1] * SECTOR:
        raise ValueError('primary GPT region has the wrong length')
    hdr = primary[512:1024]
    if hdr[:8] != b'EFI PART':
        raise ValueError('no GPT header at LBA 1')
    hsize = struct.unpack_from('<I', hdr, 12)[0]
    hcrc = struct.unpack_from('<I', hdr, 16)[0]
    h = bytearray(hdr[:hsize]); h[16:20] = b'\0\0\0\0'
    if zlib.crc32(bytes(h)) & 0xffffffff != hcrc:
        raise ValueError('GPT header CRC mismatch')
    entries_lba, count, esize, ecrc = struct.unpack_from('<QIII', hdr, 72)
    table = primary[entries_lba * 512: entries_lba * 512 + count * esize]
    if zlib.crc32(table) & 0xffffffff != ecrc:
        raise ValueError('GPT entries CRC mismatch')
    found = {}
    for i in range(count):
        e = table[i * esize:(i + 1) * esize]
        if e[:16] == bytes(16):
            continue
        first, last = struct.unpack_from('<QQ', e, 32)
        name = e[56:128].decode('utf-16-le').rstrip('\0')
        found[name] = (first, last - first + 1)
    for name, geometry in PARTITIONS.items():
        if found.get(name) != geometry:
            raise ValueError(f'GPT geometry differs for {name}: {found.get(name)} != {geometry}')
    return found


def program_xml(start, sectors):
    return ('<?xml version="1.0" ?><data><program SECTOR_SIZE_IN_BYTES="512" num_partition_sectors="%d" '
            'physical_partition_number="0" start_sector="%d" /></data>' % (sectors, start))


def sha256_file(path, limit=None):
    h = hashlib.sha256()
    left = limit
    with open(path, 'rb') as f:
        while True:
            n = 1 << 22 if left is None else min(1 << 22, left)
            if n == 0:
                break
            b = f.read(n)
            if not b:
                break
            h.update(b)
            if left is not None:
                left -= len(b)
    return h.hexdigest()
