#!/usr/bin/env python3
"""Inspect the captured A6L's QcomBds shutdown prompt entirely offline.

Requires the existing XBL UEFI extraction. Does not access a phone or network.
The small PE reader only accepts the hash-pinned AArch64 image below.
"""
import hashlib
import json
from pathlib import Path
import re
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'firmware/extracted/stock-xbl-20260915'
HASH = 'd5cb83a194d55f93a8abacc42d058e4a1f990d5472bbe4786813e29a60d8e04a'
PROMPT = 'Press any key to shutdown'


def main():
    backup = ROOT / 'firmware/raw-backup-20260914'
    manifest = json.loads((backup / 'firmware-verification.json').read_text())
    part = next(p for p in manifest['partitions'] if p['name'] == 'xbl')
    with (backup / 'emmc-firmware-prefix.bin').open('rb') as stream:
        stream.seek(part['offset'])
        xbl = stream.read(part['bytes'])
    assert hashlib.sha256(xbl).hexdigest() == part['sha256']
    assert xbl == (OUT / 'xbl.bin').read_bytes()
    images = [p for p in (OUT / 'uefi').rglob('*.pe')
              if hashlib.sha256(p.read_bytes()).hexdigest() == HASH]
    assert len(images) == 1
    source = images[0]
    assert any(p.read_bytes().decode('utf-16-le').rstrip('\0') == 'QcomBds'
               for p in source.parent.glob('*.ui'))
    data = source.read_bytes()
    (OUT / 'QcomBds.efi').write_bytes(data)
    assert data[:2] == b'MZ'
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[pe:pe + 4] == b'PE\0\0'
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    assert machine == 0xaa64
    opt_len = struct.unpack_from('<H', data, pe + 20)[0]
    opt = pe + 24
    assert struct.unpack_from('<H', data, opt)[0] == 0x20b
    base = struct.unpack_from('<Q', data, opt + 24)[0]
    size, headers = struct.unpack_from('<II', data, opt + 56)
    mapped = bytearray(size)
    mapped[:headers] = data[:headers]
    sections = []
    for i in range(count):
        off = opt + opt_len + i * 40
        name, virtual_size, rva, raw_size, raw_off = struct.unpack_from('<8sIIII', data, off)
        flags = struct.unpack_from('<I', data, off + 36)[0]
        assert rva + raw_size <= size and raw_off + raw_size <= len(data)
        mapped[rva:rva + raw_size] = data[raw_off:raw_off + raw_size]
        sections.append(dict(name=name.rstrip(b'\0').decode(), rva=rva,
                             virtual_size=virtual_size, raw_offset=raw_off,
                             raw_size=raw_size, flags=flags))
    strings = []
    for pattern, encoding in [(rb'[ -~]{5,}', 'ascii'),
                              (rb'(?:[ -~]\x00){5,}', 'utf-16-le')]:
        for match in re.finditer(pattern, mapped):
            strings.append(dict(rva=match.start(), encoding=encoding,
                                text=match.group().decode(encoding)))
    targets = [s for s in strings if s['text'] == PROMPT]
    assert len(targets) == 1
    target = targets[0]['rva']
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    refs = []
    for section in sections:
        if not section['flags'] & 0x20000000:
            continue
        for offset in range(section['rva'], section['rva'] + section['virtual_size'], 4):
            ins = next(md.disasm(bytes(mapped[offset:offset + 4]), base + offset), None)
            if not ins or ins.mnemonic not in ('adr', 'adrp'):
                continue
            reg, address = ins.operands[0].reg, ins.operands[1].imm
            candidates = [(ins.address, address)] if ins.mnemonic == 'adr' else []
            if ins.mnemonic == 'adrp':
                for nxt in md.disasm(bytes(mapped[offset + 4:offset + 36]), base + offset + 4):
                    ops = nxt.operands
                    if (nxt.mnemonic == 'add' and len(ops) == 3
                            and ops[1].type == ARM64_OP_REG and ops[1].reg == reg
                            and ops[2].type == ARM64_OP_IMM):
                        candidates.append((nxt.address, address + ops[2].imm))
                    if reg in nxt.regs_access()[1] or nxt.mnemonic in ('ret', 'b', 'br'):
                        break
            for reference, address in candidates:
                if address == base + target:
                    refs.append(dict(adr_rva=offset, reference_rva=reference - base,
                                     target_rva=target))
    assert refs, 'Prompt reference not found'
    disassembly = []
    for ref in refs:
        start = ref['adr_rva'] - 96
        end = ref['adr_rva'] + 288
        for ins in md.disasm(bytes(mapped[start:end]), base + start):
            disassembly.append(f'{ins.address - base:08x} {ins.mnemonic:8} {ins.op_str}')
    (OUT / 'shutdown-disassembly.txt').write_text('\n'.join(disassembly) + '\n')
    (OUT / 'qcombds-strings.json').write_text(json.dumps(strings, indent=2) + '\n')
    report = dict(scope='Offline inspection of captured XBL; no phone access',
                  xbl_partition=part, source=str(source.relative_to(ROOT)),
                  qcombds_sha256=HASH, image_base=base, sections=sections,
                  prompt=targets[0], prompt_references=refs,
                  limitation='Prompt location establishes the displaying module, not the original USB failure cause.')
    (OUT / 'shutdown-report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))
    print('\n'.join(disassembly))


if __name__ == '__main__':
    main()
