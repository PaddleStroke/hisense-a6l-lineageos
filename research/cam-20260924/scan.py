import sys,struct
f=open(sys.argv[1],'rb').read()
# .data file offset/vaddr from readelf
import subprocess,re
out=subprocess.check_output(['readelf','-W','-S',sys.argv[1]]).decode()
m=re.search(r'\.data\s+PROGBITS\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)',out)
va,off,sz=[int(x,16) for x in m.groups()]
d=f[off:off+sz]
# find runs of {u16 addr,u16 data,u32 delay} with addr!=0 and delay<1000
i=0;runs=[]
while i+8<=len(d):
    j=i;n=0
    while j+8<=len(d):
        a,v,dl=struct.unpack_from('<HHI',d,j)
        if a==0 or dl>=1000 or v>0xffff: break
        n+=1;j+=8
    if n>=6:
        runs.append((i,n)); i=j
    else: i+=8 if n else 4
for o,n in runs:
    a0=struct.unpack_from('<H',d,o)[0]
    print(hex(va+o),hex(o),n,hex(a0))
