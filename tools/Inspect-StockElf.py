#!/usr/bin/env python3
"""Disassemble selected AArch64 ELF symbols offline; never execute stock code."""
import argparse
from bisect import bisect_right
import re
from elftools.elf.elffile import ELFFile
from capstone import Cs, CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN

p = argparse.ArgumentParser()
p.add_argument('elf')
p.add_argument('--symbol', default='Epd|DisplayType|onTransact')
p.add_argument('--address', type=lambda x: int(x, 0))
p.add_argument('--size', type=lambda x: int(x, 0), default=256)
p.add_argument('--infer-zero-sizes', action='store_true',
               help='For recovered symbols, inspect up to the next symbol or --size bytes; not an exact function bound')
a = p.parse_args()
with open(a.elf, 'rb') as f:
    e = ELFFile(f)
    if e['e_machine'] != 'EM_AARCH64':
        raise SystemExit('Expected AArch64')
    symbol_table = e.get_section_by_name('.dynsym') or e.get_section_by_name('.symtab')
    if symbol_table is None:
        raise SystemExit('ELF contains neither dynamic nor regular symbols')
    syms = list(symbol_table.iter_symbols())
    labels = {s['st_value']: s.name for s in syms if s['st_value']}
    rel = e.get_section_by_name('.rela.plt')
    plt = e.get_section_by_name('.plt')
    if rel and plt:
        linked = e.get_section(rel['sh_link'])
        for i, r in enumerate(rel.iter_relocations()):
            labels[plt['sh_addr'] + 32 + i * 16] = linked.get_symbol(r['r_info_sym']).name + '@plt'
    ranges = [(a.address, a.size, 'address range')] if a.address is not None else [
        (s['st_value'], s['st_size'], s.name) for s in syms
        if s['st_value'] and s['st_info']['type'] == 'STT_FUNC' and re.search(a.symbol, s.name)]
    cs = Cs(CS_ARCH_ARM64, CS_MODE_LITTLE_ENDIAN)
    addresses = sorted(labels)
    for addr, size, name in ranges:
        if not size and a.infer_zero_sizes:
            index = bisect_right(addresses, addr)
            size = min(a.size, addresses[index] - addr) if index < len(addresses) else a.size
            name += ' [inferred inspection range, not debug-info size]'
        print(f'\n{name} at 0x{addr:x}, {size} bytes')
        segment = next(s for s in e.iter_segments() if s['p_type'] == 'PT_LOAD' and
                       s['p_vaddr'] <= addr and addr + size <= s['p_vaddr'] + s['p_filesz'])
        f.seek(segment['p_offset'] + addr - segment['p_vaddr'])
        for ins in cs.disasm(f.read(size), addr):
            annotation = ''
            if ins.mnemonic in ('bl', 'b') and ins.op_str.startswith('#'):
                annotation = labels.get(int(ins.op_str[1:], 0), '')
            print(f'{ins.address:08x}: {ins.mnemonic:8} {ins.op_str:40} {annotation}')
