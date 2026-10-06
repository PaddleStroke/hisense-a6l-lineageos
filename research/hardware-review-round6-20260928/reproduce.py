"""Offline review only. Assertions demonstrate faults, not desired acceptance.
Run with WSL python3. Production is snapshotted, never edited; no device access.
"""
from pathlib import Path
import hashlib, json, os, subprocess, tempfile, zipfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEV = ROOT / 'device/hisense/a6l'
hashes = {}
def method(source, signature):
    start = source.index(signature)
    pos = source.index('{', start)
    end, depth = pos + 1, 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

with tempfile.TemporaryDirectory(prefix='a6l-review6-') as tmp:
    work = Path(tmp)
    with zipfile.ZipFile(HERE/'reviewed-source.zip', 'w', zipfile.ZIP_DEFLATED) as archive:
        paths = []
        for subtree in ('gnss/lib', 'gnss/hal', 'hals/sensors/stk3338', 'audio/route', 'kernel/stk3338'):
            paths.extend(p for p in (DEV/subtree).rglob('*') if p.suffix in ('.c','.cc','.cpp','.h') and not any(x.startswith('build') for x in p.relative_to(DEV).parts))
        paths.append(DEV/'audio/mixer_paths_a6l.xml')
        for p in paths:
            data = p.read_bytes()
            rel = str(p.relative_to(ROOT))
            hashes[rel] = hashlib.sha256(data).hexdigest()
            archive.writestr(rel, data)
            dest = work/p.relative_to(DEV)
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        for p, rel in [
            (Path('/home/a6l/kernel/a6l-baseline-7.2/drivers/iio/industrialio-event.c'), 'external/industrialio-event.c'),
            (Path('/home/a6l/android/a6l-lineage24/hardware/interfaces/gnss/aidl/android/hardware/gnss/IGnss.aidl'), 'external/IGnss.aidl')]:
            data = p.read_bytes()
            archive.writestr(rel, data)
            hashes[str(p)] = hashlib.sha256(data).hexdigest()
            dest = work/rel; dest.parent.mkdir(parents=True, exist_ok=True); dest.write_bytes(data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes, indent=2)+'\n')

    gnss = work/'gnss/lib'; sensor = work/'hals/sensors/stk3338'; route = work/'audio/route'
    source = (work/'gnss/hal/Gnss.cpp').read_text()
    (work/'controls.inc').write_text('\n'.join(method(source, 'ScopedAStatus Gnss::'+name+'(') for name in ('setPositionMode','deleteAidingData')))

    # Extend only the copied test fixture to include a real XML control omitted
    # by its original fake card. The production route daemon stays byte-identical.
    fixture = (route/'tests/route_tests.c').read_text()
    fixture = fixture[:fixture.index('int main(void)')]
    fixture = fixture.replace('{ "Headphone Jack",', '{ "ADC2 MUX", 1, { "ZERO", "INP2", "INP3" }, 0 },\n\t{ "Headphone Jack",', 1)
    fixture = fixture.replace('set_enum("HPHL", "ZERO");', 'set_enum("ADC2 MUX", "ZERO"); set_enum("HPHL", "ZERO");', 1)
    fixture = fixture.replace('if (!strcmp(p, "headset-mic")) set_enum("Digital DEC1 MUX", "ADC2");',
        'if (!strcmp(p, "headset-mic")) { set_enum("Digital DEC1 MUX", "ADC2"); set_enum("ADC2 MUX", "INP2"); }')
    fixture += '''
#include <assert.h>
int main(void) {
    struct routed r = { .card = -1, .last_hp = -2 };
    ctl("Headphone Jack")->value = 1; ctl("Mic Jack")->value = 1;
    setprop(0, "1"); setprop(1, "headset"); setprop(2, "headset");
    assert(route_open(&r, 0, "x.xml") == 0);
    lost_write = "ADC2 MUX";
    assert(route_step(&r, "Hisense A6L") == 0);
    assert(ctl("ADC2 MUX")->value == 0 && applies == 1);
    assert(!strcmp(r.last_in_dev, "headset"));
    lost_write = NULL;
    for (int i=0; i<100; ++i) assert(route_step(&r, "Hisense A6L") == 0);
    assert(applies == 1 && ctl("ADC2 MUX")->value == 0);
    puts("PARTIAL_ROUTE_WRITE route_success=1 ADC2_MUX=ZERO applies=1 after_100_healthy_steps");
    route_close(&r);
}
'''
    # Verify that the fixture's added path control exists in the captured XML.
    import xml.etree.ElementTree as ET
    xml = ET.parse(work/'audio/mixer_paths_a6l.xml')
    assert xml.find("./path[@name='headset-mic']/ctl[@name='ADC2 MUX']").attrib['value'] == 'INP2'
    fixture_path = route/'tests/review_partial.c'; fixture_path.write_text(fixture)
    (HERE/'route_fixture.c').write_text(fixture)

    kernel = (work/'external/industrialio-event.c').read_text()
    poll_source = '''#include <assert.h>
#include <stdio.h>
#include <poll.h>
typedef unsigned int __poll_t;
#define EPOLLIN POLLIN
#define EPOLLRDNORM POLLRDNORM
struct iio_event_interface { int wait; int det_events; };
struct iio_dev_opaque { struct iio_event_interface *event_interface; };
struct iio_dev { void *info; struct iio_dev_opaque opaque; };
struct file { struct iio_dev *private_data; };
struct poll_table_struct { int unused; };
#define to_iio_dev_opaque(d) (&(d)->opaque)
#define kfifo_is_empty(f) (*(f) == 0)
#define poll_wait(f,w,p) ((void)0)
'''+method(kernel, 'static __poll_t iio_event_poll(')+'''
int main(void) {
    struct iio_event_interface ev = {0,1};
    struct iio_dev d = {(void*)1, {&ev}}; struct file f = {&d};
    assert(iio_event_poll(&f, NULL) == (POLLIN | POLLRDNORM));
    d.info = NULL;
    assert(iio_event_poll(&f, NULL) == 0);
    puts("IIO_UNREGISTERED poll_mask=0 even_with_queued_event (no_HUP_no_ERR_no_IN)");
}
'''
    (work/'kernel_poll.c').write_text(poll_source)
    (HERE/'kernel_poll.c').write_text(poll_source)

    sysroot = work/'fake-sys'; devroot = work/'fake-dev'; datadir = work/'fake-data'
    node = sysroot/'iio:device7'; (node/'events').mkdir(parents=True)
    devroot.mkdir(); datadir.mkdir()
    for name, value in {'name':'stk3338','in_proximity_raw':'0','in_illuminance_raw':'100','in_illuminance_scale':'1',
        'events/in_proximity_thresh_rising_en':'1','events/in_proximity_thresh_falling_en':'1',
        'events/in_proximity_thresh_rising_value':'120'}.items(): (node/name).write_text(value)
    os.mkfifo(devroot/'iio:device7')
    env = os.environ.copy(); env.update(A6L_SYSROOT=str(sysroot), A6L_DEVROOT=str(devroot), A6L_DATADIR=str(datadir))
    common = ['-O1','-g','-pthread','-fsanitize=undefined','-fno-omit-frame-pointer']
    jobs = [
        ('gnss', ['g++','-std=c++17',*common,'-I'+str(gnss),HERE/'gnss.cc',*(gnss/x for x in ('loc_client.cpp','loc_v02.cpp','qmi.cpp','nmea.cpp','android_map.cpp','qrtr_transport.cpp'))]),
        ('sensors', ['gcc','-std=gnu11',*common,'-DA6L_HOST_TEST','-I'+str(sensor),'-I'+str(sensor/'tests/include'),HERE/'sensors.c',sensor/'a6l_motion.c',sensor/'a6l_magcal.c','-lm']),
        ('controls', ['g++','-std=c++17',*common,'-I'+str(gnss),'-I'+str(work),HERE/'controls.cc',gnss/'loc_v02.cpp',gnss/'qmi.cpp']),
        ('route', ['gcc','-std=gnu11',*common,'-I'+str(route/'tests/stub'),fixture_path]),
        ('kernel-poll', ['gcc','-std=gnu11',*common,work/'kernel_poll.c']),
    ]
    results=[]
    for name, cmd in jobs:
        print('Building '+name, flush=True)
        exe = work/('review-'+name)
        build = subprocess.run([str(x) for x in [*cmd,'-o',exe]], capture_output=True, text=True, timeout=120)
        (HERE/(name+'-build.log')).write_text(build.stdout+build.stderr)
        if build.returncode: raise RuntimeError(name+' build: '+build.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30, env=env)
        (HERE/(name+'.log')).write_text(result.stdout+result.stderr)
        results.append({'test':name,'exit_code':result.returncode,'stdout':result.stdout})
        (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n')
        print(result.stdout, flush=True)
        if result.returncode: raise RuntimeError(name+' run: '+result.stderr)
    changed=[]
    for p,h in hashes.items():
        path = Path(p) if p.startswith('/') else ROOT/p
        if hashlib.sha256(path.read_bytes()).hexdigest() != h: changed.append(p)
    (HERE/'changed-during-run.json').write_text(json.dumps(changed,indent=2)+'\n')
    print('Completed 5 harnesses; source files changed during run:',len(changed),flush=True)
