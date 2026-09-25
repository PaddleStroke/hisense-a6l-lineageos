import sys,struct,subprocess,re
fn=sys.argv[1]; f=open(fn,'rb').read()
out=subprocess.check_output(['readelf','-W','-S',fn]).decode()
m=re.search(r'\.data\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',out)
va,off,sz=[int(x,16) for x in m.groups()]
d=f[off:off+sz]
o=int(sys.argv[2],16); n=int(sys.argv[3]) if len(sys.argv)>3 else 3000
for k in range(n):
    a,v,dl=struct.unpack_from('<HHI',d,o+8*k)
    if a==0 and v==0 and dl==0 and n==3000: break
    print('0x%04x 0x%04x %d'%(a,v,dl))
