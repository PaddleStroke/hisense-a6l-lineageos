"""Actual logger rotation: tight metadata may prune; userdata keeps both streams."""
import importlib.util, json, os, subprocess, tempfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('decoder',root/'tools/decode-a6l-log-ring.py')
decoder=importlib.util.module_from_spec(spec);spec.loader.exec_module(decoder)
receipts=[]
with tempfile.TemporaryDirectory(prefix='a6l-data-retention-') as folder:
 tmp=Path(folder);ring=tmp/'ring';bin=tmp/'bin';bin.mkdir()
 subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',str(root/'device/hisense/a6l/rom/debug/a6l-log-ring.c'),'-o',str(ring)],check=True)
 for name,body in {'getprop':'case "$1" in logd.ready) echo true;; sys.boot_completed) echo 1;; esac',
                   'setprop':'printf "%s %s\\n" "$1" "$2" >> "$TEST_PROPS"','sync':'exit 0',
                   'df':'if [ -d "$2/prev" ]; then free=2920; else free=4096; fi; printf "Filesystem Size Used Available\\nmock 0 0 %s\\n" "$free"'}.items():
  f=bin/name;f.write_text('#!/bin/sh\n'+body+'\n');f.chmod(0o755)
 producer=tmp/'logcat.py';producer.write_text('#!/usr/bin/env python3\nimport os,signal\nos.write(1,b"NEW_LOGCAT\\n"*2000)\nsignal.pause()\n');producer.chmod(0o755)
 for profile in ('metadata','data'):
  d=tmp/profile;cur=d/'cur';cur.mkdir(parents=True)
  for stream in ('kmsg','logcat'):
   subprocess.run([str(ring),str(cur/(stream+'.ring'))],input=('PRIOR_'+stream.upper()+'\n').encode()*2000,check=True)
  fifo=tmp/(profile+'.pipe');os.mkfifo(fifo);fd=os.open(fifo,os.O_RDWR)
  # Below pipe capacity; held read/write descriptor prevents producer EOF.
  os.write(fd,b'NEW_KERNEL\n'*100)
  prop='vendor.a6l.bootlog.data_health' if profile=='data' else 'vendor.a6l.bootlog.health'
  env=dict(os.environ,PATH=str(bin)+':'+os.environ['PATH'],A6L_BL_DIR=str(d),TEST_PROPS=str(tmp/(profile+'.props')),A6L_BL_HEALTH_PROP=prop,
           A6L_BL_KMSG=str(fifo),A6L_BL_RING=str(ring),A6L_BL_LOGCAT=str(producer),
           A6L_BL_PM_TRACE='/missing',A6L_BL_PSTORE=str(tmp/'absent'),A6L_BL_TOMB=str(tmp/'absent'),
           A6L_BL_DF_AVAIL_KB='65536' if profile=='data' else '',
           A6L_BL_CAP_KB='4096',A6L_BL_FREE_KB='16384' if profile=='data' else '1024',
           A6L_BL_KEEP_PREV_LOGCAT='1' if profile=='data' else '0',A6L_BL_AFTER_BC='2')
  p=subprocess.run(['bash',str(root/'device/hisense/a6l/rom/debug/a6l-bootlog.sh')],env=env,capture_output=True,timeout=20)
  os.close(fd);assert p.returncode==0,(p.stderr,(cur/'bootlog.txt').read_text())
  properties=(tmp/(profile+'.props')).read_text()
  assert prop+' healthy' in properties and prop+' stopped' in properties
  if profile=='data':assert 'vendor.a6l.bootlog.health ' not in properties
  for stream in ('kmsg','logcat'):
   data,health=decoder.decode(cur/(stream+'.ring'));assert data and not health['rejected_slots']
  if profile=='data':
   for stream in ('kmsg','logcat'):
    data,health=decoder.decode(d/'prev'/(stream+'.ring'))
    assert ('PRIOR_'+stream.upper()).encode() in data and not health['rejected_slots']
   assert sum(f.stat().st_size for f in d.rglob('*') if f.is_file())<4096*1024
  else:
   assert not (d/'prev').exists()
   assert 'previous boot removed' in (cur/'bootlog.txt').read_text()
  receipts.append({'profile':profile,'previous_streams_retained':profile=='data','current_streams_valid':True})
print(json.dumps({'passed':True,'actual_supervisor_profiles':receipts}))
