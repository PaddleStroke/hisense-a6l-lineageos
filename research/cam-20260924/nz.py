import sys,struct,subprocess,re
fn=sys.argv[1]; f=open(fn,'rb').read()
out=subprocess.check_output(['readelf','-W','-S',fn]).decode()
m=re.search(r'\.data\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',out)
va,off,sz=[int(x,16) for x in m.groups()]
d=f[off:off+sz]
skip=[(int(a,16),int(a,16)+24000) for a in sys.argv[2:]]
line=[]
for i in range(0,sz-3,4):
    if any(a<=i<b for a,b in skip): continue
    w,=struct.unpack_from('<I',d,i)
    if w: print('%06x %08x %d'%(i,w,w))
