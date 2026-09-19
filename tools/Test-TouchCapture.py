"""Exercise wrong-panel rejection, multitouch/release, bounds, and lost-event handling."""
import importlib.util,json
from pathlib import Path
root=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('touch',root/'tools/Capture-TouchEvents.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
def caps(x,y):return f'add device 1: /dev/input/event1\n INPUT_PROP_DIRECT\n ABS_MT_SLOT : value 0, min 0, max 9\n ABS_MT_POSITION_X : value 0, min 0, max {x}\n ABS_MT_POSITION_Y : value 0, min 0, max {y}\n'
d=m.front_devices(caps(1079,2339))[0]
def events(seq):return '\n'.join(f'[ 1.000] /dev/input/event1: {kind} {code} {value:08x}' for kind,code,value in seq)
def a(code,value):return ('EV_ABS','ABS_MT_'+code,value)
s=('EV_SYN','SYN_REPORT',0)
seq=[a('SLOT',0),a('TRACKING_ID',12),a('POSITION_X',0),a('POSITION_Y',2339),a('SLOT',1),a('TRACKING_ID',13),a('POSITION_X',1079),a('POSITION_Y',0),s,a('SLOT',0),a('TRACKING_ID',0xffffffff),s]
decoded=m.decode(events(seq),d)
drop=m.decode(events(seq[:5]+[('EV_SYN','SYN_DROPPED',0),a('POSITION_X',55),s,s]),d)
bad=m.decode(events([a('SLOT',99),a('TRACKING_ID',3),s,a('SLOT',0),a('POSITION_X',9999),s]),d)
reuse=m.decode(events(seq+[a('SLOT',0),a('TRACKING_ID',14),s]),d)
checks={'upstream_dimensions':len(m.front_devices(caps(1079,2339)))==1,'stock_dimensions':len(m.front_devices(caps(1080,2340)))==1,'rear_rejected':not m.front_devices(caps(720,1440)),
        'two_fingers':len(decoded['frames'][0]['points'])==2,'release':len(decoded['frames'][1]['points'])==1 and decoded['frames'][1]['points'][0]['id']==13,
        'correct_edges':decoded['frames'][0]['points'][0]['y']==2339 and decoded['frames'][0]['points'][1]['x']==1079,
        'dropped_clears_contacts':drop['dropped_events']==1 and not drop['touch_observed'],
        'bounds_rejected':len(bad['errors'])==2,'idle_not_success':not m.decode(events([s]),d)['touch_observed'],
        'slot_axes_retained':reuse['frames'][-1]['points'][0]['id']==14 and reuse['frames'][-1]['points'][0]['y']==2339}
out=root/'research/touchscreen-prep-20260917';out.mkdir(exist_ok=True,parents=True)
(out/'capture-tests.json').write_text(json.dumps(checks,indent=2)+'\n');print(json.dumps(checks,indent=2));assert all(checks.values())
