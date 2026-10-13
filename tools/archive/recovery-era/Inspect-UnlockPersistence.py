#!/usr/bin/env python3
"""Record hash-pinned, offline evidence for the A6L vendor unlock boundary."""
import hashlib
import json
from pathlib import Path
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

root = Path(__file__).resolve().parent.parent
abl_path = root / 'firmware/extracted/stock-abl-20260914/LinuxLoader.efi'
vb_path = root / 'firmware/extracted/stock-xbl-20260915/VerifiedBootDxe.efi'
abl, vb = abl_path.read_bytes(), vb_path.read_bytes()
assert hashlib.sha256(abl).hexdigest() == '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523'
assert hashlib.sha256(vb).hexdigest() == '53b0b13fdd3bd3ae563030f783277319f74bbf232b62e01262515fe21bd1e1fd'
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)


def instructions(data, start, end):
    return [{'rva': hex(i.address), 'mnemonic': i.mnemonic, 'operands': i.op_str}
            for i in md.disasm(data[start:end], start)]


gate = instructions(abl, 0x146a8, 0x146b0)
assert [(i['mnemonic'], i['operands']) for i in gate] == [('mov', 'w0, #1'), ('ret', '')]
assert instructions(abl, 0x33e4c, 0x33e50)[0]['operands'] == '#0x146a8'
assert abl[0x4cde5:0x4ce20].split(b'\0', 1)[0] == b'Unlock is not allowed!'
assert abl[0x500b0:0x500c0] == vb[0xb0c0:0xb0d0]
assert struct.unpack_from('<Q', vb, 0xb190)[0] == 0x155c

backup = root / 'firmware/raw-backup-20260914'
manifest = json.loads((backup / 'firmware-verification.json').read_text())
part = next(p for p in manifest['partitions'] if p['name'] == 'devinfo')
with (backup / 'emmc-firmware-prefix.bin').open('rb') as stream:
    stream.seek(part['offset'])
    devinfo = stream.read(part['bytes'])
assert hashlib.sha256(devinfo).hexdigest() == part['sha256']
assert devinfo[:13] == b'ANDROID-BOOT!'

report = {
    'scope': 'Offline analysis only; no unlock or persistent operation executed',
    'abl_sha256': hashlib.sha256(abl).hexdigest(),
    'verifiedboot_dxe_sha256': hashlib.sha256(vb).hexdigest(),
    'standard_unlock_and_lock_gate': gate,
    'standard_unlock_and_lock_handler': instructions(abl, 0x33e4c, 0x33e70),
    'vendor_runtime_flags_only': instructions(abl, 0x23ee8, 0x23f08),
    'custom_key_persistence_branch': instructions(abl, 0x316cc, 0x316e0),
    'erase_user_key': instructions(abl, 0x245d0, 0x246a0),
    'device_info_protocol': {'guid_bytes': abl[0x500b0:0x500c0].hex(),
                             'provider_table_rva': '0xb188', 'read_write_function_rva': '0x155c'},
    'storage_choice': {
        'initial_security_query_rva': '0x15b4',
        'plain_devinfo_write_rva': '0x18b8',
        'secure_app_read_command': '0x202', 'secure_app_write_command': '0x203',
        'secure_write_rva': '0x17b8', 'protected_transfer_bytes': 4096,
        'fallback_devinfo_write_rva': '0x1960',
        'limitation': 'Protected versus ordinary storage is chosen using secure-monitor state; the branch taken on this spare has not been directly observed.'},
    'captured_gpt_devinfo': {'sha256': part['sha256'], 'bytes': part['bytes'],
                            'unlocked_byte': devinfo[0xd], 'critical_unlocked_byte': devinfo[0xe],
                            'user_key_length': struct.unpack_from('<I', devinfo, 0x94)[0],
                            'limitation': 'These GPT bytes do not establish the live protected-storage contents.'},
    'conclusions': [
        'The captured standard flashing unlock and lock paths reject before the OEM permission check.',
        'Hisense unlock sets both runtime unlock flags; the function does not itself persist them.',
        'erase avb_custom_key clears the runtime custom key and persists the complete 0x998-byte device-info structure.',
        'The erase special case returns OKAY without itself scheduling a reboot or calling the standard unlock wipe routine.',
        'The community reports a subsequent wipe prompt; exact first-boot behavior on this older captured version remains untested.',
        'The firmware backup does not cover RPMB; a full reversal of the security change and normal relocking cannot be promised.'],
}
out = root / 'firmware/extracted/stock-xbl-20260915/unlock-persistence-report.json'
out.write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps({'report': str(out), 'conclusions': report['conclusions']}, indent=2))
