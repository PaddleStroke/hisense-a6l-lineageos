"""WSL-only: add bounded, opt-in traces to the actual DWC3 event path."""
from pathlib import Path
import subprocess, hashlib, difflib, json

ROOT = Path('/mnt/c/Users/Pierre/Desktop/A6L')
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
REL = 'drivers/usb/dwc3/gadget.c'
p = KERNEL / REL
old = p.read_text()
head = subprocess.run(['git', '-C', str(KERNEL), 'show', 'HEAD:' + REL], capture_output=True, text=True, check=True).stdout
assert old == head, 'Do not overwrite previous DWC3 changes'
s = old
pos = s.index('#include ')
s = s[:pos] + '#include <linux/init.h>\n#include <linux/kstrtox.h>\n#include <linux/of.h>\n#include <linux/atomic.h>\n' + s[pos:]
pos = s.index('static ')
s = s[:pos] + '''/* Temporary bounded A6L diagnostic; no change to event handling. */
static bool a6l_usb_trace;
static bool a6l_usb_trace_active;
static atomic_t a6l_irq_trace_count = ATOMIC_INIT(0);
static atomic_t a6l_thread_trace_count = ATOMIC_INIT(0);
static atomic_t a6l_event_trace_count = ATOMIC_INIT(0);
static int __init a6l_usb_trace_setup(char *value)
{
	return kstrtobool(value, &a6l_usb_trace);
}
early_param("a6l_usb_trace", a6l_usb_trace_setup);

''' + s[pos:]

def replace(before, after):
    global s
    assert s.count(before) == 1, before
    s = s.replace(before, after, 1)

replace('int dwc3_gadget_init(struct dwc3 *dwc)\n{', 'int dwc3_gadget_init(struct dwc3 *dwc)\n{')
replace('\tirq = dwc3_gadget_get_irq(dwc);', '''	a6l_usb_trace_active = a6l_usb_trace &&
		of_machine_is_compatible("hisense,hlte730t");
	if (a6l_usb_trace_active)
		dev_info(dwc->dev, "A6L_USB_EVENT_TRACE armed: first 8 IRQs/threads and 16 events\\n");
	irq = dwc3_gadget_get_irq(dwc);''')
replace('''static irqreturn_t dwc3_interrupt(int irq, void *_evt)
{
	struct dwc3_event_buffer	*evt = _evt;

	return dwc3_check_event_buf(evt);
}''', '''static irqreturn_t dwc3_interrupt(int irq, void *_evt)
{
	struct dwc3_event_buffer	*evt = _evt;
	irqreturn_t ret;
	bool trace = a6l_usb_trace_active &&
		atomic_inc_return(&a6l_irq_trace_count) <= 8;

	if (trace)
		dev_info(evt->dwc->dev, "A6L_USB_IRQ_BEGIN irq=%d\\n", irq);
	ret = dwc3_check_event_buf(evt);
	if (trace)
		dev_info(evt->dwc->dev, "A6L_USB_IRQ_END ret=%d count=%u pos=%u\\n",
			 ret, evt->count, evt->lpos);
	return ret;
}''')
replace('''	u32 amount;
	u32 count;

	if (pm_runtime_suspended(dwc->dev))''', '''	u32 amount;
	u32 count;
	bool trace = a6l_usb_trace_active && atomic_read(&a6l_irq_trace_count) <= 8;

	if (trace)
		dev_info(dwc->dev, "A6L_USB_EVENT_CHECK suspended=%d flags=%x\\n",
			 pm_runtime_suspended(dwc->dev), evt->flags);
	if (pm_runtime_suspended(dwc->dev))''')
replace('\tcount = dwc3_readl(dwc, DWC3_GEVNTCOUNT(0));', '''	if (trace)
		dev_info(dwc->dev, "A6L_USB_EVENT_COUNT_READ_BEGIN\\n");
	count = dwc3_readl(dwc, DWC3_GEVNTCOUNT(0));
	if (trace)
		dev_info(dwc->dev, "A6L_USB_EVENT_COUNT_READ_END raw=%08x len=%u pos=%u\\n",
			 count, evt->length, evt->lpos);''')
replace('\tamount = min(count, evt->length - evt->lpos);', '''	if (trace)
		dev_info(dwc->dev, "A6L_USB_EVENT_COPY_BEGIN count=%u\\n", count);
	amount = min(count, evt->length - evt->lpos);''')
replace('\tdwc3_writel(dwc, DWC3_GEVNTCOUNT(0), count);', '''	if (trace)
		dev_info(dwc->dev, "A6L_USB_EVENT_COPY_END\\n");
	dwc3_writel(dwc, DWC3_GEVNTCOUNT(0), count);''')
replace('''		union dwc3_event event;

		event.raw = *(u32 *) (evt->cache + evt->lpos);

		dwc3_process_event_entry(dwc, &event);''', '''		union dwc3_event event;
		bool trace = a6l_usb_trace_active &&
			atomic_inc_return(&a6l_event_trace_count) <= 16;

		event.raw = *(u32 *) (evt->cache + evt->lpos);
		if (trace)
			dev_info(dwc->dev, "A6L_USB_EVENT_BEGIN raw=%08x pos=%u left=%d\\n",
				 event.raw, evt->lpos, left);
		dwc3_process_event_entry(dwc, &event);
		if (trace)
			dev_info(dwc->dev, "A6L_USB_EVENT_END raw=%08x\\n", event.raw);''')
replace('''	irqreturn_t ret = IRQ_NONE;

	local_bh_disable();''', '''	irqreturn_t ret = IRQ_NONE;
	bool trace = a6l_usb_trace_active &&
		atomic_inc_return(&a6l_thread_trace_count) <= 8;

	if (trace)
		dev_info(dwc->dev, "A6L_USB_THREAD_BEGIN irq=%d\\n", irq);
	local_bh_disable();''')
replace('''	local_bh_enable();

	return ret;''', '''	local_bh_enable();
	if (trace)
		dev_info(dwc->dev, "A6L_USB_THREAD_END ret=%d\\n", ret);

	return ret;''')
patch = ''.join(difflib.unified_diff(old.splitlines(True), s.splitlines(True), fromfile='a/'+REL, tofile='b/'+REL))
out = ROOT/'device/hisense/a6l/kernel/a6l-usb-event-trace.patch'
assert not out.exists()
out.write_text(patch)
p.write_text(s)
(ROOT/'logs/usb-event-trace-patch.json').write_text(json.dumps({'source':REL,'before_sha256':hashlib.sha256(old.encode()).hexdigest(),'after_sha256':hashlib.sha256(s.encode()).hexdigest(),'scope':'Bounded printk around existing event path; A6L and boot flag gated; no extra MMIO reads or hardware parameter changes'},indent=2)+'\n')
print('DWC3 bounded event trace patch applied')
