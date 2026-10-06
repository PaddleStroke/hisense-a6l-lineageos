#!/usr/bin/env python3
"""Sync device build inputs by content, preserving timestamps of unchanged files.

Code/text CRLF is normalised before comparison. Excluded top-level entries
remain untouched. This prepares inputs only; it does not run a build or flash.
"""
import argparse
from pathlib import Path
import os
import shutil

TEXT_SUFFIXES = {'.mk', '.rc', '.bp', '.xml', '.c', '.cc', '.cpp', '.h', '.te',
                 '.sh', '.conf', '.txt', '.java', '.kl', '.idc', '.py'}
TEXT_NAMES = {'file_contexts', 'genfs_contexts'}


def content(path):
    data = path.read_bytes()
    if path.suffix in TEXT_SUFFIXES or path.name in TEXT_NAMES or path.name.endswith('_contexts'):
        data = data.replace(b'\r\n', b'\n')
    return data


def sync(source, target, preserve=(), files=None):
    source, target = Path(source), Path(target)
    if not source.is_dir() or source.is_symlink() or target.is_symlink():
        raise ValueError('source must be a directory; root directory symlinks are refused')
    if source.resolve() == target.resolve() or source.resolve() in target.resolve().parents or target.resolve() in source.resolve().parents:
        raise ValueError('source and target must be separate trees')
    target.mkdir(parents=True, exist_ok=True)
    changed = 0

    def remove(path):
        nonlocal changed
        if path.is_symlink() or path.is_file():
            path.unlink()
        elif path.is_dir():
            shutil.rmtree(path)
        changed += 1

    def visit(src, dst):
        nonlocal changed
        if src.is_symlink():
            link = os.readlink(src)
            if dst.is_symlink() and os.readlink(dst) == link:
                return
            if dst.exists() or dst.is_symlink():
                remove(dst)
            dst.symlink_to(link)
            changed += 1
        elif src.is_dir():
            if dst.is_symlink() or (dst.exists() and not dst.is_dir()):
                remove(dst)
            dst.mkdir(exist_ok=True)
            names = {p.name for p in src.iterdir()}
            for child in dst.iterdir():
                if child.name not in names:
                    remove(child)
            for child in src.iterdir():
                visit(child, dst / child.name)
        else:
            data = content(src)
            if dst.is_file() and not dst.is_symlink() and dst.read_bytes() == data:
                return
            if dst.is_symlink() or dst.is_dir():
                remove(dst)
            dst.write_bytes(data)
            shutil.copymode(src, dst)
            changed += 1

    names = {p.name for p in source.iterdir()}
    for child in target.iterdir():
        if files is None and child.name not in names and child.name not in preserve:
            remove(child)
    for child in source.iterdir():
        if child.name not in preserve and (files is None or child.name in files):
            visit(child, target / child.name)
    return changed


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, nargs='?')
    parser.add_argument('target', type=Path, nargs='?')
    parser.add_argument('--preserve', action='append', default=[])
    parser.add_argument('--files', nargs='+')
    parser.add_argument('--normalize', type=Path, nargs='+')
    parser.add_argument('--staging', action='store_true', help='allow a temporary ROM staging destination')
    args = parser.parse_args()
    device = Path('/home/a6l/android/a6l-lineage24/device/hisense/a6l')
    if args.normalize:
        changed = 0
        for root in args.normalize:
            if not root.resolve().is_relative_to(device.resolve()):
                parser.error('normalisation must stay inside the A6L build device tree')
            for path in ([root] if root.is_file() else root.rglob('*')):
                if path.is_file() and not path.is_symlink():
                    data = content(path)
                    if data != path.read_bytes():
                        path.write_bytes(data)
                        changed += 1
        print('Line endings changed:', changed)
        raise SystemExit(0)
    if args.source is None or args.target is None:
        parser.error('source and target are required for synchronisation')
    repo_device = Path(__file__).resolve().parent.parent / 'device/hisense/a6l'
    expected_source = repo_device.resolve() if args.files else repo_device.resolve() / args.source.name
    expected_target = device if args.files else device / args.source.name
    staging_target = False
    if args.staging:
        staging_target = (args.source.resolve() == repo_device.resolve() / 'rom'
                          and tuple(args.target.parts[-4:]) == ('device', 'hisense', 'a6l', 'rom')
                          and (args.target.resolve().is_relative_to(Path('/tmp'))
                               or args.target.resolve().is_relative_to(Path('/home/a6l/scratch'))))
        if not staging_target:
            parser.error('temporary staging requires the ROM subtree under /tmp or /home/a6l/scratch')
    if args.source.resolve() != expected_source or (args.target.absolute() != expected_target and not staging_target):
        parser.error('sync requires a repository A6L subtree and its matching A6L build destination')
    if any('/' in name or '\\' in name or name in ('', '.', '..') for name in args.preserve + (args.files or [])):
        parser.error('--preserve/--files requires a top-level entry name')
    if args.files and any(not (args.source / name).is_file() for name in args.files):
        parser.error('--files names must exist as source files')
    print('Device input entries changed:', sync(args.source, args.target, args.preserve, args.files))
