"""Validate stock QCA TLV framing and prepare case-correct upstream filenames."""
import hashlib,json,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/controls-radio-prep-20260917'
DEST=OUT/'firmware/qca';DEST.mkdir(parents=True,exist_ok=False)
report=[]
def header(data,off):
    assert off+4<=len(data)
    word=struct.unpack_from('<I',data,off)[0]
    return word&255,word>>8
for source in sorted((OUT/'stock-assets/bluetooth').rglob('*')):
    if not source.is_file() or not source.name.startswith('CR'):continue
    data=source.read_bytes();typ,size=header(data,0)
    assert size+4==len(data),source
    item={'source':source.relative_to(ROOT).as_posix(),'type':typ,'size':size,'sha256':hashlib.sha256(data).hexdigest()}
    if typ==1:
        assert size>=24
        product,rom,patch=struct.unpack_from('<HHH',data,16)
        item.update(product_id=product,rom_build=rom,patch_version=patch)
    else:
        assert typ==4
        offset=4;sets=[]
        while offset<len(data):
            kind,length=header(data,offset);end=offset+4+length
            assert end<=len(data)
            tags=[];pos=offset+4
            while pos<end:
                assert pos+12<=end
                tag,taglen=struct.unpack_from('<HH',data,pos)
                assert pos+12+taglen<=end
                tags.append({'id':tag,'length':taglen});pos+=12+taglen
            assert pos==end
            sets.append({'type':kind,'length':length,'tags':tags});offset=end
        assert sets and sets[0]['type']==2
        item['sets']=sets
    dest=DEST/source.name.lower();dest.write_bytes(data)
    item['upstream_file']=dest.relative_to(OUT).as_posix();report.append(item)
assert len(report)==6
(OUT/'bluetooth-firmware-report.json').write_text(json.dumps({'passed':True,'files':report,'rom_variant_selected':False,'scope':'TLV lengths and NVM tag bounds only; no signature, hardware or ROM-compatibility verification'},indent=2)+'\n')
print('BLUETOOTH_TLV_PASS',len(report),'files; variant selection deferred to hardware version query')
