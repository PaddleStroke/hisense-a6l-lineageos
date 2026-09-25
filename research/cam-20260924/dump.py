import sys,struct,subprocess,re
fn=sys.argv[1]; f=open(fn,'rb').read()
out=subprocess.check_output(['readelf','-W','-S',fn]).decode()
m=re.search(r'\.data\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',out)
va,off,sz=[int(x,16) for x in m.groups()]
d=f[off:off+sz]
N=3000
def arr(o):
    size,=struct.unpack_from('<H',d,o+N*8)
    tail=struct.unpack_from('<HHIII',d,o+N*8)
    return size,tail
init=int(sys.argv[2],16)
for k in range(8):
    o=init+k*(N*8+16)
    if o+N*8+16>len(d): break
    print('init',k,hex(o),arr(o))
res=int(sys.argv[3],16)
for k in range(10):
    o=res+k*(N*8+16)
    if o+N*8+16>len(d): break
    print('res',k,hex(o),arr(o), struct.unpack_from('<HHI',d,o))
