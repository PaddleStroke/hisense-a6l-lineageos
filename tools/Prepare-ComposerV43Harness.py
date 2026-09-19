"""Derive the V43 package and diskless scanout verification from checked V42."""
from pathlib import Path
root=Path(__file__).resolve().parents[1]
dest=root/'tools/Test-AndroidPresentV43.py';assert not dest.exists()
s=(root/'tools/Test-AndroidGraphicsV42.py').read_text()
s=s.replace('argparse,gzip,hashlib,json,re,subprocess,time','argparse,gzip,hashlib,json,re,subprocess,time,os,socket,struct')
s=s.replace('graphics-v42','present-v43').replace('a6l-v42','a6l-v43').replace('/v42','/v43').replace('v42-console','v43-console')
s=s.replace("P/'system/bin/a6l_graphics_services'","P/'system/bin/a6l_present_services'")
s=s.replace("put(name,P/group/path,0o755 if path.startswith('bin/') else 0o644)","put(name,P/group/('bin/a6l_present_client' if path=='bin/a6l_graphics_client' else path),0o755 if path.startswith('bin/') else 0o644)")
s=s.replace("['graphics_services.c','graphics_client.cpp']","['present_services.c','present_client.cpp']")
s=s.replace('/tmp/a6l-v43/bin/a6l_graphics_services\necho', '''(
  while [ ! -f /tmp/a6l-v43/root/logs/client.log ]; do sleep 0.1; done
  /system/bin/toybox tail -n +1 -f /tmp/a6l-v43/root/logs/client.log
) &
tail_pid=$!
/tmp/a6l-v43/bin/a6l_graphics_services
probe_exit=$?
kill "$tail_pid"
echo''')
s=s.replace('echo A6L_QEMU_GRAPHICS_EXIT=$?','echo A6L_QEMU_GRAPHICS_EXIT=$probe_exit')
s=s.replace("args=['qemu-system-aarch64'","sock=Path(f'/tmp/v43-{os.getpid()}.sock')\nargs=['qemu-system-aarch64'")
s=s.replace("'-no-reboot','-dtb'","'-no-reboot','-qmp',f'unix:{sock},server=on,wait=off','-dtb'")
s=s.replace('loglevel=1 panic=0','loglevel=1 drm.debug=0x1ff panic=0')
start=s.index("with (OUT/'console.log').open('wb') as f:")
end=s.index("data=(OUT/'console.log').read_bytes();checks=",start)
s=s[:start]+'''sampled=False
with (OUT/'console.log').open('wb') as f:
    proc=subprocess.Popen(args,stdout=f,stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+10
        while not sock.exists() and time.monotonic()<deadline:time.sleep(.05)
        with socket.socket(socket.AF_UNIX,socket.SOCK_STREAM) as conn:
            conn.settimeout(5);conn.connect(str(sock));stream=conn.makefile('rwb',buffering=0);stream.readline()
            def qmp(command,arguments=None):
                req={'execute':command}
                if arguments:req['arguments']=arguments
                stream.write(json.dumps(req).encode()+b'\\n')
                while True:
                    msg=json.loads(stream.readline());assert 'error' not in msg,msg
                    if 'return' in msg:return msg['return']
            qmp('qmp_capabilities');deadline=time.monotonic()+95
            while time.monotonic()<deadline:
                data=(OUT/'console.log').read_bytes()
                if not sampled and b'A6L_PRESENT_VISIBLE' in data:
                    qmp('stop');qmp('pmemsave',{'val':0x9d400000,'size':1080*2340*4+4096,'filename':str(OUT/'framebuffer.bin')});qmp('cont');sampled=True
                if b'A6L_QEMU_GRAPHICS_DONE' in data or b'Kernel panic' in data or proc.poll() is not None:break
                time.sleep(.1)
    finally:
        if proc.poll() is None:proc.kill();proc.wait(timeout=5)
        sock.unlink(missing_ok=True)
''' +s[end:]
anchor="report={'passed':all(checks.values())"
i=s.index(anchor)
s=s[:i]+'''checks['present']=b'A6L_PRESENT_PASS frames=4 buffers=2 commands=checked fences=checked' in data
checks['sampled']=sampled
if sampled:
    frame=(OUT/'framebuffer.bin').read_bytes();(ARCH/'framebuffer.bin').write_bytes(frame)
    colors=[0xffff0000,0xff00ff00,0xff0000ff,0xffffffff,0xff000000]
    bars=b''.join(struct.pack('<I',colors[x*5//1080]) for x in range(1080))
    gray=b''.join(struct.pack('<I',0xff000000|((x*255//1079)*0x010101)) for x in range(1080))
    marked=gray[:600*4]+b'\\xff'*(150*4)+gray[750*4:]
    expected=bars*1170+gray*630+marked*100+gray*440
    checks['full_scanout_pixels']=frame[:len(expected)]==expected
    checks['outside_frame_untouched']=not any(frame[len(expected):])
''' + s[i:]
dest.write_text(s)
print(dest)
