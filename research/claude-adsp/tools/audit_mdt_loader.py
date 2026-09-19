#!/usr/bin/env python3
"""Offline re-implementation of the pinned kernel's mdt_loader.c acceptance checks.

Source of truth: sdm660-mainline/linux commit e47d622cb6d2440a9eacdc8bb2df32c037bec7b8
(Linux 7.2.3), drivers/soc/qcom/mdt_loader.c:
  mdt_header_valid(), mdt_phdr_loadable(), qcom_mdt_read_metadata(),
  qcom_mdt_bins_are_split(), qcom_mdt_load_no_init(), __qcom_mdt_pas_init().
It reports exactly what the driver would compute (min/max paddr, relocation,
metadata length passed to QCOM_SCM_PIL_PAS_INIT_IMAGE, PAS_MEM_SETUP size) and
whether every split segment file has the p_filesz the loader will demand.
It does NOT authenticate signatures; only TZ can.

Usage: audit_mdt_loader.py <firmware.mdt> <region_base_hex> <region_size_hex> [-o report.json]
"""
import hashlib, json, os, struct, sys

PT_LOAD = 1
QCOM_MDT_TYPE_MASK = 7 << 24
QCOM_MDT_TYPE_HASH = 2 << 24
QCOM_MDT_RELOCATABLE = 1 << 27
SZ_4K = 0x1000

def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for c in iter(lambda: f.read(1 << 20), b''):
            h.update(c)
    return h.hexdigest()

def align(v, a):
    return (v + a - 1) & ~(a - 1)

