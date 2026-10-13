#!/usr/bin/env python3
"""Read the extracted LinuxLoader PE and record its sections and diagnostic strings."""
import hashlib
import json
from pathlib import Path
import re
import struct
import pefile

root = Path(__file__).resolve().parent.parent / 'firmware/extracted/stock-abl-20260914'
images = list((root / 'uefi').rglob('section1.pe'))
assert len(images) == 1, 'Expected exactly one LinuxLoader PE'
data = images[0].read_bytes()
assert hashlib.sha256(data).hexdigest() == '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523'
pe = pefile.PE(data=data)
(root / 'LinuxLoader.efi').write_bytes(data)
report = {'sha256': hashlib.sha256(data).hexdigest(), 'bytes': len(data),
          'machine': hex(pe.FILE_HEADER.Machine), 'image_base': pe.OPTIONAL_HEADER.ImageBase,
          'entry_point_rva': pe.OPTIONAL_HEADER.AddressOfEntryPoint,
          'sections': [{ 'name': section.Name.decode().rstrip('\0'),
                         'rva': section.VirtualAddress, 'virtual_size': section.Misc_VirtualSize,
                         'file_offset': section.PointerToRawData, 'file_size': section.SizeOfRawData,
                         'flags': hex(section.Characteristics)} for section in pe.sections]}
# The registration loop at 0x2f388..0x2f3ac walks 17 pointer pairs at 0x4d5e8.
# This interpretation is specific to the hash-guarded payload and was checked
# against AArch64 disassembly; strings alone do not establish command support.
commands = []
for offset in range(0x4d5e8, 0x4d5e8 + 17 * 16, 16):
    name, handler = struct.unpack_from('<QQ', data, offset)
    assert 0x3e000 <= name < 0x51000 and 0x1000 <= handler < 0x3e000
    end = data.index(b'\0', name)
    commands.append({'table_offset': hex(offset), 'prefix': data[name:end].decode('ascii'),
                     'handler_rva': hex(handler)})
report['registered_fastboot_commands'] = commands
report['boot_memory_constants'] = {hex(offset): hex(struct.unpack_from('<I', data, offset)[0])
                                  for offset in [0x3e30c, 0x3e310, 0x3e314]}
for kind, pattern, codec in [('ascii', rb'[ -~]{5,}', 'ascii'),
                             ('utf16', rb'(?:[ -~]\x00){5,}', 'utf-16-le')]:
    strings = []
    for match in re.finditer(pattern, data):
        try:
            address = pe.OPTIONAL_HEADER.ImageBase + pe.get_rva_from_offset(match.start())
        except pefile.PEFormatError:
            continue
        strings.append(f'{match.start():08x} va={address:08x} {match.group().decode(codec)}')
    (root / f'loader-strings-{kind}.txt').write_text('\n'.join(strings) + '\n')
(root / 'loader-report.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
