#!/usr/bin/env python3
"""Locate the stock recovery selector and bootloader entry logic offline."""
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct

from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'firmware/extracted/recovery-exit-20260915'


def pinned(path, digest):
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != digest:
        raise ValueError('Hash differs: ' + str(path))
    return data


def map_pe(data):
    pe = struct.unpack_from('<I', data, 0x3c)[0]
    assert data[:2] == b'MZ' and data[pe:pe + 4] == b'PE\0\0'
    machine, count = struct.unpack_from('<HH', data, pe + 4)
    assert machine == 0xaa64
    opt = pe + 24
    opt_len = struct.unpack_from('<H', data, pe + 20)[0]
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
                             virtual_size=virtual_size, raw_size=raw_size, flags=flags))
    return base, mapped, sections


def main():
    OUT.mkdir(exist_ok=True)
    archive_hash = 'c48cf8b1c6fb998e8708e619b8cd51be0dd43260fe60cc94d351600954127291'
    compressed = pinned(ROOT / 'firmware/extracted/boot-roundtrip-20260914-v4/recovery/parts/ramdisk', archive_hash)
    archive = gzip.decompress(compressed)
    offset = 0
    graphic = None
    while True:
        assert archive[offset:offset + 6] in (b'070701', b'070702')
        fields = [int(archive[offset + 6 + i * 8:offset + 14 + i * 8], 16) for i in range(13)]
        size, name_size = fields[6], fields[11]
        name_bytes = archive[offset + 110:offset + 110 + name_size]
        assert name_bytes.endswith(b'\0')
        name = name_bytes[:-1].decode()
        start = (offset + 110 + name_size + 3) & ~3
        assert start + size <= len(archive)
        if name == 'TRAILER!!!':
            break
        if name == 'res/images/recovery_init_vision.png':
            assert graphic is None
            graphic = archive[start:start + size]
        offset = (start + size + 3) & ~3
    assert graphic is not None
    graphic_hash = hashlib.sha256(graphic).hexdigest()
    assert graphic_hash == 'e96826dcc612fefa73676fb39264b216346f3513bc0b6d725ebffa824872ed5b'
    (OUT / 'stock-recovery-selector.png').write_bytes(graphic)

    abl_hash = '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523'
    abl = pinned(ROOT / 'firmware/extracted/stock-abl-20260914/LinuxLoader.efi', abl_hash)
    md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    md.detail = True
    blocks = {'boot_entry': (0x16d0, 0x1a70), 'button_boot_flow': (0x28d00, 0x29134),
              'read_scan_code': (0x256f0, 0x25904), 'read_power_reason': (0x2a028, 0x2a244),
              'fastboot_continue': (0x32204, 0x322dc), 'recovery_request': (0x1bf78, 0x1c200)}
    for name, (start, end) in blocks.items():
        lines = [f'{i.address:08x} {i.mnemonic:8} {i.op_str}' for i in md.disasm(abl[start:end], start)]
        (OUT / (name + '.txt')).write_text('\n'.join(lines) + '\n')

    button_hash = '64ba57b1181136d79ea623207689c9d6055a7de6541efac1245c1392e33209f0'
    button_sources = [p for p in (ROOT / 'firmware/extracted/stock-xbl-20260915/uefi').rglob('*.pe')
                      if hashlib.sha256(p.read_bytes()).hexdigest() == button_hash]
    assert len(button_sources) == 1
    button = button_sources[0].read_bytes()
    assert any(p.read_bytes().decode('utf-16le').rstrip('\0') == 'ButtonsDxe'
               for p in button_sources[0].parent.glob('*.ui'))
    base, mapped, sections = map_pe(button)
    strings = {m.start(): m.group().decode() for m in re.finditer(rb'[ -~]{6,}', mapped)}
    refs, disassembly = [], []
    for section in sections:
        if not section['flags'] & 0x20000000:
            continue
        for off in range(section['rva'], section['rva'] + section['virtual_size'], 4):
            ins = next(md.disasm(bytes(mapped[off:off + 4]), base + off), None)
            if not ins:
                continue
            disassembly.append(f'{ins.address - base:08x} {ins.mnemonic:8} {ins.op_str}')
            if ins.mnemonic != 'adrp':
                continue
            reg, address = ins.operands[0].reg, ins.operands[1].imm
            for nxt in md.disasm(bytes(mapped[off + 4:off + 36]), base + off + 4):
                ops = nxt.operands
                if (nxt.mnemonic == 'add' and len(ops) == 3
                        and ops[1].type == ARM64_OP_REG and ops[1].reg == reg
                        and ops[2].type == ARM64_OP_IMM):
                    target = address + ops[2].imm - base
                    if target in strings:
                        refs.append(dict(instruction=hex(nxt.address - base),
                                         target=hex(target), text=strings[target]))
                if reg in nxt.regs_access()[1] or nxt.mnemonic in ('ret', 'b', 'br'):
                    break
    (OUT / 'buttons-disassembly.txt').write_text('\n'.join(disassembly) + '\n')
    (OUT / 'buttons-string-refs.json').write_text(json.dumps(refs, indent=2) + '\n')
    report = {
        'scope': 'Offline exact-firmware evidence; no phone operation',
        'recovery_ramdisk_sha256': archive_hash,
        'selector_member': 'res/images/recovery_init_vision.png',
        'selector_sha256': graphic_hash,
        'selector_labels_visually_verified': ['进入Recovery模式', '正常启动系统'],
        'abl_sha256': abl_hash, 'buttons_dxe_sha256': button_hash,
        'button_power_reason_jump_table': [hex(v) for v in struct.unpack_from('<16Q', abl, 0x4a2f0)],
        'findings': [
            'The two-option selection graphic is inside stock recovery, so replacement removes that UI.',
            'The user reports a Power-only forced restart from this UI returns to the same UI.',
            'ABL calls button boot-flow analysis before RecoveryInit, then prioritizes the fastboot flag.',
            'Power reasons 8/16 (USB/wall charger) with UEFI scan code 1 select boot-flow mode 4, setting fastboot.',
            'The physical button and power-cycle procedure still require validation on this spare.',
        ],
        'physical_independent_exit_verified': False,
        'diagnostic_installation_ready': False,
    }
    (OUT / 'report.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(report, ensure_ascii=True, indent=2))


if __name__ == '__main__':
    main()