def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-o')]
    out = None
    if '-o' in sys.argv:
        out = sys.argv[sys.argv.index('-o') + 1]
        args = [a for a in args if a != out]
    mdt, base, size = args[0], int(args[1], 16), int(args[2], 16)
    rep = {'mdt': os.path.abspath(mdt), 'mdt_sha256': sha256(mdt), 'mdt_size': os.path.getsize(mdt),
           'region': {'base': hex(base), 'size': hex(size), 'end': hex(base + size)},
           'checks': [], 'segments': [], 'passed': True}
    def check(name, ok, detail=''):
        rep['checks'].append({'check': name, 'ok': bool(ok), 'detail': detail})
        if not ok:
            rep['passed'] = False
    data = open(mdt, 'rb').read()
    # mdt_header_valid()
    check('elf_magic', data[:4] == b'\x7fELF')
    e_phoff, = struct.unpack_from('<I', data, 28)
    e_shoff, = struct.unpack_from('<I', data, 32)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from('<HHHH', data, 42)
    check('e_phentsize == 32', e_phentsize == 32, str(e_phentsize))
    check('phdr table inside mdt', e_phoff + 32 * e_phnum <= len(data), f'{e_phoff}+32*{e_phnum} <= {len(data)}')
    check('section table absent or valid', (e_shentsize == 0 and e_shnum == 0) or (e_shentsize == 40 and e_shoff + 40 * e_shnum <= len(data)))
    phdrs = []
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align = struct.unpack_from('<IIIIIIII', data, e_phoff + 32 * i)
        phdrs.append(dict(i=i, p_type=p_type, p_offset=p_offset, p_vaddr=p_vaddr, p_paddr=p_paddr,
                          p_filesz=p_filesz, p_memsz=p_memsz, p_flags=p_flags, p_align=p_align))
    # qcom_mdt_read_metadata()
    check('e_phnum >= 2', e_phnum >= 2, str(e_phnum))
    check('phdr[0] is not PT_LOAD (ELF header segment)', phdrs[0]['p_type'] != PT_LOAD, hex(phdrs[0]['p_type']))
    hash_seg = 0
    for i in range(1, e_phnum):
        if (phdrs[i]['p_flags'] & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH:
            hash_seg = i
            break
    check('hash segment present', hash_seg != 0, f'index {hash_seg}')
    ehdr_size = phdrs[0]['p_filesz']
    hash_size = phdrs[hash_seg]['p_filesz'] if hash_seg else 0
    split_packed = (ehdr_size + hash_size == len(data))
    rep['metadata'] = {'ehdr_size': ehdr_size, 'hash_size': hash_size, 'metadata_len_to_PAS_INIT_IMAGE': ehdr_size + hash_size,
                       'split_packed_hash_after_ehdr': split_packed,
                       'hash_within_mdt': (not split_packed) and (phdrs[hash_seg]['p_offset'] + hash_size <= len(data)) if hash_seg else None}
    check('metadata obtainable from .mdt alone', split_packed or (hash_seg and phdrs[hash_seg]['p_offset'] + hash_size <= len(data)))
    # qcom_mdt_bins_are_split()
    is_split = any(p['p_filesz'] and (p['p_offset'] > len(data) or p['p_offset'] + p['p_filesz'] > len(data)) for p in phdrs)
    rep['is_split'] = is_split
    # __qcom_mdt_pas_init() / qcom_mdt_load_no_init()
    min_addr, max_addr, relocate = 2**64, 0, False
    prefix = mdt[:-3]  # strip 'mdt'
    for p in phdrs:
        loadable = p['p_type'] == PT_LOAD and (p['p_flags'] & QCOM_MDT_TYPE_MASK) != QCOM_MDT_TYPE_HASH and p['p_memsz'] != 0
        p['loadable'] = loadable
        p['is_hash'] = (p['p_flags'] & QCOM_MDT_TYPE_MASK) == QCOM_MDT_TYPE_HASH
        p['relocatable'] = bool(p['p_flags'] & QCOM_MDT_RELOCATABLE)
        if not loadable:
            continue
        if p['relocatable']:
            relocate = True
        min_addr = min(min_addr, p['p_paddr'])
        max_addr = max(max_addr, align(p['p_paddr'] + p['p_memsz'], SZ_4K))
    mem_reloc = min_addr if relocate else base
    rep['loader'] = {'relocate': relocate, 'min_addr': hex(min_addr), 'max_addr_4k_aligned': hex(max_addr),
                     'pas_mem_setup_size': hex(max_addr - min_addr), 'mem_reloc': hex(mem_reloc),
                     'pas_mem_setup_call': f'QCOM_SCM_PIL_PAS_MEM_SETUP(pas_id, addr={hex(base)}, size={hex(max_addr - min_addr)})' if relocate else 'not relocatable: no PAS_MEM_SETUP'}
    check('firmware fits reserved region', max_addr - min_addr <= size, f'{hex(max_addr - min_addr)} <= {hex(size)}')
    for p in phdrs:
        seg = {k: (hex(v) if isinstance(v, int) and k not in ('i',) else v) for k, v in p.items()}
        if p['loadable']:
            offset = p['p_paddr'] - mem_reloc
            seg['load_offset_in_region'] = hex(offset)
            seg['load_phys'] = hex(base + offset)
            check(f'seg{p["i"]} inside region', 0 <= offset and offset + p['p_memsz'] <= size)
            check(f'seg{p["i"]} p_filesz <= p_memsz', p['p_filesz'] <= p['p_memsz'])
            if p['p_filesz'] and is_split:
                fn = f'{prefix}b{p["i"]:02d}'
                seg['split_file'] = os.path.basename(fn)
                exists = os.path.exists(fn)
                seg['split_file_size'] = os.path.getsize(fn) if exists else None
                seg['split_file_sha256'] = sha256(fn) if exists else None
                check(f'seg{p["i"]} split file exists with exact p_filesz', exists and os.path.getsize(fn) == p['p_filesz'],
                      f'{os.path.basename(fn)} size {seg["split_file_size"]} vs p_filesz {p["p_filesz"]}')
        elif p['is_hash']:
            seg['note'] = 'hash segment: skipped by loader, carried in metadata'
        rep['segments'].append(seg)
    js = json.dumps(rep, indent=1)
    if out:
        open(out, 'w').write(js)
    print(js if not out else f'wrote {out}; passed={rep["passed"]}')

if __name__ == '__main__':
    main()
