"""Apply the bounded A6L diagnostic-only delayed USB connection patch in WSL."""
import difflib
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
REL = 'drivers/usb/gadget/udc/core.c'
p = KERNEL / REL
old = p.read_text()
head = subprocess.run(['git', '-C', str(KERNEL), 'show', 'HEAD:' + REL],
                      capture_output=True, text=True, check=True).stdout
assert old == head, 'Refuse to overwrite an existing UDC-core modification'
s = old.replace('#include <linux/workqueue.h>', '#include <linux/workqueue.h>\n#include <linux/init.h>\n#include <linux/kstrtox.h>\n#include <linux/of.h>', 1)
s = s.replace('static DEFINE_IDA(gadget_id_numbers);', '''/* Temporary A6L RAM diagnostic, opt-in and board gated. */
static bool a6l_manual_usb;
static int __init a6l_manual_usb_setup(char *value)
{
	return kstrtobool(value, &a6l_manual_usb);
}
early_param("a6l_manual_usb", a6l_manual_usb_setup);

static DEFINE_IDA(gadget_id_numbers);''', 1)
s = s.replace('bool\t\t\t\tallow_connect;', 'bool\t\t\t\tallow_connect;\n\tbool\t\t\t\ta6l_hold_connect;', 1)
assert s != old and s.count('bool\t\t\t\ta6l_hold_connect;') == 1
anchor = '\tif (gadget->connected)\n\t\tgoto out;'
assert s.count(anchor) == 1
s = s.replace(anchor, '''	if (gadget->udc->a6l_hold_connect) {
		dev_info(&gadget->udc->dev, "A6L_USB_CONNECT_HELD: driver bound, pullup not called\\n");
		goto out;
	}

''' + anchor, 1)
anchor = '\tret = driver->bind(udc->gadget, driver);'
assert s.count(anchor) == 1
s = s.replace(anchor, '''	udc->a6l_hold_connect = a6l_manual_usb &&
		of_machine_is_compatible("hisense,hlte730t");
	if (udc->a6l_hold_connect)
		dev_info(&udc->dev, "A6L_USB_MANUAL_CONNECT armed\\n");

''' + anchor, 1)
old_soft = '''		usb_gadget_udc_start_locked(udc);
		usb_gadget_connect_locked(udc->gadget);
		mutex_unlock(&udc->connect_lock);'''
assert s.count(old_soft) == 1
s = s.replace(old_soft, '''		if (udc->a6l_hold_connect) {
			dev_info(&udc->dev, "A6L_USB_CONNECT_RELEASE_BEGIN\\n");
			/* Binding already started the driver and installed its IRQ. */
			udc->a6l_hold_connect = false;
			ret = usb_gadget_connect_locked(udc->gadget);
			dev_info(&udc->dev, "A6L_USB_CONNECT_RELEASE_END result=%zd\\n", ret);
			mutex_unlock(&udc->connect_lock);
			if (ret)
				goto out;
		} else {
			usb_gadget_udc_start_locked(udc);
			usb_gadget_connect_locked(udc->gadget);
			mutex_unlock(&udc->connect_lock);
		}''', 1)
patch = ''.join(difflib.unified_diff(old.splitlines(True), s.splitlines(True),
                                   fromfile='a/' + REL, tofile='b/' + REL))
out = ROOT / 'device/hisense/a6l/kernel/a6l-manual-usb-connect.patch'
assert not out.exists(), 'Do not overwrite existing patch'
out.write_text(patch)
p.write_text(s)
report = {'source': REL, 'before_sha256': hashlib.sha256(old.encode()).hexdigest(),
          'after_sha256': hashlib.sha256(s.encode()).hexdigest(),
          'activation': 'a6l_manual_usb=1 AND machine compatible hisense,hlte730t',
          'scope': 'Hold USB pullup until explicit sysfs soft_connect connect; no voltage or DT changes'}
(ROOT / 'logs/manual-usb-kernel-patch.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report, indent=2))
