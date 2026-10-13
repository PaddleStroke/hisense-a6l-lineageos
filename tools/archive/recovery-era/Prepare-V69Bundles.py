"""RAM payloads for the attended V69 hardware session. Offline; run inside WSL. Every area = directory with
modules (dependency-ordered order.txt), trusted SHA256SUMS and one bounded script. Nothing here runs by itself."""
import hashlib,json,re,shutil,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/v69-attended-bundle-20260921';OUT.mkdir(exist_ok=False)
WORK=Path('/home/a6l/v67mods/all');shutil.rmtree(WORK,ignore_errors=True);WORK.mkdir(parents=True)
with tarfile.open(KERNEL/'modules.tar.gz') as t:t.extractall(WORK)
for extra in (KERNEL/'extra-modules').glob('*.ko'):shutil.copyfile(extra,WORK/extra.name)
trusted=(KERNEL/'modules-SHA256SUMS').read_text()+(KERNEL/'extra-modules/SHA256SUMS').read_text()
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
# ATTENDED ONLY, V69 diagnostic recovery. usage: D=/tmp/<area> sh run.sh     (PATH must include /tmp/bin toybox links)
set -u
D=${D:-$(dirname "$0")}
[ "$(tr -d '\\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v69 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
( cd "$D" && sha256sum -c SHA256SUMS > /dev/null ) || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_HW_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\\$p"; }
load() { while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules && continue; insmod "$D/modules/$ko" || { echo "A6L_HW_FAIL insmod $ko"; klog | tail -n 15; exit 4; }; done < "$D/modules/order.txt"; }
fw() { [ -d "$D/firmware" ] && { mkdir -p /lib/firmware; cp -r "$D"/firmware/* /lib/firmware/; }; }
'''
V=ROOT/'firmware/extracted/vendor/firmware'
gpufw=[(V/n,'qcom/hisense/a6l/'+n) for n in ['a512_zap.mdt','a512_zap.b00','a512_zap.b01','a512_zap.b02']]+[(V/n,'qcom/'+n) for n in ['a530_pm4.fw','a530_pfp.fw','a530v3_gpmu.fw2']]
area('gpu',['msm'],'''fw; load; sleep 5
ls -l /sys/class/drm/ | grep -i "card\\|render"
klog | grep -i "msm\\|adreno\\|zap\\|gpu\\|smmu\\|fault" | tail -n 40
if [ -e /sys/class/drm/renderD128 ]; then echo A6L_GPU_RENDER_NODE_PASS; cat /sys/kernel/debug/dri/*/gpu 2>/dev/null | head -n 12; else echo A6L_GPU_RENDER_NODE_MISSING; fi
klog | grep -qi "Oops\\|BUG:\\|Unhandled context fault" && echo A6L_GPU_KERNEL_ERRORS
''',gpufw)
area('eink-pmic',['tps65185'],'''load; sleep 2
klog | grep -i "tps65185\\|regulator.*epd\\|2-0068\\|i2c" | tail -n 15
for h in /sys/class/hwmon/hwmon*; do [ "$(cat $h/name 2>/dev/null)" = tps65185 ] && echo "A6L_EPD_PMIC_TEMP_mC=$(cat $h/temp1_input)"; done
ls /sys/bus/i2c/devices/ | tr "\\n" " "; echo
[ -e /sys/bus/i2c/drivers/tps65185 ] && ls /sys/bus/i2c/drivers/tps65185/ | grep -q 0068 && echo A6L_EPD_PMIC_BOUND_PASS || echo A6L_EPD_PMIC_NOT_BOUND
# Rails stay OFF: no consumer enables vposneg/vcom in this test.
''')
area('touch',['edt-ft5x06'],'''load; sleep 3
for e in /sys/class/input/event*; do echo "$e $(cat $e/device/name) $(cat $e/device/phys 2>/dev/null)"; done
klog | grep -i "edt_ft5x06\\|ft5x06\\|0038" | tail -n 10
n=$(grep -c ft5x06 /sys/class/input/event*/device/name); echo "A6L_TOUCH_DEVICES=$n (2 = front + rear)"
''')
area('front-als',['stk3310'],'''load; sleep 2
klog | grep -i "stk3310\\|0047\\|chip id" | tail -n 8
for d in /sys/bus/iio/devices/iio:device*; do [ "$(cat $d/name 2>/dev/null)" = stk3310 ] || continue; echo "A6L_ALS_RAW=$(cat $d/in_illuminance_raw 2>&1) A6L_PROX_RAW=$(cat $d/in_proximity_raw 2>&1)"; echo A6L_FRONT_ALS_PASS; done
''')
area('sensors-adsp',['qcom_pd_mapper','sns_smgr','qcom_smgr','qcom_smgr_accel','qcom_smgr_gyro','qcom_smgr_mag','qcom_smgr_prox'],'''# Requires the ADSP already RUNNING (run the adsp bundle with HOLD and no stop, or start it via remoteproc first).
[ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo A6L_HW_FAIL adsp not running; exit 5; }
load; sleep 6
klog | grep -i "smgr\\|sns\\|qrtr\\|pd.mapper\\|pdr" | tail -n 20
for d in /sys/bus/iio/devices/iio:device*; do echo "$d $(cat $d/name 2>/dev/null)"; done
''')
PF=ROOT/'firmware/extracted/peripheral-prep-20260917/firmware/qcom/hisense/a6l'
modemfw=[(p,'qcom/hisense/a6l/'+p.name) for p in sorted(PF.iterdir()) if p.is_file() and re.fullmatch(r'(modem\.(mdt|b\d\d)|mba\.mbn|wlanmdsp\.mbn|modem\w*\.jsn)',p.name)]
modemfw+=[(Path('/home/a6l/fw-dl/firmware-5.bin'),'ath10k/WCN3990/hw1.0/firmware-5.bin'),(Path('/home/a6l/fw-dl/board-2.bin'),'ath10k/WCN3990/hw1.0/board-2.bin')]
VB=Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l/vendor')
area('modem-wifi',['qcom_pd_mapper','qcom_q6v5_mss','ath10k_snoc'],'''# RF-CAPABLE: the modem boots its real firmware. Requires A6L_MODEM_APPROVED=1 set by the attended operator.
# Modem storage: RAM COPIES only (read-only dd of modemst1/modemst2/fsg/fsc); the live partitions are never opened for writing.
[ "${A6L_MODEM_APPROVED:-0}" = 1 ] || { echo A6L_HW_FAIL modem run not approved; exit 6; }
mkdir -p /tmp/rmtfs /tmp/tqftpserv
for pair in modemst1:modem_fs1 modemst2:modem_fs2 fsg:modem_fsg fsc:modem_fsc; do
    part=${pair%%:*}; file=${pair##*:}; dev=""
    for u in /sys/class/block/*/uevent; do grep -q "^PARTNAME=$part$" $u && dev=$(dirname $u); done
    [ -n "$dev" ] || { echo "A6L_HW_FAIL partition $part not found (load sdhci-msm first)"; exit 7; }
    mm=$(cat $dev/dev); mknod /dev/a6l-ro-$part b ${mm%%:*} ${mm##*:}; dd if=/dev/a6l-ro-$part of=/tmp/rmtfs/$file bs=1M 2>/dev/null; rm /dev/a6l-ro-$part
    echo "A6L_RMTFS_COPY $part $(sha256sum /tmp/rmtfs/$file | cut -c1-16)"
done
fw
insmod "$D/modules/qrtr.ko" 2>/dev/null; insmod "$D/modules/qrtr-smd.ko" 2>/dev/null
export LD_LIBRARY_PATH=$D/bin; chmod 755 $D/bin/*
$D/bin/rmtfs -o /tmp/rmtfs -v > /tmp/rmtfs.log 2>&1 &
$D/bin/tqftpserv > /tmp/tqftpserv.log 2>&1 &
sleep 1; load
i=0; while [ $i -lt 40 ]; do for r in /sys/class/remoteproc/remoteproc*; do case "$(cat $r/name)" in *4080000*|mss|modem) M=$r;; esac; done; [ "$(cat ${M:-/nonexistent}/state 2>/dev/null)" = running ] && break; sleep 1; i=$((i+1)); done
echo "A6L_MODEM_STATE=$(cat ${M:-/nonexistent}/state 2>/dev/null) after=${i}s"
sleep 15
$D/bin/qrtr-lookup 2>&1 | head -n 40
klog | grep -i "q6v5\\|mss\\|mba\\|rmtfs\\|ath10k\\|wlan\\|qmi\\|board" | tail -n 40
ls /sys/class/net; tail -n 5 /tmp/rmtfs.log /tmp/tqftpserv.log
''',modemfw,[VB/'bin/rmtfs',VB/'bin/tqftpserv',VB/'bin/qrtr-lookup',VB/'lib64/libqrtr.so'])
(OUT/'cpu-speed.sh').write_text('''#!/system/bin/sh
# Same script on stock Android (adb shell) and in the recovery: compares the fixed recovery CPU speed with stock.
for c in 0 7; do s=$(date +%s%N 2>/dev/null); taskset $(printf %x $((1<<c))) sh -c 'i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done'; e=$(date +%s%N); echo "A6L_CPU_LOOP cpu$c ms=$(( (e-s)/1000000 ))"; done
''')
manifest={str(p.relative_to(OUT)):sha(p.read_bytes()) for p in sorted(OUT.rglob('*')) if p.is_file()}
(OUT/'manifest.json').write_text(json.dumps({'requires_recovery':'recovery-v69-candidate-20260921','files':manifest,'phone_access':False},indent=2)+'\n')
print('V69_BUNDLES_PASS',len(manifest))
