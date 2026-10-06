#!/usr/bin/env python3
"""Run the actual supervisor with two flooding streams and finite test coverage."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import threading

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('decoder', ROOT / 'tools/decode-a6l-log-ring.py')
decoder = importlib.util.module_from_spec(spec); spec.loader.exec_module(decoder)


with tempfile.TemporaryDirectory(prefix='a6l-bootlog-integration-') as folder:
    tmp = Path(folder); tools = tmp / 'bin'; tools.mkdir()
    ring = tmp / 'ring'
    subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror',
                    str(ROOT/'device/hisense/a6l/rom/debug/a6l-log-ring.c'),'-o',str(ring)],check=True)
    def tool(name, text):
        p = tools / name; p.write_text('#!/bin/sh\n'+text); p.chmod(0o755)
    tool('getprop', '''case "$1" in
logd.ready) echo true;;
sys.boot_completed) echo 1;;
vendor.a6l.display) echo ready;;
init.svc.a6l_modules_display) echo stopped;;
ro.vendor.a6l.rom.build) echo ring-test;;
esac
''')
    tool('setprop', 'printf "%s %s\\n" "$1" "$2" >> "$TEST_PROPS"\n')
    tool('sync','exit 0\n')
    producer = tmp/'android-log.py'
    producer.write_text('#!/usr/bin/env python3\nimport os,signal\nfor i in range(2048):\n data=b"android audit flood\\n"*1600\n while data:\n  n=os.write(1,data);data=data[n:]\nsignal.pause()\n')
    producer.chmod(0o755)
    fifo=tmp/'kernel.pipe'; os.mkfifo(fifo)
    fd=os.open(fifo,os.O_RDWR)
    errors=[]
    def kernel():
        try:
            for _ in range(2048):
                data=b'kernel audit flood\n'*1600
                while data:
                    n=os.write(fd,data);data=data[n:]
        except BrokenPipeError:
            pass
        except Exception as error:
            errors.append(str(error))
    thread=threading.Thread(target=kernel,daemon=True); thread.start()
    env=dict(os.environ,PATH=str(tools)+':'+os.environ['PATH'],TEST_PROPS=str(tmp/'properties'),
             A6L_BL_DIR=str(tmp/'metadata/a6l'),A6L_BL_KMSG=str(fifo),A6L_BL_RING=str(ring),
             A6L_BL_LOGCAT=str(producer),A6L_BL_PM_TRACE='/missing-pm-trace',
             A6L_BL_PSTORE=str(tmp/'missing-pstore'),A6L_BL_TOMB=str(tmp/'missing-tombs'),
             A6L_BL_DF_AVAIL_KB='4096',A6L_BL_AFTER_BC='4')
    result=subprocess.run(['bash',str(ROOT/'device/hisense/a6l/rom/debug/a6l-bootlog.sh')],
                          env=env,capture_output=True,timeout=25)
    os.close(fd)
    assert result.returncode==0,result.stderr.decode()
    assert not errors,errors
    cur=tmp/'metadata/a6l/cur'
    props=(tmp/'properties').read_text(); log=(cur/'bootlog.txt').read_text()
    assert 'vendor.a6l.bootlog.health healthy' in props,props
    assert 'vendor.a6l.bootlog.health stopped' in props,props
    assert 'exhausted' not in log and 'exited rc=' not in log,log
    for stream in ['kmsg','logcat']:
        payload,health=decoder.decode(cur/(stream+'.ring'))
        assert payload and not health['rejected_slots'] and health['sequence_gaps'],health
        assert len(list(cur.glob(stream+'.*')))==1
    total=sum(p.stat().st_size for p in cur.iterdir() if p.is_file())
    assert total<1500*1024,total
    print(json.dumps({'passed':True,'supervisor_healthy_during_flood':True,
                      'retries':0,'stream_file_count':2,'total_current_bytes':total}))
