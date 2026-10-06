#!/usr/bin/env python3
"""Offline fourth-review reproductions. No adb, RF, real sysfs, or source edits.
Run under WSL: python3 .../run_review.py. Compiles snapshots in a fresh /tmp dir.
Assertions describe existing defects; PASS means reproduced, not fixed.
"""
from pathlib import Path
import hashlib, json, os, shutil, socket, struct, subprocess, tempfile, threading

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
DEV = ROOT / 'device/hisense/a6l'
PLATFORM = Path('/home/a6l/android/a6l-lineage24')
sources = [
 'eink/src/a6l_epdd.c', 'eink/src/a6l_eink_mirror.c', 'eink/src/eink_logic.c',
 'eink/src/eink_logic.h', 'eink/a6l_eink.rc', 'wifi/hal/a6l_wifi_hal.cpp',
 'rom/bin/a6l-radio.sh', 'rom/tests/test-a6l-radio-start.sh',
 'rom/v2/init.a6l.wifibt.rc', 'rom/modules/bt.txt', 'rom/modules/ipa.txt',
 'rom/modules/radio.txt', 'audio/patches/0002-a6l-call-route-mute.patch',
 'audio/mixer_paths_a6l.xml', 'kvoice/q6voiced/a6l_q6voiced.c',
]
platform_sources = [
 'hardware/interfaces/audio/aidl/default/Telephony.cpp',
 'hardware/interfaces/audio/aidl/default/ModulePrimary.cpp',
 'frameworks/av/media/libaudiohal/impl/DeviceHalAidl.cpp',
 'hardware/interfaces/bluetooth/aidl/default/net_bluetooth_mgmt.cpp',
 'external/drm_hwcomposer/drm/DrmPlane.cpp',
]
results = []
def record(name, **evidence):
    results.append(dict(test=name, **evidence))
    print(name, json.dumps(evidence), flush=True)

def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 1
    i = brace + 1
    while depth:
        depth += (text[i] == '{') - (text[i] == '}')
        i += 1
    return text[start:i]

