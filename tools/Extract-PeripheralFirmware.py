"""Extract stock modem/DSP firmware from verified backup ranges, read-only."""
import hashlib,io,json,stat
from pathlib import Path
from dissect.fat import fat
from dissect.extfs import extfs
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT/'firmware/raw-backup-20260914'
OUT=ROOT/'firmware/extracted/peripheral-firmware-20260917-r3'
OUT.mkdir(exist_ok=False)
report=json.loads((BASE/'firmware-verification.json').read_text())
inventory=[]
for name in ['modem','dsp']:
    part=next(p for p in report['partitions'] if p['name']==name)
    with (BASE/'emmc-firmware-prefix.bin').open('rb') as source:
        source.seek(part['offset']);data=source.read(part['bytes'])
    assert len(data)==part['bytes'] and hashlib.sha256(data).hexdigest()==part['sha256']
    dest=OUT/name;dest.mkdir()
    isfat=name=='modem'
    fs=fat.FATFS(io.BytesIO(data)) if isfat else extfs.ExtFS(io.BytesIO(data))
    seen=set()
    def walk(node,rel):
        # Materialize entries before opening children: FAT streams share a seekable backing file.
        for child in list(node.iterdir()):
            n=child.name if isfat else child.filename
            if n in ['.','..']:continue
            assert n and not any(c in n for c in '/\\:') and n not in ['.','..']
            path=rel/n; target=dest/path
            assert target.resolve().is_relative_to(dest.resolve())
            original_path=path.as_posix()
            if str(path).casefold() in seen:
                # Stock VERINFO contains two different entries named VER_INFO.TXT.
                # Preserve both with explicit cluster suffixes, never silently overwrite.
                assert isfat and not child.is_directory(), str(path)
                path=path.with_name(path.name+'.fat-cluster-'+str(child.cluster))
                target=dest/path
                assert str(path).casefold() not in seen
            seen.add(str(path).casefold())
            if isfat and child.is_volume_id():continue
            directory=child.is_directory() if isfat else child.filetype==stat.S_IFDIR
            if directory:
                target.mkdir();walk(child,path)
            elif isfat or child.filetype==stat.S_IFREG:
                with child.open() as f:content=f.read()
                assert len(content)==child.size
                target.write_bytes(content)
                inventory.append({'partition':name,'path':path.as_posix(),'original_path':original_path,'bytes':len(content),'sha256':hashlib.sha256(content).hexdigest()})
            else:
                inventory.append({'partition':name,'path':path.as_posix(),'special_mode':child.filetype})
    walk(fs.root,Path())
    print(name,'verified partition SHA256',part['sha256'],'files',sum(x['partition']==name for x in inventory),flush=True)
(OUT/'inventory.json').write_text(json.dumps(inventory,indent=2)+'\n')
(OUT/'provenance.json').write_text(json.dumps({'partitions':[p for p in report['partitions'] if p['name'] in ['modem','dsp']],'parser':'dissect.fat 3.13 / dissect.extfs','source_open_mode':'rb','phone_access':False,'flashable_image':False},indent=2)+'\n')
