#!/usr/bin/env python3
"""Exercise the pinned regulator core's actual voltage validation with inert I/O."""
import hashlib
import json
from pathlib import Path
import re
import subprocess
from a6l_fdt import cells, read_fdt

ROOT = Path(__file__).resolve().parents[1]
KERNEL = Path('/home/a6l/kernel/a6l-mainline')
OUT = ROOT / 'firmware/extracted/regulator-constraints-20260916'
WORK = Path('/home/a6l/kernel/test-regulator-constraints-20260916')
assert subprocess.check_output(['git', '-C', str(KERNEL), 'rev-parse', 'HEAD']).decode().strip() == 'e47d622cb6d2440a9eacdc8bb2df32c037bec7b8'
for name in ('drivers/regulator/core.c', 'drivers/regulator/qcom_smd-regulator.c'):
    assert not subprocess.check_output(['git', '-C', str(KERNEL), 'diff', 'HEAD', '--', name])
core = (KERNEL / 'drivers/regulator/core.c').read_text()
start = core.index('static int machine_constraints_voltage(')
end = core.index('\nstatic int machine_constraints_current(', start)
function = core[start:end]
driver = (KERNEL / 'drivers/regulator/qcom_smd-regulator.c').read_text()
dt = read_fdt((ROOT / 'firmware/extracted/recovery-probe-visible-userspace-20260915/base.dtb').read_bytes())
prefix = '/remoteproc/glink-edge/rpm-requests/'
cases = []
for group, table in [('regulators-0', 'rpm_pm660l_regulators'), ('regulators-1', 'rpm_pm660_regulators')]:
    table_body = re.search(r'static const struct rpm_regulator_data '+table+r'\[\] = \{(.*?)\n\};', driver, re.S)[1]
    for path, props in dt.items():
        if not path.startswith(prefix+group+'/') or 'regulator-min-microvolt' not in props:
            continue
        name = path.rsplit('/', 1)[1]
        descriptor = re.search(r'\{\s*"'+name+r'"\s*,[^,]+,\s*\d+\s*,\s*&([a-z0-9_]+)', table_body)[1]
        body = re.search(r'static const struct regulator_desc '+descriptor+r' = \{(.*?)\n\};', driver, re.S)[1]
        ranges = re.findall(r'REGULATOR_LINEAR_RANGE\((\d+),\s*(\d+),\s*(\d+),\s*(\d+)\)', body)
        assert len(ranges) == 1
        low, first, last, step = map(int, ranges[0]); assert first == 0
        cmin, cmax = cells(props['regulator-min-microvolt'])[0], cells(props['regulator-max-microvolt'])[0]
        supported = [low+i*step for i in range(last+1) if cmin <= low+i*step <= cmax]
        cases.append(dict(name=group+'/'+name, descriptor=descriptor, base=low, count=last+1, step=step, cmin=cmin, cmax=cmax, supported=supported, expected=0 if supported else -22))
old = next(c for c in cases if c['name']=='regulators-0/l4')
assert old['supported']==[] and old['base']==1504000 and old['step']==8000
corrected = {**old, 'name':'regulators-0/l4-corrected', 'cmin':2944000, 'supported':[2944000], 'expected':0}
cases.append(corrected)
stock = []
for name in ('stock-00-merged.dtb','stock-01-merged.dtb'):
    tree = read_fdt((ROOT / 'firmware/extracted/stock-dtbo-20260914' / name).read_bytes())
    refs = {cells(p['phandle'])[0]: n for n,p in tree.items() if 'phandle' in p}
    supply = tree[refs[cells(tree['/soc/sdhci@c0c4000']['vdd-supply'])[0]]]
    minimum, maximum = cells(supply['regulator-min-microvolt'])[0], cells(supply['regulator-max-microvolt'])[0]
    assert minimum <= corrected['cmin'] <= corrected['cmax'] <= maximum
    stock.append(dict(file=name, min_uV=minimum, max_uV=maximum))

harness = r'''
#include <assert.h>
#include <stdbool.h>
#include <limits.h>
#include <errno.h>
#include <stdio.h>
#define EPROBE_DEFER 517
#define ERR_PTR(x) ((void *)(long)(x))
#define rdev_err(...) ((void)0)
#define rdev_info(...) ((void)0)
#define rdev_dbg(...) ((void)0)
struct regulator_dev;
struct regulator_ops { int (*list_voltage)(struct regulator_dev *, unsigned int); };
struct regulator_desc { const struct regulator_ops *ops; int n_voltages, continuous_voltage_range; };
struct regulation_constraints { int min_uV, max_uV; bool apply_uV; };
struct regulator_dev { struct regulator_desc *desc; struct regulation_constraints *constraints; };
static int base, step, writes, voltage;
static int list(struct regulator_dev *r, unsigned int i) { (void)r; return base+(int)i*step; }
static int regulator_get_voltage_rdev(struct regulator_dev *r) { (void)r; return voltage; }
static int _regulator_do_set_voltage(struct regulator_dev *r, int lo, int hi) {
    (void)r; assert(lo==hi); assert((lo-base)%step==0); writes++; voltage=lo; return 0;
}
'''+function+'\nint main(void) {\n'
for case in cases:
    expected_voltage = min(case['supported']) if case['supported'] else 0
    harness += f'''{{
 struct regulator_ops ops={{.list_voltage=list}};
 struct regulator_desc desc={{.ops=&ops,.n_voltages={case['count']}}};
 struct regulation_constraints c={{{case['cmin']},{case['cmax']},true}};
 struct regulator_dev r={{&desc,&c}};
 base={case['base']};step={case['step']};voltage=0;writes=0;
 int result=machine_constraints_voltage(&r,&c);
 assert(result=={case['expected']});assert(writes=={1 if case['supported'] else 0});
 assert(voltage=={expected_voltage});
 printf("{case['name']} result=%d min=%d max=%d writes=%d voltage=%d\\n",result,c.min_uV,c.max_uV,writes,voltage);
 }}\n'''
harness += 'return 0;\n}\n'
OUT.mkdir(exist_ok=False);WORK.mkdir(exist_ok=False)
(WORK/'test.c').write_text(harness)
subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O2',str(WORK/'test.c'),'-o',str(WORK/'test')],check=True)
result = subprocess.check_output([str(WORK/'test')]).decode()
(OUT/'test.c').write_text(harness);(OUT/'results.txt').write_text(result)
report = dict(passed=True, cases=cases, stock_bounds=stock,
              core_function_sha256=hashlib.sha256(function.encode()).hexdigest(),
              scope='Actual pinned core function and regulator voltage tables; all voltage reads/writes replaced by inert stubs. No physical regulator operation or complete provider-unwind emulation.')
(OUT/'report.json').write_text(json.dumps(report,indent=2)+'\n')
print(result)