with tempfile.TemporaryDirectory(prefix='a6l-review4-') as tmp:
    w = Path(tmp)
    snap = w/'source'
    provenance = []
    for base, paths, prefix in [(DEV, sources, 'device'), (PLATFORM, platform_sources, 'platform')]:
        for rel in paths:
            src = base/rel
            raw = src.read_bytes()
            dst = snap/prefix/rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(raw.replace(b'\r\n', b'\n'))
            provenance.append(dict(path=str(src), sha256=hashlib.sha256(raw).hexdigest()))
    (HERE/'source-provenance.json').write_text(json.dumps(provenance, indent=2)+'\n')
    sd = snap/'device'
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
    def run(cmd, **kw):
        r = subprocess.run([str(a) for a in cmd], cwd=w, env=env,
                           text=True, capture_output=True, timeout=30, **kw)
        if r.returncode:
            raise RuntimeError(f'{cmd}: {r.returncode}\n{r.stdout}\n{r.stderr}')
        return r
    def compile_run(name, code, cpp=False):
        f = w/(name+('.cpp' if cpp else '.c')); f.write_text(code)
        exe = w/name
        run(['g++' if cpp else 'gcc', '-std=gnu++20' if cpp else '-std=gnu11',
             '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
             '-no-pie', f, '-o', exe])
        r = run([exe]); (HERE/(name+'.log')).write_text(r.stdout+r.stderr)
        return r.stdout

    # F35: exact drive() body; all hardware calls replaced with deterministic fakes.
    body = function((sd/'eink/src/a6l_epdd.c').read_text(), 'static int drive(int n)')
    out = compile_run('epd-rails', r'''
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>
#define LOG(...) do { printf(__VA_ARGS__); puts(""); } while(0)
#define FRAME 4
struct fb { int id; void *map; };
static int started=1, lead=1, tail=1, last_ms, updates;
static unsigned gaps;
static uint32_t px=1, dst;
static uint32_t *frames[]={&px};
static struct fb idle={1,&dst}, fa={2,&dst}, fb2={3,&dst};
static int on_error, off_error, flips;
static int drm_start(void) { return 0; }
static double now(void) { return 1; }
static int power(int on) { return on ? on_error : off_error; }
static int flip(int id) { (void)id; flips++; return 0; }
''' + body + r'''
int main(void) {
 on_error=-1; int rc=drive(1);
 printf("rail_on_error=-1 return=%d flips=%d\n",rc,flips);
 assert(rc==0 && flips==3);
 on_error=0; off_error=-1; flips=0; rc=drive(1);
 printf("rail_off_error=-1 return=%d flips=%d\n",rc,flips);
 assert(rc==0 && flips==3);
}
''')
    record('F35 rails failures acknowledged as successful drive', output=out.strip())

    # F38: exact readiness function, fake property/sysfs predicates; no netlink.
    body = function((sd/'wifi/hal/a6l_wifi_hal.cpp').read_text(), 'wifi_error a6l_wait_for_driver_ready()')
    out = compile_run('wifi-ready', r'''
#include <string>
#include <cstring>
#include <cstdio>
#include <cassert>
#define PROPERTY_VALUE_MAX 92
#define ALOGI(...) do {} while(0)
#define ALOGE(...) do {} while(0)
#define ALOGW(...) do {} while(0)
using wifi_error=int;
enum { WIFI_SUCCESS=0, WIFI_ERROR_UNKNOWN=-1, WIFI_ERROR_TIMED_OUT=-7 };
static const char *state="";
static int ticks;
static std::string primaryIfaceName() { return "wlan0"; }
static int property_get_int32(const char *, int) { return 1; }
static int property_get(const char *k, char *out, const char *) {
 strcpy(out, !strcmp(k,"persist.vendor.a6l.radio") ? "1" : state); return strlen(out);
}
static bool isWirelessNetdev(const std::string &) { return true; }
static int usleep(unsigned) { ticks++; return 0; }
''' + body + r'''
int main() {
 for(auto s : {"", "starting", "stopped", "missing"}) {
   state=s; ticks=0; int rc=a6l_wait_for_driver_ready();
   printf("present=1 state='%s' rc=%d wait_ticks=%d\n",s,rc,ticks);
   assert(rc==WIFI_SUCCESS && ticks==11);
 }
 state="ready"; ticks=0;
 assert(a6l_wait_for_driver_ready()==WIFI_SUCCESS && ticks==0);
}
''', True)
    record('F38 readiness bypass', output=out.strip())

    # F36: compile the full mirror, use a real Unix socket and a constant source image.
    mirror = w/'mirror'
    run(['gcc', '-DNO_DRM', '-O1', '-g', '-fsanitize=address,undefined', '-no-pie',
         sd/'eink/src/a6l_eink_mirror.c', sd/'eink/src/eink_logic.c', '-o', mirror])
    raw = w/'constant.raw'
    raw.write_bytes(struct.pack('<4I', 8, 16, 1, 0)+bytes([128,128,128,255])*128)
    for scenario in ['error', 'disconnect']:
        path = w/(scenario+'.sock')
        srv = socket.socket(socket.AF_UNIX); srv.bind(str(path)); srv.listen(); srv.settimeout(0.1)
        commands=[]; stop=threading.Event(); errors=[]
        def serve():
            try:
                while not stop.is_set():
                    try: conn,_ = srv.accept()
                    except socket.timeout: continue
                    with conn:
                        conn.settimeout(0.2)
                        buf=b''
                        while not stop.is_set():
                            try:
                                while b'\n' not in buf:
                                    data=conn.recv(65536)
                                    if not data: break
                                    buf+=data
                                if b'\n' not in buf: break
                                line,buf=buf.split(b'\n',1)
                                commands.append(line.decode())
                                if line.startswith(b'frame '):
                                    _,ww,hh,*_=line.split(); need=int(ww)*int(hh)
                                    while len(buf)<need:
                                        data=conn.recv(65536)
                                        if not data: raise RuntimeError('short frame')
                                        buf+=data
                                    buf=buf[need:]
                                    if scenario=='disconnect': break
                                    conn.sendall(b'ERR update failed (injected)\n')
                                else: conn.sendall(b'OK cleared\n')
                            except socket.timeout: continue
            except Exception as e: errors.append(repr(e))
        th=threading.Thread(target=serve); th.start()
        try:
            r=run([mirror,'--source','file:'+str(raw),'--epd-socket',path,
                   '--no-props','--mode','mirror','--key-dev','none','--touch-dev','none',
                   '--interval','50','--frames','100'])
        finally:
            stop.set(); th.join(2); srv.close()
        (HERE/('mirror-'+scenario+'.log')).write_text(r.stdout+r.stderr)
        frames=[c for c in commands if c.startswith('frame ')]
        assert not errors, errors
        assert len(frames)==1, commands
        assert 'exit after 100 frames' in r.stdout
        record('F36 '+scenario+' is never retried for constant page', commands=commands, captures=100)

    # F39/F40: reuse only the fake-root setup from the existing test, not its tests or cleanup.
    original=(sd/'rom/tests/test-a6l-radio-start.sh').read_text()
    setup=original[:original.index('# failure cases: modem start never reached')]
    # Temporary sandbox supplied by this runner rather than an untracked mktemp directory.
    setup=setup.replace('W=$(mktemp -d)', 'W="$REVIEW_FAKE"')
    checks=r'''
setup
FAIL_MODS=ipa2_lite run
echo "EARLY_FAIL state=$st bt_modules=$(grep -Ec '^btqca$|^hci_uart$' $F/insmod.log) addr_start=$(grep -c '^ctl.start vendor.a6l-bt-addr' $F/setprop.log)"
setup
# Fail after the modem start write: fake sysfs stays 'start' instead of becoming 'running'.
sed -i '/st=\$S\/class\/remoteproc/,/exit 0/{ /\[.*cat.*start.*echo running/d; }' $W/bin/sleep
run
echo "LATE_FAIL state=$st bt_modules=$(grep -Ec '^btqca$|^hci_uart$' $F/insmod.log) addr_start=$(grep -c '^ctl.start vendor.a6l-bt-addr' $F/setprop.log)"
setup running
# The state path becomes a directory: the redirection fails (EISDIR), while the
# cat shim continues to report a running modem. All paths are in the temp root.
rm $F/root/sys/class/remoteproc/remoteproc1/state
mkdir $F/root/sys/class/remoteproc/remoteproc1/state
cat > $W/bin/cat <<'SH'
#!/bin/sh
case "$1" in */remoteproc1/state) echo running;; *) exec /bin/cat "$@";; esac
SH
chmod +x $W/bin/cat
env PATH="$W/bin:$PATH" FAKE=$F A6L_ROOT=$F/root dash $W/a6l-radio.sh stop >$F/stop.log 2>&1
rc=$?
echo "STOP_NOT_CONFIRMED rc=$rc state=$(cat $F/props/vendor.a6l.radio.state) daemons_stopped=$(grep -c '^ctl.stop ' $F/setprop.log)"
cat $F/stop.log
'''
    (w/'radio-check.sh').write_text(setup+checks)
    fake=w/'fake'; fake.mkdir()
    env['REVIEW_FAKE']=str(fake)
    r=run(['bash',w/'radio-check.sh',sd/'rom'])
    (HERE/'radio-lifecycle.log').write_text(r.stdout+r.stderr)
    assert 'EARLY_FAIL state=failed:ipa-module bt_modules=2 addr_start=0' in r.stdout, r.stdout
    assert 'LATE_FAIL state=failed:mss-start bt_modules=0 addr_start=0' in r.stdout, r.stdout
    assert 'STOP_NOT_CONFIRMED rc=0 state=stopped daemons_stopped=3' in r.stdout, r.stdout
    record('F39 Bluetooth incomplete on modem failures; F40 stop false success', output=r.stdout.strip())

    # Read-only platform call-chain artifact, including full relevant methods.
    tele=(snap/'platform'/platform_sources[0]).read_text()
    mod=(snap/'platform'/platform_sources[1]).read_text()
    dev=(snap/'platform'/platform_sources[2]).read_text()
    trace='\n\n'.join([
        function(dev, 'status_t DeviceHalAidl::setVoiceVolume('),
        function(mod, 'ndk::ScopedAStatus ModulePrimary::getTelephony('),
        function(tele, 'ndk::ScopedAStatus Telephony::setTelecomConfig(')])
    (HERE/'audio-volume-trace.txt').write_text(trace+'\n')
    assert 'make<Telephony>()' in trace
    assert 'mTelecomConfig.voiceVolume = in_config.voiceVolume;' in trace
    record('F37 source trace saved (static evidence, not acoustic test)', file='audio-volume-trace.txt')

    # F41: exact capture_drm() against a 2x2 KMS plane with controlled metadata.
    # DRM objects are reduced host fakes; pixels live in RAM, not a device mapping.
    body=function((sd/'eink/src/a6l_eink_mirror.c').read_text(), 'static int capture_drm(void)')
    out=compile_run('plane-composition', r'''
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define LOG(...) do {} while(0)
#define CAP_FRONT_OFF 1
#define DRM_MODE_OBJECT_PLANE 1
#define DRM_FORMAT_XRGB8888 1
#define DRM_FORMAT_ARGB8888 2
#define DRM_FORMAT_XBGR8888 3
#define DRM_FORMAT_ABGR8888 4
#define DRM_FORMAT_RGB565 5
#define DRM_MODE_FB_MODIFIERS 1
#define DRM_FORMAT_MOD_LINEAR 0
#define DRM_CLOEXEC 1
#define DMA_BUF_SYNC_START 1
#define DMA_BUF_SYNC_READ 2
#define DMA_BUF_SYNC_END 4
#define DMA_BUF_IOCTL_SYNC 1
#define DRM_IOCTL_GEM_CLOSE 2
#define PROT_READ 1
#define MAP_SHARED 1
#define MAP_FAILED ((void*)-1)
struct dma_buf_sync { unsigned flags; };
struct drm_gem_close { uint32_t handle; };
typedef struct { unsigned count_planes; uint32_t *planes; } drmModePlaneRes;
typedef struct { uint32_t crtc_id, fb_id, plane_id; } drmModePlane;
typedef struct { uint32_t pixel_format, flags, handles[4], pitches[4], offsets[4], height, width; uint64_t modifier; } drmModeFB2;
struct pl { uint32_t fb; int64_t zpos; int cx,cy,cw,ch; double sx,sy,sw,sh; };
static int drm_fd=123, front_w=2,front_h=2,gw=2,gh=2;
static uint8_t gray_data[4], *gray=gray_data, pixels[16];
static uint32_t plane_id=1;
static uint64_t rotation=1, opacity=65535, blend=1;
static int transform_queries;
static drmModePlaneRes pr={1,&plane_id};
static drmModePlane plane={1,1,1};
static drmModeFB2 fb={.pixel_format=DRM_FORMAT_XRGB8888,.handles={1},.pitches={8},.height=2,.width=2};
static int drm_allowed(void) { return 1; }
static int drm_open_once(void) { return 0; }
static int lcd_crtc(uint32_t *c,int *w,int *h) { *c=1; *w=*h=2; return 1; }
static int gray_alloc(int w,int h) { gw=w; gh=h; return 0; }
static int cmp_pl(const void *a,const void *b) { (void)a; (void)b; return 0; }
static uint64_t prop_of(uint32_t obj,uint32_t type,const char *name,uint64_t def) {
 (void)obj; (void)type;
 if(!strcmp(name,"rotation")){ transform_queries++; return rotation; }
 if(!strcmp(name,"alpha")){ transform_queries++; return opacity; }
 if(!strcmp(name,"pixel blend mode")){ transform_queries++; return blend; }
 return def;
}
static drmModePlaneRes *drmModeGetPlaneResources(int fd) { (void)fd; return &pr; }
static drmModePlane *drmModeGetPlane(int fd,uint32_t p) { (void)fd; (void)p; return &plane; }
static void drmModeFreePlane(drmModePlane *p) { (void)p; }
static void drmModeFreePlaneResources(drmModePlaneRes *p) { (void)p; }
static drmModeFB2 *drmModeGetFB2(int fd,uint32_t f) { (void)fd; (void)f; return &fb; }
static void drmModeFreeFB2(drmModeFB2 *f) { (void)f; }
static int drmPrimeHandleToFD(int fd,uint32_t h,int flags,int *out) { (void)fd;(void)h;(void)flags; *out=456; return 0; }
static void *mmap(void *a,size_t n,int prot,int flags,int fd,long off) { (void)a;(void)n;(void)prot;(void)flags;(void)fd;(void)off; return pixels; }
static int munmap(void *p,size_t n) { (void)p;(void)n; return 0; }
static int ioctl(int fd,int request,void *data) { (void)fd;(void)request;(void)data; return 0; }
static int drmIoctl(int fd,int request,void *data) { return ioctl(fd,request,data); }
static int close(int fd) { (void)fd; return 0; }
static uint8_t luma(int r,int g,int b) { return (77*r+150*g+29*b)>>8; }
''' + body + r'''
static void px(int i,int v,int alpha) { pixels[4*i]=pixels[4*i+1]=pixels[4*i+2]=v; pixels[4*i+3]=alpha; }
int main(void) {
 for(int i=0;i<4;i++)px(i,255,255);
 opacity=0; assert(capture_drm()==0);
 printf("alpha=0 expected_black=0 actual=%u\n",gray[0]); assert(gray[0]==255);
 opacity=65535; rotation=4; /* DRM_MODE_ROTATE_180 */
 px(0,0,255); px(1,64,255); px(2,128,255); px(3,255,255);
 assert(capture_drm()==0);
 printf("rotation=180 expected=255,128,64,0 actual=%u,%u,%u,%u\n",gray[0],gray[1],gray[2],gray[3]);
 assert(gray[0]==0 && gray[3]==255);
 rotation=1; fb.pixel_format=DRM_FORMAT_ARGB8888; blend=2; /* Coverage */
 for(int i=0;i<4;i++)px(i,200,128);
 assert(capture_drm()==0);
 printf("coverage alpha=128 expected_about=100 actual=%u metadata_queries=%d\n",gray[0],transform_queries);
 assert(gray[0]==200 && transform_queries==0);
}
''')
    record('F41 plane transforms and blending ignored',output=out.strip())
    plane_src=(snap/'platform'/platform_sources[4]).read_text()
    (HERE/'compositor-plane-trace.txt').write_text('\n'.join(
        f'{i}: {line}' for i,line in enumerate(plane_src.splitlines(),1) if 435<=i<=470)+'\n')

    # Confirm source wasn't changed by another worker during the run.
    changed=[p['path'] for p in provenance if hashlib.sha256(Path(p['path']).read_bytes()).hexdigest()!=p['sha256']]
    record('concurrent source-change check', changed=changed)
    (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    assert not changed, changed
print('REVIEW4_REPRODUCTIONS_PASS')
