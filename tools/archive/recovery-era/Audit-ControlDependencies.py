"""Record evidence and unresolved activation prerequisites, without phone writes."""
import hashlib,json,re,subprocess
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'firmware/extracted/controls-radio-prep-20260917'
vendor=ROOT/'firmware/extracted/vendor'
gnss=[]
for rel in ['bin/hw/vendor.qti.gnss@2.0-service','lib64/libloc_api_v02.so','lib64/libqmi_cci.so']:
    p=vendor/rel
    text=subprocess.check_output(['readelf','-d',str(p)],text=True)
    needed=re.findall(r'Shared library: \[(.*?)\]',text)
    gnss.append({'file':rel,'sha256':hashlib.sha256(p.read_bytes()).hexdigest(),'needed':needed})
report={
 'phone_changed':False,
 'gnss':{'stock_elf_dependencies':gnss,'transport':'Stock libloc_api_v02 links libqmi_cci and libqmi_common_so; future QRTR/QMI LOC discovery required','prerequisites':['MSS/modem firmware boot and stable QRTR transport','RMTFS and modem service dependencies','LOC service discovery, then short explicitly requested satellite/fix test','Android GNSS HAL/SELinux integration'],'not_required_for_initial_proof':'LTE IP data path need not work before testing standalone satellite reception','not_done':'No location queried, assistance data erased, GNSS session started or stock daemon transplanted'},
 'bluetooth':{'stock_uart':'/sys/class/tty/ttyHS0/device -> c1af000.uart','pins':[16,17,18,19],'driver':'hci_uart/QCA WCN3990','known_supply_mapping':{'vddxo':'PM660 L9, stock BT core 1800000..1900000 uV','vddrf':'PM660 L6, stock BT PA 1304000..1370000 uV','vddch0':'PM660 L19, stock BT LDO 3312000..3400000 uV'},'unresolved':'vddio: stock chip-pwd is PM660L BOB pin1 at 3.6V; must not equate that to upstream 1.8V VDD_IO. Related SDM660 boards use different rails, some explicitly marked TODO. Do not borrow their wiring.','firmware':'Six original crbtfw/crnv 11/20/21 images structurally checked and lowercased; let hardware ROM query choose variant','next':['Trace VDD_IO from stock power implementation/firmware and board evidence','Compile reviewed UART/supply DT with correct regulator load permissions','Controller version and firmware initialization first; discovery/pairing later','Android Bluetooth HAL integration and audio transports remain separate']},
 'audio':{'codec':'PM660L analog SID3 f000 + digital 152c0000, stock MCLK 9600000 Hz','stock_supplies':{'cp_pa':'PM660 S4; downstream CP 1.9..2.05V, PA 2.04V','micbias':'PM660L L7; downstream request 3.088V'},'route_reference':'stock-audio-routes.json preserves order and expands named subpaths for eight XML variants','provisional_channels':{'earpiece':'RX1 -> EAR','headphones':'RX1/RX2 -> HPHL/HPHR','handset_mic':'ADC1 -> DEC1','headset_mic':'ADC2 -> DEC1'},'limits':['Active stock mixer XML not conclusively identified; /proc/asound and card sysfs inaccessible to stock shell','Stock gains are not safe generic upstream defaults; muted/low-gain first test','ADSP, APR, clock, analog/digital codec and DAI-link bring-up required before routing','Headset presence/buttons and mic bias/polarity need physical tests','TFA9894 speaker path remains separate; no speaker-protection bypass']},
}
(OUT/'dependency-audit.json').write_text(json.dumps(report,indent=2)+'\n')
print('CONTROL_DEPENDENCIES_ARCHIVED',len(gnss),'GNSS ELF dependency sets')
