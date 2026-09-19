#!/usr/bin/env python3
"""Disassemble verified extracted ABL code offline; candidate ADR(P) string refs."""
import argparse
import hashlib
import json
from pathlib import Path
import re
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_REG

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--start', type=lambda value: int(value, 0))
parser.add_argument('--end', type=lambda value: int(value, 0))
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent / 'firmware/extracted/stock-abl-20260914'
data = (root / 'LinuxLoader.efi').read_bytes()
assert hashlib.sha256(data).hexdigest() == '6bbe53f345729c77c9a0ba74aa7f7aadb6cd68b0ac1511f6f160bc69de6ab523'
md = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
md.detail = True
if args.start is not None:
    assert 0x1000 <= args.start < args.end <= 0x51000
    for ins in md.disasm(data[args.start:args.end], args.start):
        print(f'{ins.address:08x}  {ins.mnemonic:8} {ins.op_str}')
else:
    refs = []
    for offset in range(0x1000, 0x3e000, 4):
        decoded = list(md.disasm(data[offset:offset + 4], offset))
        if not decoded or decoded[0].mnemonic not in ('adr', 'adrp'):
            continue
        ins = decoded[0]
        reg, address = ins.operands[0].reg, ins.operands[1].imm
        candidates = [(ins.address, address)] if ins.mnemonic == 'adr' else []
        if ins.mnemonic == 'adrp':
            for next_ins in md.disasm(data[offset + 4:offset + 36], offset + 4):
                ops = next_ins.operands
                if (next_ins.mnemonic == 'add' and len(ops) == 3 and
                        ops[1].type == ARM64_OP_REG and ops[1].reg == reg and
                        ops[2].type == ARM64_OP_IMM):
                    candidates.append((next_ins.address, address + ops[2].imm))
                if reg in next_ins.regs_access()[1] or next_ins.mnemonic in ('ret', 'b', 'br'):
                    break
        for reference, target in candidates:
            if not 0x3e000 <= target < len(data):
                continue
            end = data.find(b'\0', target, min(target + 300, len(data)))
            if end < 0:
                continue
            value = data[target:end]
            if len(value) >= 3 and re.fullmatch(rb'[ -~\n\r]+', value):
                refs.append({'adr': hex(ins.address), 'reference': hex(reference),
                             'target': hex(target), 'string': value.decode()})
    (root / 'loader-candidate-string-refs.json').write_text(json.dumps(refs, indent=2) + '\n')
    wanted = re.compile(r'Kernel (Load Address|base address|mode check)|unknown command|^getvar:|^Hisense$|^boot$|^max-download-size$|^download:$|^flashing unlock$')
    print(json.dumps([r for r in refs if wanted.search(r['string'])], indent=2))
