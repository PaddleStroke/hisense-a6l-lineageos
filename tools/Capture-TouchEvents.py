"""Bounded, read-only front-touch capture in authenticated A6L V38 diagnostics.

No event grabbing, injected touches, sysfs writes or controller configuration.
The default twenty seconds is an interactive gesture test, not a boot delay.
"""
import argparse,json,re,subprocess
from pathlib import Path

SERIAL='HLTE730T-PROBE'
def front_devices(capabilities):
    devices=[]
    for block in re.split(r'(?=add device \d+:)',capabilities):
        node=re.search(r'add device \d+: (/dev/input/event\d+)',block)
        if not node or 'INPUT_PROP_DIRECT' not in block:continue
        def maximum(axis):
            m=re.search(axis+r'\s*:.*?min (\d+), max (\d+)',block)
            return int(m[2]) if m and m[1]=='0' else None
        x,y=maximum('ABS_MT_POSITION_X'),maximum('ABS_MT_POSITION_Y')
        slots=maximum('ABS_MT_SLOT')
        if x in (1079,1080) and y in (2339,2340) and slots is not None and slots<32:
            devices.append({'path':node[1],'max_x':x,'max_y':y,'max_slot':slots})
    return devices

def decode(raw,device):
    slots={};slot=0;dropped=False;frames=[];errors=[];drops=0
    for line in raw.splitlines():
        m=re.search(r'\[\s*([\d.]+)\].*?\b(EV_ABS|EV_SYN)\s+(\w+)\s+([0-9a-fA-F]+)\s*$',line)
        if not m:continue
        stamp,kind,code,value=m.groups();value=int(value,16)
        if kind=='EV_SYN':
            if code=='SYN_DROPPED':
                dropped=True;slots={};drops+=1
            elif code=='SYN_REPORT':
                if dropped:dropped=False;slot=None;continue
                points=[{'slot':s,**p} for s,p in sorted(slots.items()) if p.get('id',-1)>=0 and 'x' in p and 'y' in p]
                frames.append({'time':float(stamp),'points':points})
            continue
        if dropped:continue
        if code=='ABS_MT_SLOT':
            slot=value if value<=device['max_slot'] else None
            if slot is None:errors.append('slot outside declared range')
        elif slot is not None:
            point=slots.setdefault(slot,{'id':-1})
            if code=='ABS_MT_TRACKING_ID':
                # Type-B slots retain axes; unchanged coordinates can be
                # omitted when a new contact replaces the old tracking ID.
                point['id']=-1 if value==0xffffffff else value
            elif code in ('ABS_MT_POSITION_X','ABS_MT_POSITION_Y'):
                axis='x' if code.endswith('_X') else 'y'
                if value>device['max_'+axis]:errors.append(axis+' outside declared range')
                else:point[axis]=value
    return {'frames':frames,'errors':errors,'dropped_events':drops,
            'touch_observed':any(f['points'] for f in frames)}

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('output',type=Path)
    ap.add_argument('--seconds',type=int,default=20);args=ap.parse_args()
    assert 5<=args.seconds<=60
    args.output.mkdir(mode=0o700,parents=True,exist_ok=False)
    report={'phone_writes':False,'seconds':args.seconds,'serial':SERIAL,'complete':False}
    def adb(*command,timeout=10):
        return subprocess.run(['adb','-s',SERIAL,*command],text=True,capture_output=True,timeout=timeout)
    try:
        identity=adb('shell','getprop ro.a6l.ramdiag; uname -r; getprop ro.adb.secure; id')
        (args.output/'identity.txt').write_text(identity.stdout+identity.stderr)
        lines=identity.stdout.splitlines()
        assert identity.returncode==0 and lines[:3]==['v38','7.2.3-a6l-probe+','1'] and len(lines)>3 and 'uid=0(root)' in lines[3]
        caps=adb('shell','/system/bin/toolbox','getevent','-lp');(args.output/'capabilities.txt').write_text(caps.stdout+caps.stderr)
        assert caps.returncode==0
        candidates=front_devices(caps.stdout);assert len(candidates)==1,candidates
        device=candidates[0];report['device']=device
        print('Touch the front screen: corners, centre, a swipe, then two fingers. Recording for',args.seconds,'seconds.',flush=True)
        result=adb('shell','/system/bin/toybox','timeout',str(args.seconds),'/system/bin/toolbox','getevent','-lt',device['path'],timeout=args.seconds+10)
        (args.output/'events.txt').write_text(result.stdout);(args.output/'stderr.txt').write_text(result.stderr)
        assert result.returncode in (0,124,143),result.returncode
        decoded=decode(result.stdout,device)
        (args.output/'frames.json').write_text(json.dumps(decoded,indent=2)+'\n')
        report.update(complete=True,touch_observed=decoded['touch_observed'],errors=decoded['errors'],dropped_events=decoded['dropped_events'],frames=len(decoded['frames']))
    except Exception as e:report['error']=repr(e)
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
    return 0 if report['complete'] and report['touch_observed'] and not report['errors'] and not report['dropped_events'] else 1
if __name__=='__main__':raise SystemExit(main())
