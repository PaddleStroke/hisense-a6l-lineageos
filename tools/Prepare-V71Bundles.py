"""RAM payloads for the attended V71 session: ALL hardware areas in one bundle (V69 + V70 areas, with the 21 Sep fixes). Offline; run inside WSL. Every area = directory with
modules (dependency-ordered order.txt), trusted SHA256SUMS and one bounded script. Nothing here runs by itself."""
import hashlib,json,re,shutil,subprocess,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/v71-attended-bundle-20260922';OUT.mkdir(exist_ok=False)
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
# ATTENDED ONLY, V71 diagnostic recovery. usage: D=/tmp/<area> sh run.sh     (PATH must include /tmp/bin toybox links)
set -u
# msm.ko is ALWAYS loaded with separate_gpu_kms=1 (21 Sep: without it the GPU waits for the whole display chain). Never rmmod msm (oops in adreno_remove).
D=${D:-$(dirname "$0")}
[ "$(tr -d '\\0' < /proc/device-tree/chosen/hisense,a6l-controls 2>/dev/null)" = v71 ] || { echo A6L_HW_FAIL wrong image; exit 2; }
( cd "$D" && sha256sum -c SHA256SUMS > /dev/null ) || { echo A6L_HW_FAIL payload hash; exit 3; }
MARK="A6L_HW_$$"; echo "$MARK" > /dev/kmsg
klog() { dmesg | sed -n "/$MARK/,\\$p"; }
load() { while read -r ko; do [ -n "$ko" ] || continue; n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules && continue; p=""; [ "$n" = msm ] && p="separate_gpu_kms=1 ${A6L_MSM_PARAMS:-}"; [ "$n" = panel_ft8719_tianma_1080x2340 ] && p="${A6L_PANEL_PARAMS:-}"; [ "$n" = panel_a6l_epd_dsi ] && p="${A6L_EPD_PARAMS:-}"; insmod "$D/modules/$ko" $p || { echo "A6L_HW_FAIL insmod $ko"; klog | tail -n 15; exit 4; }; done < "$D/modules/order.txt"; }
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
area('eink-pmic',['tps65185'],'''# V71: gpio42/gpio56 are DT regulators now; no a6l_gpio_hold needed.
cat /sys/kernel/debug/regulator/regulator_summary 2>/dev/null | grep -i "epd" | head -n 6
load; sleep 2
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
SNSREG=ROOT/'firmware/extracted/sensors-registry-20260922/sns.reg'   # from the 14 Sep persist backup (/sensors/sns.reg); device calibration, not in git
area('sensors-adsp',['qcom_sns_reg','qcom_pd_mapper','sns_smgr','qcom_smgr','qcom_smgr_accel','qcom_smgr_gyro','qcom_smgr_mag','qcom_smgr_prox'],'''# 21 Sep finding: the ADSP never offered SMGR (QMI 256) because nobody served the Sensor Registry. The tree has a kernel
# registry server (qcom_sns_reg.ko, reads /lib/firmware/qcom/sensors/sns.reg = this phone's own persist copy, RAM only).
# Order: PHASE=pre BEFORE starting the ADSP (registry ready when the DSP asks), then the adsp bundle, then PHASE=post.
fw
if [ "${PHASE:-post}" = pre ]; then
    [ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] && echo "A6L_NOTE adsp already running: registry arrives late"
    while read -r ko; do n=$(echo "${ko%.ko}" | tr - _); grep -q "^$n " /proc/modules || insmod "$D/modules/$ko" || { echo "A6L_HW_FAIL insmod $ko"; exit 4; }; [ "$n" = qcom_sns_reg ] && break; done < "$D/modules/order.txt"
    echo A6L_SNS_REG_LOADED; exit 0
fi
[ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo A6L_HW_FAIL adsp not running; exit 5; }
load; sleep 8
klog | grep -i "smgr\\|sns\\|qrtr\\|pd.mapper\\|pdr\\|registry" | tail -n 25
n=0; for d in /sys/bus/iio/devices/iio:device*; do nm=$(cat $d/name 2>/dev/null); echo "$d $nm"; case "$nm" in *smgr*|*accel*|*gyro*|*mag*|*prox*) n=$((n+1));; esac; done
[ $n -gt 0 ] && echo "A6L_SMGR_SENSORS_PASS count=$n" || echo A6L_SMGR_SENSORS_MISSING
''',[(SNSREG,'qcom/sensors/sns.reg')])
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
DIAG=Path('/home/a6l/display-diag')
area('display',['panel-ft8719-tianma-1080x2340','tps65185','panel-a6l-epd-dsi','msm'],'''# Native display takeover EXPERIMENT (21 Sep: connectors came up, LCD stayed backlit black, RCG update WARNs).
# Options (env): A6L_DISPLAY_QUIESCE=1  gate the bootloader-left MDSS branch clocks before msm loads (RCG roots go off)
#                A6L_DISPLAY_PATTERN=1  show the modetest SMPTE pattern on the LCD for 15 s
#                A6L_PANEL_PARAMS="skip_init=1"  keep the bootloader panel state (no reset, no DCS init)
#                A6L_MSM_PARAMS="prefer_mdp5=0|1 ..." extra msm.ko parameters
# Everything is captured under /tmp/display-diag/ for pulling. The LCD may stay dark for this boot; ADB stays up.
O=/tmp/display-diag; mkdir -p $O; cp "$D"/bin/* /tmp/; chmod 755 /tmp/a6l_mmio /tmp/modetest /tmp/mmcc-diag.sh
sh /tmp/mmcc-diag.sh pre > $O/mmcc-pre.txt 2>&1; cat $O/mmcc-pre.txt | head -n 12
if [ "${A6L_DISPLAY_QUIESCE:-0}" = 1 ] && ! grep -q "^msm " /proc/modules; then
    # CBCR bit0 = enable. Order: interface clocks first, then MDP core. AHB/AXI stay on. MMCC window only.
    for r in 0c8c2314 0c8c233c 0c8c2374 0c8c2344 0c8c2328 0c8c231c; do v=$(/tmp/a6l_mmio r $r | cut -d" " -f2); nv=${v%?}$(printf %x $(( 0x${v#???????} & 14 )));   # clear bit0 without 32-bit shell arithmetic on the full word
        A6L_MMIO_WRITE=1 /tmp/a6l_mmio w $r $nv; done
    sh /tmp/mmcc-diag.sh quiesced > $O/mmcc-quiesced.txt 2>&1
fi
fw; load; sleep 12
sh /tmp/mmcc-diag.sh post > $O/mmcc-post.txt 2>&1
ls /sys/class/drm/ | tr "\\n" " "; echo
for c in /sys/class/drm/card*-D*; do echo "$c status=$(cat $c/status) enabled=$(cat $c/enabled) modes=$(head -n 1 $c/modes)"; done
for f in /sys/kernel/debug/dri/*/state; do echo "== $f"; cat $f; done > $O/drm-state.txt 2>&1
/tmp/a6l_mmio r 0c994000 80 > $O/dsi0-ctrl.txt 2>&1      # DSI0 host: 0x004 CTRL, 0x008 STATUS, 0x00c FIFO_STATUS, 0x0b4 lane status
/tmp/a6l_mmio r 0c96b800 16 > $O/intf1.txt 2>&1          # INTF1 timing engine (0x000 TIMING_ENGINE_EN)
mkdir -p /dev/dri; for c in /sys/class/drm/card[0-9] /sys/class/drm/renderD*; do n=${c##*/}; d=$(cat $c/dev); [ -e /dev/dri/$n ] || mknod /dev/dri/$n c ${d%%:*} ${d##*:}; done   # the recovery has no /dev/dri
/tmp/modetest -c -e -p > $O/modetest.txt 2>&1
if [ "${A6L_DISPLAY_PATTERN:-0}" = 1 ]; then
    id=$(grep "DSI-1" $O/modetest.txt | head -n 1 | cut -f1); echo "A6L_PATTERN connector=$id"
    [ -n "$id" ] && { sleep 15 | /tmp/modetest -s $id:1080x2340 > $O/pattern.txt 2>&1; tail -n 3 $O/pattern.txt; }
    sh /tmp/mmcc-diag.sh pattern > $O/mmcc-pattern.txt 2>&1
fi
dmesg > $O/dmesg.txt; cat /sys/kernel/debug/clk/clk_summary > $O/clk_summary.txt
grep -i "rcg didn\\|vblank\\|timeout\\|underrun\\|dsi.*err\\|fault\\|ft8719\\|Failed" $O/dmesg.txt | tail -n 25
diff $O/mmcc-pre.txt $O/mmcc-post.txt | head -n 40
ls /sys/class/drm/ | grep -q "DSI-1" && echo A6L_NATIVE_DISPLAY_CONNECTOR_PASS || echo A6L_NATIVE_DISPLAY_CONNECTOR_MISSING
''',gpufw,[DIAG/'modetest',DIAG/'a6l_mmio',ROOT/'device/hisense/a6l/diagnostic/mmcc-diag.sh'])
area('eink-dsi',['tps65185','panel-a6l-epd-dsi'],'''# Requires the display area loaded first (msm.ko owns DSI1). Rails are NOT enabled here; this only checks that the
# bridge and DPI panel bind and a 384x725 connector appears. Driving the panel is a separate, later step.
load; sleep 5
for c in /sys/class/drm/card*-DSI-* /sys/class/drm/card*-DPI-*; do [ -e $c ] && echo "$c status=$(cat $c/status) modes=$(head -n 1 $c/modes)"; done
klog | grep -i "tc358762\\|tps65185\\|panel-dpi\\|dsi@c996000\\|bridge" | tail -n 25
cat /sys/class/drm/card*-*/modes 2>/dev/null | grep -q 384x725 && echo A6L_EINK_DSI_MODE_PASS || echo A6L_EINK_DSI_MODE_MISSING
''')
EPD=sorted((ROOT/'firmware/extracted').glob('eink-swtcon-*-r*/update*-t*.a6lepd'))[-2:]   # newest dump run: update1 (clear) + update2 (grey bars)
assert len(EPD)==2 and EPD[0].parent==EPD[1].parent,EPD
area('eink-draw',['tps65185','panel-a6l-epd-dsi'],'''# E-INK DRAW EXPERIMENT (state after 21 Sep: rails OK, frames flip at 85 Hz, bridge does not answer on I2C, no image yet).
# Requires the display area loaded in THIS boot. Env:
#   A6L_EPD_HV=1        rails + VCOM on during playback (attended, Pierre watches the rear screen)
#   A6L_EPD_XON=1|0     hold TLMM gpio61 (panel XON) high/low during playback (default: untouched)
#   A6L_EPD_ARGS="--xbgr --invert"   player options (byte order / polarity experiments)
#   A6L_EPD_I2C=id|dump|init762|init767   talk to the bridge over I2C like stock, with the rails ON, before playback
mk() { [ -e "$2" ] || { d=$(cat "$1"); mknod "$2" c ${d%%:*} ${d##*:}; }; }
mkdir -p /dev/dri; for c in /sys/class/drm/card[0-9]; do mk $c/dev /dev/dri/${c##*/}; done
mk /sys/class/i2c-dev/i2c-0/dev /dev/i2c-0; mk /sys/bus/gpio/devices/gpiochip0/dev /dev/gpiochip0
cp "$D"/bin/* /tmp/; chmod 755 /tmp/a6l_epd_play /tmp/a6l_tps65185_step /tmp/a6l_dsi2dpi_init /tmp/a6l_gpio_hold
E="$D/firmware/epd"; P=/sys/module/panel_a6l_epd_dsi/parameters/hv
/tmp/a6l_epd_play --dry $E/*.a6lepd || exit 7
[ -e $P ] || { echo A6L_HW_FAIL panel-a6l-epd-dsi not loaded; exit 5; }
/tmp/a6l_tps65185_step --vcom /dev/i2c-0 2400 || exit 8          # VCOM register resets to 1.25 V on every wake
cycle() { /tmp/a6l_epd_play --lead 1 --tail 1 $E/update2-t25.a6lepd > /dev/null; sleep 1; }   # rails only switch in panel prepare
[ "${A6L_EPD_HV:-0}" = 1 ] && { echo 1 > $P; cycle; klog | grep -E "rails O|power good" | tail -n 1; }
[ -n "${A6L_EPD_XON:-}" ] && { /tmp/a6l_gpio_hold /dev/gpiochip0 20 61=$A6L_EPD_XON > /dev/null 2>&1 & sleep 1; }
[ -n "${A6L_EPD_I2C:-}" ] && /tmp/a6l_dsi2dpi_init /dev/i2c-0 $A6L_EPD_I2C
/tmp/a6l_epd_play ${A6L_EPD_ARGS:-} $E/update1-t25.a6lepd $E/update2-t25.a6lepd | tail -n 1; rc=$?
echo 0 > $P; cycle; klog | grep -E "rails O" | tail -n 1
exit $rc
''',[(f,'epd/'+f.name) for f in EPD],[DIAG/'a6l_epd_play',DIAG/'a6l_tps65185_step',DIAG/'a6l_dsi2dpi_init',Path('/home/a6l/a6l_gpio_hold')])
area('audio',['qcom_pd_mapper','apr','q6core','q6afe','q6afe-dai','q6afe-clocks','q6adm','q6asm','q6asm-dai','q6routing','pinctrl-lpass-lpi','pinctrl-sdm660-lpass-lpi','snd-soc-msm8916-analog','snd-soc-msm8916-digital','snd-soc-sm8250'],'''# Requires the ADSP RUNNING (adsp bundle, left running). Registers the sound card only: no playback, no capture.
[ "$(cat /sys/class/remoteproc/remoteproc0/state 2>/dev/null)" = running ] || { echo A6L_HW_FAIL adsp not running; exit 5; }
load; sleep 8
cat /proc/asound/cards 2>&1; cat /proc/asound/pcm 2>&1 | head
klog | grep -v "Failed to add route" | grep -i "q6\\|apr\\|snd\\|asoc\\|wcd\\|codec\\|sndcard" | tail -n 30; echo "route failures: $(klog | grep -c 'Failed to add route')"
grep -q "A6L\\|Hisense" /proc/asound/cards 2>/dev/null && echo A6L_SOUND_CARD_PASS || echo A6L_SOUND_CARD_MISSING
''')
BT=ROOT/'firmware/extracted/controls-radio-prep-20260917/firmware/qca'
area('bluetooth',['hci_uart'],'''# RF-capable: requires A6L_BT_APPROVED=1. Initialises the WCN3990 BT core and reads its version; no scan, no pairing.
[ "${A6L_BT_APPROVED:-0}" = 1 ] || { echo A6L_HW_FAIL bluetooth run not approved; exit 6; }
fw; load; sleep 10
ls /sys/class/bluetooth/ 2>&1
klog | grep -i "bluetooth\\|hci\\|qca\\|wcn3990\\|serial@c1af000" | tail -n 25
[ -e /sys/class/bluetooth/hci0 ] && echo A6L_BT_HCI0_PASS || echo A6L_BT_HCI0_MISSING
''',[(p,'qca/'+p.name) for p in sorted(BT.iterdir()) if p.is_file()])
(OUT/'cpu-speed.sh').write_text('''#!/system/bin/sh
# Same script on stock Android (adb shell) and in the recovery: compares the fixed recovery CPU speed with stock.
for c in 0 7; do s=$(date +%s%N 2>/dev/null); taskset $(printf %x $((1<<c))) sh -c 'i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done'; e=$(date +%s%N); echo "A6L_CPU_LOOP cpu$c ms=$(( (e-s)/1000000 ))"; done
''')
manifest={str(p.relative_to(OUT)):sha(p.read_bytes()) for p in sorted(OUT.rglob('*')) if p.is_file()}
(OUT/'manifest.json').write_text(json.dumps({'requires_recovery':'recovery-v71-candidate-20260921','files':manifest,'phone_access':False},indent=2)+'\n')
print('V71_BUNDLES_PASS',len(manifest))
