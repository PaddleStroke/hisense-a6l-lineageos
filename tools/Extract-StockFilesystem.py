"""Read system/vendor from the verified backup; never mount or modify an image.

Regular files are extracted for inspection. Symlinks/special files and original
ownership/mode are recorded in the inventory, not instantiated on Windows.
This output is not a flashable filesystem image.
"""
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
from dissect.extfs import extfs
from dissect.util.stream import RangeStream

BASE = Path('firmware/raw-backup-20260914')
OUT = Path('firmware/extracted')


def extract(partition):
    report = json.loads((BASE/'firmware-verification.json').read_text())
    part = next(p for p in report['partitions'] if p['name'] == partition)
    destination = (OUT/partition).resolve()
    destination.mkdir(parents=True, exist_ok=True)
    inventory = []
    seen = set()
    with (BASE/'emmc-firmware-prefix.bin').open('rb') as image:
        fs = extfs.ExtFS(RangeStream(image, part['offset'], part['bytes']))
        def visit(node, relative):
            if relative:
                components = PurePosixPath(relative).parts
                if any(p in ('..', '.', '') or ':' in p or '\\' in p for p in components):
                    raise ValueError('Unsafe filesystem name')
                key = relative.casefold()
                if key in seen: raise ValueError('Case collision: '+relative)
                seen.add(key)
            target = destination.joinpath(*PurePosixPath(relative).parts)
            if not target.resolve().is_relative_to(destination): raise ValueError('Path escapes output')
            item = dict(path=relative, inode=node.inum, bytes=node.size, mode=node.inode.i_mode,
                        uid=node.inode.i_uid, gid=node.inode.i_gid)
            if node.filetype == stat.S_IFDIR:
                target.mkdir(exist_ok=True)
                item['type']='directory'
                inventory.append(item)
                for child in node.iterdir():
                    if child.filename in ('.','..'): continue
                    visit(child, relative+'/'+child.filename if relative else child.filename)
            elif node.filetype == stat.S_IFREG:
                digest=hashlib.sha256()
                count=0
                with node.open() as source, target.open('wb') as output:
                    while data:=source.read(4*1024*1024):
                        output.write(data); digest.update(data); count+=len(data)
                if count!=node.size: raise ValueError('Truncated file: '+relative)
                item.update(type='file',sha256=digest.hexdigest())
                inventory.append(item)
            elif node.filetype == stat.S_IFLNK:
                item.update(type='symlink',target=node.link)
                inventory.append(item)
            else:
                item['type']='special'
                inventory.append(item)
        visit(fs.root,'')
    (OUT/(partition+'-inventory.json')).write_text(json.dumps(inventory,indent=2)+'\n')
    print(partition, 'entries',len(inventory),'regular files',sum(i['type']=='file' for i in inventory),flush=True)


if __name__=='__main__':
    for partition in ['system','vendor']: extract(partition)
