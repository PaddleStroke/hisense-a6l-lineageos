"""RAM payloads for the attended V70 (display/e-ink transport/audio) session. Offline; run inside WSL. Every area = directory with
modules (dependency-ordered order.txt), trusted SHA256SUMS and one bounded script. Nothing here runs by itself."""
import hashlib,json,re,shutil,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/v70-attended-bundle-20260921';OUT.mkdir(exist_ok=False)
WORK=Path('/home/a6l/v67mods/all');shutil.rmtree(WORK,ignore_errors=True);WORK.mkdir(parents=True)
with tarfile.open(KERNEL/'modules.tar.gz') as t:t.extractall(WORK)
for sub in ['extra-modules','display-modules']:
    for extra in (KERNEL/sub).glob('*.ko'):shutil.copyfile(extra,WORK/extra.name)
trusted=(KERNEL/'modules-SHA256SUMS').read_text()+(KERNEL/'extra-modules/SHA256SUMS').read_text()+(KERNEL/'display-modules/SHA256SUMS').read_text()
index={p.name.replace('-','_'):p for p in WORK.rglob('*.ko')}
sha=lambda b:hashlib.sha256(b).hexdigest()
def closure(names):
    order=[]
    def visit(n):
        key=n.replace('-','_');key=key if key.endswith('.ko') else key+'.ko'
        p=index[key]
        if p in order:return
        for d in filter(None,subprocess.check_output(['modinfo','-F','depends',str(p)],text=True).strip().split(',')):visit(d)
        order.append(p)
    for n in names:visit(n)
    return order
def area(name,modules,script,firmware=(),binaries=()):
    d=OUT/name;(d/'modules').mkdir(parents=True)
    order=closure(modules)
    for p in order:
        data=p.read_bytes();assert sha(data) in trusted,p;(d/'modules'/p.name).write_bytes(data)
    (d/'modules/order.txt').write_text('\n'.join(p.name for p in order)+'\n')
    if firmware:
        (d/'firmware').mkdir()
        for src,rel in firmware:
            dest=d/'firmware'/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(src,dest)
    if binaries:
        (d/'bin').mkdir()
        for src in binaries:shutil.copyfile(src,d/'bin'/Path(src).name)
    (d/'run.sh').write_text(COMMON+script)
    files=sorted(str(p.relative_to(d)) for p in d.rglob('*') if p.is_file())
    (d/'SHA256SUMS').write_text(''.join(f'{sha((d/f).read_bytes())}  {f}\n' for f in files))
    print(name,len(order),'modules',len(firmware),'firmware files')
COMMON='''#!/system/bin/sh
# ATTENDED ONLY, V70 diagnostic recovery. usage: D=/tmp/<area> sh run.sh     (PATH must include /tmp/bin toybox links)
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v70 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
( cd "$D" && sha256sum -c SHA256SUMS > /dev/null ) || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_HW_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\\$p"; }
load() { while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules && continue; insmod "$D/modules/$ko" || { echo "A6L_HW_FAIL insmod $ko"; klog | tail -n 15; exit 4; }; done < "$D/modules/order.txt"; }
fw() { [ -d "$D/firmware" ] && { mkdir -p /lib/firmware; cp -r "$D"/firmware/* /lib/firmware/; }; }
'''
V=ROOT/'firmware/extracted/vendor/firmware'
gpufw=[(V/n,'qcom/hisense/a6l/'+n) for n in ['a512_zap.mdt','a512_zap.b00','a512_zap.b01','a512_zap.b02']]+[(V/n,'qcom/'+n) for n in ['a530_pm4.fw','a530_pfp.fw','a530v3_gpmu.fw2']]
area('display',['panel-ft8719-tianma-1080x2340','msm'],'''# Native display takeover: the LCD may go dark for good in this boot if the panel driver is wrong. ADB stays up.
fw; load; sleep 8
ls /sys/class/drm/ | tr "\\n" " "; echo
for c in /sys/class/drm/card*-DSI-*; do echo "$c status=$(cat $c/status) enabled=$(cat $c/enabled) modes=$(head -n 1 $c/modes)"; done
klog | grep -i "msm\\|mdss\\|dsi\\|panel\\|ft8719\\|smmu\\|fault" | tail -n 40
[ -e /sys/class/drm/card*-DSI-1 ] && echo A6L_NATIVE_DISPLAY_CONNECTOR_PASS || echo A6L_NATIVE_DISPLAY_CONNECTOR_MISSING
''',gpufw)
area('eink-dsi',['tps65185','tc358762-a6l'],'''# Requires the display area loaded first (msm.ko owns DSI1). Rails are NOT enabled here; this only checks that the
# bridge and DPI panel bind and a 384x725 connector appears. Driving the panel is a separate, later step.
load; sleep 5
for c in /sys/class/drm/card*-DSI-* /sys/class/drm/card*-DPI-*; do [ -e $c ] && echo "$c status=$(cat $c/status) modes=$(head -n 1 $c/modes)"; done
klog | grep -i "tc358762\\|tps65185\\|panel-dpi\\|dsi@c996000\\|bridge" | tail -n 25
cat /sys/class/drm/card*/modes 2>/dev/null | grep -q 384x725 && echo A6L_EINK_DSI_MODE_PASS || echo A6L_EINK_DSI_MODE_MISSING
''')
area('audio',['qcom_pd_mapper','apr','q6core','q6afe','q6afe-dai','q6afe-clocks','q6adm','q6asm','q6asm-dai','q6routing','snd-soc-msm8916-analog','snd-soc-msm8916-digital','snd-soc-sm8250'],'''# Requires the ADSP RUNNING (adsp bundle, left running). Registers the sound card only: no playback, no capture.
[ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo A6L_HW_FAIL adsp not running; exit 5; }
load; sleep 8
cat /proc/asound/cards 2>&1; cat /proc/asound/pcm 2>&1 | head
klog | grep -i "q6\\|apr\\|snd\\|asoc\\|wcd\\|codec\\|sndcard" | tail -n 30
grep -q "A6L\\|Hisense" /proc/asound/cards 2>/dev/null && echo A6L_SOUND_CARD_PASS || echo A6L_SOUND_CARD_MISSING
''')
manifest={str(p.relative_to(OUT)):sha(p.read_bytes()) for p in sorted(OUT.rglob('*')) if p.is_file()}
(OUT/'manifest.json').write_text(json.dumps({'requires_recovery':'recovery-v70-candidate-20260921','files':manifest,'phone_access':False},indent=2)+'\n')
print('V70_BUNDLES_PASS',len(manifest))
