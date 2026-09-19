"""Summarize observed V45 results, keeping physical and software evidence distinct."""
import hashlib,json,re
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];OUT=ROOT/'captures/capture-controls-user-v45'
def read(p):return json.loads((OUT/p).read_text())
def slot_updates(path):
    # This counts coordinate updates, NOT active contacts: a capture beginning
    # mid-contact lacks initial tracking IDs and cannot reconstruct full state.
    slot=None;changed=set();counts=[];tracking=0
    for line in (OUT/path).read_text().splitlines():
        m=re.search(r'\b(EV_ABS|EV_SYN)\s+(\w+)\s+([0-9a-fA-F]+)\s*$',line)
        if not m:continue
        kind,code,value=m.groups();value=int(value,16)
        if code=='ABS_MT_SLOT':slot=value
        elif code=='ABS_MT_TRACKING_ID':tracking+=1
        elif code in ['ABS_MT_POSITION_X','ABS_MT_POSITION_Y'] and slot is not None:changed.add(slot)
        elif code=='SYN_REPORT':counts.append(len(changed));changed=set()
    return {'frames_with_two_or_more_slots_updating':sum(c>=2 for c in counts),'maximum_slots_updating_in_one_frame':max(counts,default=0),'tracking_id_events_seen':tracking,'initial_slot_state_not_captured':True,'not_an_active_contact_count':True}
surface=read('surface/report.json');battery=read('stages/battery/report.json')
keys=read('stages/keys/report.json');touch=read('touch-two-finger/report.json')
report={
 'version':'v45','candidate_sha256':'aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14',
 'graphics':{'software_pass':surface['passed'],'user_confirmed':True,'real_surfaceflinger':surface['graphics_passed'],'software_rendering':surface['software_vulkan']},
 'touch':{'front_driver_bound':True,'attended_frames_first':read('touch-repeat-attended/report.json')['frames'],'attended_frames_second':touch['frames'],'coordinate_errors':touch['errors'],'dropped_events':touch['dropped_events'],'multitouch_evidence':slot_updates('touch-two-finger/events.txt'),'limitation':'Initial input state missing: decoded max-contact count is a lower bound. Repeat with recorder armed before lifting/reapplying fingers, or snapshot EVIOCGMTSLOTS.'},
 'buttons':{'observed_press_codes':keys['key_down_codes'],'eink_key_missing':True,'user_confirmed_eink_key_was_pressed':True,'eink_irq_count':0},
 'brightness':{'software_pass':read('stages-followup/brightness/report.json')['passed'],'user_confirmed':True,'original_value_restored':True},
 'vibration':{'software_pass':read('stages/vibration/report.json')['passed'],'user_felt_pulse':False,'status':'Physical output unconfirmed; do not report working'},
 'battery':{'software_pass':battery['passed'],'readings':battery['telemetry_fields'],'charging_validated':False,'accuracy_calibrated':False},
 'runtime_repairs':['Registered input character nodes created in /dev tmpfs','Missing printf symlink supplied in RAM root filesystem'],
 'persistent_changes':'Only the verified diagnostic recovery image was installed; test stages used RAM and hardware controls, no persistent filesystem mounts',
 'session':{k:v for k,v in read('session.json').items() if k in ['phase','worker_exit','android_return_verified','services_restored','finished_utc','error','cleanup_errors']}}
(OUT/'analysis.json').write_text(json.dumps(report,indent=2)+'\n')
files={str(p.relative_to(OUT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in OUT.rglob('*') if p.is_file() and p.name!='evidence-sha256.json'}
(OUT/'evidence-sha256.json').write_text(json.dumps(files,indent=2)+'\n')
print(json.dumps(report,indent=2))
