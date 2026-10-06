import struct,sys,subprocess,glob,os
def segs(f):
    d=open(f,'rb').read()
    if d[:4]!=b'\x7fELF' or d[4]!=2: return None,None
    phoff=struct.unpack_from('<Q',d,0x20)[0]; phn=struct.unpack_from('<H',d,0x38)[0]
    S=[]
    for i in range(phn):
        t,fl,off,va,pa,fs,ms,al=struct.unpack_from('<IIQQQQQQ',d,phoff+i*56)
        if t==1: S.append((va,off,fs))
    return d,S
def v2o(S,v):
    for va,off,fs in S:
        if va<=v<va+fs: return off+v-va
res={}
for f in sorted(glob.glob(sys.argv[1]+'/*.so'))+sorted(glob.glob(sys.argv[2]+'/*')):
    try: out=subprocess.run(['nm','-D','--defined-only',f],capture_output=True,text=True).stdout
    except Exception: continue
    syms=[l.split() for l in out.splitlines() if 'qmi_idl_service_object' in l]
    if not syms: continue
    d,S=segs(f)
    if not d: continue
    for s in syms:
        if len(s)<3: continue
        o=v2o(S,int(s[0],16))
        if o is None: continue
        lv,iv,sid,mx=struct.unpack_from('<IIII',d,o)
        res.setdefault(sid,set()).add((s[2],os.path.basename(f)))
for sid in sorted(res):
    names=sorted({n for n,_ in res[sid]})
    print(sid,hex(sid),names[:3],sorted({b for _,b in res[sid]})[:3])
