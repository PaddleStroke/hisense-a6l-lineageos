import sys,struct,subprocess,re
fn=sys.argv[1]; f=open(fn,'rb').read()
out=subprocess.check_output(['readelf','-W','-S',fn]).decode()
m=re.search(r'\.data\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',out)
va,off,sz=[int(x,16) for x in m.groups()]
d=f[off:off+sz]
o=int(sys.argv[2],16); n=int(sys.argv[3],16)
for i in range(o,o+n,16):
    w=struct.unpack_from('<4I',d,i)
    print('%06x: '%i+' '.join('%08x(%d)'%(x,x) for x in w))
