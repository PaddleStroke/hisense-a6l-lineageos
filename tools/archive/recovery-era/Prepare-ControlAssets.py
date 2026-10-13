"""Preserve stock Bluetooth firmware and resolve audio route dependencies offline."""
import hashlib,io,json,shutil,xml.etree.ElementTree as ET
from pathlib import Path
from dissect.fat import fat
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/controls-radio-prep-20260917'
DEST=OUT/'stock-assets';DEST.mkdir(exist_ok=False)
sha=lambda b:hashlib.sha256(b).hexdigest()
report=json.loads((ROOT/'firmware/raw-backup-20260914/firmware-verification.json').read_text())
part=next(p for p in report['partitions'] if p['name']=='bluetooth')
with (ROOT/'firmware/raw-backup-20260914/emmc-firmware-prefix.bin').open('rb') as f:
    f.seek(part['offset']);data=f.read(part['bytes'])
assert len(data)==part['bytes'] and sha(data)==part['sha256']
fs=fat.FATFS(io.BytesIO(data));inventory=[]
def walk(node,rel):
    for child in list(node.iterdir()):
        n=child.name
        if n in ['.','..'] or child.is_volume_id():continue
        assert n and not any(c in n for c in '/\\:')
        path=rel/n;target=DEST/'bluetooth'/path
        assert target.resolve().is_relative_to(DEST.resolve())
        if child.is_directory():walk(child,path)
        else:
            with child.open() as f:content=f.read()
            assert len(content)==child.size and not target.exists()
            target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(content)
            inventory.append({'file':target.relative_to(OUT).as_posix(),'bytes':len(content),'sha256':sha(content)})
walk(fs.root,Path())
etc=ROOT/'firmware/extracted/vendor/etc'
routes={}
targets=['handset','handset-mic','headphones','headset-mic','speaker-mic','voice-handset','voice-headset']
for source in sorted(etc.glob('mixer_paths*.xml')):
    root=ET.parse(source).getroot();paths={p.attrib['name']:p for p in root.findall('path')}
    def expand(name,stack=()):
        assert name not in stack,(source,name,stack)
        if name not in paths:return [{'unresolved_path':name}]
        result=[]
        for child in paths[name]:
            if child.tag=='ctl':result.append(dict(child.attrib))
            elif child.tag=='path':result.extend(expand(child.attrib['name'],stack+(name,)))
        return result
    routes[source.name]={'sha256':sha(source.read_bytes()),'routes':{n:expand(n) for n in targets if n in paths}}
    dest=DEST/'audio'/source.name;dest.parent.mkdir(exist_ok=True);shutil.copyfile(source,dest)
    inventory.append({'file':dest.relative_to(OUT).as_posix(),'bytes':dest.stat().st_size,'sha256':sha(dest.read_bytes())})
for name in ['gps.conf','izat.conf','flp.conf','lowi.conf','sap.conf']:
    source=etc/name
    if source.is_file():
        dest=DEST/'gnss'/name;dest.parent.mkdir(exist_ok=True);shutil.copyfile(source,dest)
        inventory.append({'file':dest.relative_to(OUT).as_posix(),'bytes':dest.stat().st_size,'sha256':sha(dest.read_bytes())})
(OUT/'stock-assets-manifest.json').write_text(json.dumps({'bluetooth_partition':part,'source_open_mode':'rb','assets':inventory,'phone_access':False},indent=2)+'\n')
(OUT/'stock-audio-routes.json').write_text(json.dumps({'routes':routes,'warning':'Stock mixer names/gains are reference evidence, not commands for upstream ALSA. Active stock XML must be confirmed.'},indent=2)+'\n')
print('CONTROL_ASSETS_PASS',len(inventory),'files;',len(routes),'audio XML variants; Bluetooth partition hash verified')
