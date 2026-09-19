#!/usr/bin/env python3
"""Offline DT_NEEDED audit using the stock vendor linker namespace configuration.

This models search paths and whitelisted namespace links, not the whole linker:
dlopen, inherited/global groups, symbol versions and interposition need separate
analysis. Missing symbol candidates are leads, not proof of a runtime failure.
"""
import argparse
from collections import Counter, deque
from functools import lru_cache
import hashlib
import json
from pathlib import Path
import posixpath
import re
from elftools.elf.elffile import ELFFile

p = argparse.ArgumentParser()
p.add_argument('extracted', type=Path)
p.add_argument('output', type=Path)
p.add_argument('roots', nargs='+', help='Absolute Android paths of vendor ELF roots')
p.add_argument('--without-vndk28', action='store_true', help='Controlled missing-library experiment, not an Android 17 linker simulation')
a = p.parse_args()
entries = {}
for partition, prefix in [('system', ''), ('vendor', '/vendor')]:
    for item in json.loads((a.extracted / f'{partition}-inventory.json').read_text()):
        virtual = posixpath.normpath(prefix + '/' + item['path'])
        entries[virtual] = (item, a.extracted / partition / item['path'])

def resolve_file(path):
    path = posixpath.normpath(path)
    for _ in range(32):
        parts = path.split('/')[1:]
        for i in range(len(parts)):
            prefix = '/' + '/'.join(parts[:i+1])
            entry = entries.get(prefix)
            if entry and entry[0]['type'] == 'symlink':
                target = entry[0]['target']
                if not target.startswith('/'):
                    target = posixpath.join(posixpath.dirname(prefix), target)
                path = posixpath.normpath(posixpath.join(target, *parts[i+1:]))
                break
        else:
            if a.without_vndk28 and any(x in path for x in ('/vndk-28/', '/vndk-sp-28/')):
                return None
            entry = entries.get(path)
            return (path, entry[1]) if entry and entry[0]['type'] == 'file' else None
    raise ValueError(f'Symlink cycle at {path}')

config = {}
section = None
for line in (a.extracted / 'system/system/etc/ld.config.28.txt').read_text().splitlines():
    line = line.split('#', 1)[0].strip()
    if line.startswith('['):
        section = line
    elif section == '[vendor]' and '=' in line:
        match = re.fullmatch(r'(\S+)\s*(\+?=)\s*(.*)', line)
        if match:
            key, op, value = match.groups()
            config[key] = config.get(key, '') + ':' + value if op == '+=' else value

@lru_cache(None)
def elf_info(path):
    resolved = resolve_file(path)
    if not resolved:
        raise ValueError(f'Unavailable ELF {path}')
    _, local = resolved
    with local.open('rb') as f:
        elf = ELFFile(f)
        dyn = elf.get_section_by_name('.dynamic')
        symbols = elf.get_section_by_name('.dynsym')
        needed = [t.needed for t in dyn.iter_tags() if t.entry.d_tag == 'DT_NEEDED'] if dyn else []
        strong, exports = set(), set()
        for s in symbols.iter_symbols() if symbols else []:
            if s['st_shndx'] == 'SHN_UNDEF':
                if s.name and s['st_info']['bind'] == 'STB_GLOBAL':
                    strong.add(s.name)
            elif s.name and s['st_info']['bind'] in ('STB_GLOBAL', 'STB_WEAK'):
                exports.add(s.name)
        digest = hashlib.sha256(local.read_bytes()).hexdigest()
        if digest != entries[resolved[0]][0]['sha256']:
            raise ValueError(f'Extracted file differs from inventory: {path}')
        return dict(bits=elf.elfclass, machine=elf['e_machine'], needed=needed,
                    strong=strong, exports=exports, sha256=digest)

def find_library(name, namespace, bits, visited=()):
    if namespace in visited:
        return None
    key = 'namespace.' + namespace
    for directory in config.get(key + '.search.paths', '').split(':'):
        if not directory:
            continue
        result = resolve_file(directory.replace('${LIB}', 'lib64' if bits == 64 else 'lib') + '/' + name)
        if result and elf_info(result[0])['bits'] == bits:
            return result[0], namespace
    for linked in config.get(key + '.links', '').split(','):
        if not linked:
            continue
        link = key + '.link.' + linked
        allowed = config.get(link + '.allow_all_shared_libs') == 'true'
        if allowed or name in config.get(link + '.shared_libs', '').split(':'):
            result = find_library(name, linked, bits, visited + (namespace,))
            if result:
                return result
    return None

reports = []
for root in a.roots:
    if not root.startswith('/vendor/'):
        raise ValueError('This audit only models vendor ELF roots')
    nodes, edges, missing = {}, [], []
    queue = deque([(root, 'default')])
    while queue:
        path, ns = queue.popleft()
        key = ns + ':' + path
        if key in nodes:
            continue
        info = elf_info(path)
        nodes[key] = dict(path=path, namespace=ns, bits=info['bits'], sha256=info['sha256'])
        for name in info['needed']:
            found = find_library(name, ns, info['bits'])
            edge = dict(source=key, needed=name, target=(found[1]+':'+found[0]) if found else None)
            edges.append(edge)
            if found:
                queue.append(found)
            else:
                missing.append(edge)
    all_exports = set().union(*(elf_info(n['path'])['exports'] for n in nodes.values()))
    missing_symbols = {k: sorted(elf_info(n['path'])['strong'] - all_exports) for k, n in nodes.items()}
    missing_symbols = {k: v for k, v in missing_symbols.items() if v}
    counts = Counter('vndk28' if '/vndk' in n['path'] else 'vendor' if n['path'].startswith('/vendor/')
                     else 'system' for n in nodes.values())
    reports.append(dict(root=root, counts=dict(counts), nodes=nodes, edges=edges,
                        unresolved_needed=missing, strong_symbols_without_candidate=missing_symbols))
a.output.parent.mkdir(parents=True, exist_ok=True)
a.output.write_text(json.dumps(dict(scope='Stock vendor namespace DT_NEEDED model; not a runtime compatibility test',
                                  without_vndk28=a.without_vndk28,
                                  reports=reports), indent=2)+'\n')
for r in reports:
    print(r['root'], r['counts'], 'unresolved libraries:', len(r['unresolved_needed']),
          'objects with missing symbol candidates:', len(r['strong_symbols_without_candidate']))
