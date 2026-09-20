"""Build the RAM payloads for the attended V68 session. Offline; run inside WSL.
  eink/  a6l_epd_nor_read + spidev.ko + run script       (read-only SPI NOR backup)
  adsp/  adsp_diag_r2.sh + 10 modules (V67 tree) + stock ADSP firmware, each with a TRUSTED SHA256SUMS
Trust roots: module hashes must appear in the V67 kernel archive's modules-SHA256SUMS; firmware hashes must
match research/claude-adsp/checks/adsp-mdt-loader-audit.json / the recorded adsp.mdt hash.
"""
import hashlib,json,re,shutil,tarfile
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
KERNEL=ROOT/'firmware/extracted/phone-kernel-v67-candidate-20260919'
OUT=ROOT/'firmware/extracted/v68-attended-bundle-20260920';OUT.mkdir(exist_ok=False)
sha=lambda b:hashlib.sha256(b).hexdigest()
trusted=(KERNEL/'modules-SHA256SUMS').read_text()
tar=tarfile.open(KERNEL/'modules.tar.gz');members={Path(m.name).name:m for m in tar.getmembers() if m.name.endswith('.ko')}
def module(name,dest):
    data=tar.extractfile(members[name]).read();assert sha(data) in trusted,name;(dest/name).write_bytes(data);return sha(data)
def sums(directory,names):(directory/'SHA256SUMS').write_text(''.join(f'{sha((directory/n).read_bytes())}  {n}\n' for n in names))
# ---- e-ink -------------------------------------------------------------------------------
E=OUT/'eink';E.mkdir()
tool=Path('/home/a6l/android/a6l-lineage24/out/target/product/a6l/system/bin/a6l_epd_nor_read');shutil.copyfile(tool,E/'a6l_epd_nor_read')
module('spidev.ko',E)
(E/'run-eink-read.sh').write_text('''#!/system/bin/sh
# Attended only. Read-only SPI NOR backup under the V68 diagnostic kernel. NOT YET RUN.
set -u
D=${D:-/tmp/eink}
[ "$(tr -d '\\0' < /proc/device-tree/chosen/hisense,a6l-eink-flash 2>/dev/null)" = read1 ] || { echo A6L_EINK_READ_FAIL wrong image; exit 2; }
( cd "$D" && sha256sum -c SHA256SUMS ) || { echo A6L_EINK_READ_FAIL payload hash; exit 3; }
S=""; for d in /sys/bus/spi/devices/spi*; do [ -e "$d/of_node" ] && case "$(readlink -f $d/of_node)" in */spi@c1b8000/epd-flash@0) S=$(basename $d);; esac; done
[ -n "$S" ] || { echo A6L_EINK_READ_FAIL no spi device: controller did not probe; dmesg | grep -i "spi\\|qup" | tail -n 20; exit 4; }
lsmod 2>/dev/null | grep -q '^spidev ' || insmod "$D/spidev.ko" || { echo A6L_EINK_READ_FAIL insmod spidev; exit 5; }
echo spidev > /sys/bus/spi/devices/$S/driver_override
[ -e /sys/bus/spi/devices/$S/driver ] || echo $S > /sys/bus/spi/drivers/spidev/bind || { echo A6L_EINK_READ_FAIL bind; exit 6; }
N=/dev/spidev${S#spi}; [ -c "$N" ] || { M=$(cat /sys/bus/spi/devices/$S/spidev/*/dev 2>/dev/null); [ -n "$M" ] && mknod "$N" c ${M%%:*} ${M##*:}; }
[ -c "$N" ] || { echo A6L_EINK_READ_FAIL no spidev node; exit 7; }
G=""; for c in /sys/bus/gpio/devices/gpiochip*; do case "$(readlink -f $c/of_node 2>/dev/null)" in */pinctrl@*) [ "$(cat /sys/class/gpio/$(basename $c)/ngpio 2>/dev/null || echo 0)" -ge 100 ] 2>/dev/null && G=/dev/$(basename $c);; esac; done
[ -n "$G" ] || G=/dev/gpiochip0
[ -c "$G" ] || { M=$(cat /sys/bus/gpio/devices/$(basename $G)/dev); mknod "$G" c ${M%%:*} ${M##*:}; }
chmod 0755 "$D/a6l_epd_nor_read"
"$D/a6l_epd_nor_read" "$N" "$G" "$D/epd-nor.bin"; rc=$?
[ $rc -eq 0 ] && sha256sum "$D/epd-nor.bin"
echo A6L_EINK_READ_EXIT=$rc; exit $rc
''')
sums(E,['a6l_epd_nor_read','spidev.ko','run-eink-read.sh'])
# ---- ADSP --------------------------------------------------------------------------------
A=OUT/'adsp';(A/'modules').mkdir(parents=True);(A/'firmware').mkdir()
order=[l.split()[0] for l in (ROOT/'research/claude-adsp/tools/adsp-diag-modules.txt').read_text().splitlines() if l and not l.startswith('#')]
assert len(order)==10,order
for n in order:module(n,A/'modules')
(A/'modules/order.txt').write_text('# insmod order from research/claude-adsp; binaries rebuilt from the V67 kernel tree\n'+'\n'.join(order)+'\n')
sums(A/'modules',order)
fwsrc=next(p for p in (ROOT/'firmware/extracted/peripheral-prep-20260917/firmware').rglob('adsp.mdt')).parent
audit=json.dumps(json.loads((ROOT/'research/claude-adsp/checks/adsp-mdt-loader-audit.json').read_text()))
fw=sorted(p.name for p in fwsrc.iterdir() if re.fullmatch(r'adsp\.(mdt|b\d\d)',p.name));assert len(fw)==21,fw
for n in fw:
    data=(fwsrc/n).read_bytes();(A/'firmware'/n).write_bytes(data)
    assert sha(data) in audit or n=='adsp.mdt',('hash not in the offline loader audit',n)
assert sha((A/'firmware/adsp.mdt').read_bytes())=='9a5105caf136434128f0893e4993fe77b33a00a3df3f42286559cbaf35822800'
sums(A/'firmware',fw)
shutil.copyfile(ROOT/'device/hisense/a6l/diagnostic/adsp_diag_r2.sh',A/'adsp_diag_r2.sh')
manifest={str(p.relative_to(OUT)).replace('\\','/'):sha(p.read_bytes()) for p in sorted(OUT.rglob('*')) if p.is_file()}
(OUT/'manifest.json').write_text(json.dumps({'kernel_archive':KERNEL.name,'requires_recovery':'recovery-v68-candidate-20260920','files':manifest,'phone_access':False},indent=2)+'\n')
print('V68_BUNDLES_PASS files',len(manifest))
