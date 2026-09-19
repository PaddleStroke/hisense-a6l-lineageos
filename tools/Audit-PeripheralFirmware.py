"""Validate stock split ELF files and prepare hashed, unchanged local firmware assets.

This does not authenticate Qualcomm signatures, choose a WLAN board variant,
start a processor, or stage anything on the phone.
"""
import hashlib,json,shutil,struct
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
SRC=ROOT/'firmware/extracted/peripheral-firmware-20260917-r3'
OUT=ROOT/'firmware/extracted/peripheral-prep-20260917'
DEST=OUT/'firmware/qcom/hisense/a6l';DEST.mkdir(parents=True,exist_ok=False)
inventory=json.loads((SRC/'inventory.json').read_text())
byname={Path(x['path']).name.lower():x for x in inventory if x['partition']=='modem' and x['path'].startswith('IMAGE/')}
files=[]
def copy(name,sub=''):
    item=byname[name.lower()];p=SRC/'modem'/item['path'];data=p.read_bytes()
    assert hashlib.sha256(data).hexdigest()==item['sha256']
    target=DEST/sub/name.lower();target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(data)
    files.append({'source':str(p.relative_to(ROOT)),'destination':str(target.relative_to(OUT)),'sha256':item['sha256'],'bytes':len(data)})
    return data
groups=[]
for stem in ['modem','adsp','cdsp']:
    data=copy(stem+'.mdt')
    assert data[:6]==b'\x7fELF\x01\x01',stem
    header=struct.unpack_from('<16sHHIIIIIHHHHHH',data)
    phoff=header[5];phentsize=header[9];phnum=header[10]
    assert phentsize==32 and phoff+phnum*phentsize<=len(data)
    segments=[]
    for i in range(phnum):
        typ,off,va,pa,fsz,msz,flags,align=struct.unpack_from('<IIIIIIII',data,phoff+i*phentsize)
        if typ!=1 or (flags & (7<<24))==(2<<24) or not msz:continue
        assert fsz<=msz
        entry={'index':i,'physical_address':hex(pa),'file_bytes':fsz,'memory_bytes':msz,'relocatable':bool(flags&(1<<27))}
        if fsz:
            payload=copy(stem+f'.b{i:02d}')
            assert len(payload)==fsz,(stem,i,len(payload),fsz)
        segments.append(entry)
    groups.append({'name':stem,'elf_class':32,'load_segments':segments,'file_segments_valid':True})
for name in ['mba.mbn','wlanmdsp.mbn','modemr.jsn','modemuw.jsn','adspr.jsn','adsps.jsn','adspua.jsn','cdspr.jsn']:
    copy(name)
boards=[]
for name in sorted(byname):
    if name.startswith('bdwlan.'):
        data=copy(name,'unselected-board-data')
        boards.append({'name':name,'bytes':len(data),'sha256':hashlib.sha256(data).hexdigest()})
audio=OUT/'stock-audio';audio.mkdir(exist_ok=False)
vendor=ROOT/'firmware/extracted/vendor'
paths=[vendor/'firmware/tfa98xx.cnt']
paths+=list((vendor/'etc').glob('mixer_paths*.xml'))+list((vendor/'etc').glob('audio_platform_info*.xml'))
paths+=list((vendor/'etc').glob('audio_policy*'))
for p in paths:
    if not p.is_file():continue
    target=audio/p.relative_to(vendor);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(p,target)
    files.append({'source':str(p.relative_to(ROOT)),'destination':str(target.relative_to(OUT)),'sha256':hashlib.sha256(target.read_bytes()).hexdigest(),'bytes':target.stat().st_size})
report={'passed':True,'split_elf_groups':groups,'board_data_candidates':boards,'selected_board_data':None,'files':files,'signature_authentication_tested':False,'phone_tested':False,'note':'WLAN board selection requires live QMI chip/board IDs or verified stock selection. No generic board.bin installed. MDT validation covers file structure/segment sizes, not trusted firmware authentication.'}
(OUT/'firmware-manifest.json').write_text(json.dumps(report,indent=2)+'\n')
print('PERIPHERAL_FIRMWARE_STRUCTURE_PASS',len(files),'files;',len(boards),'unselected board candidates')
