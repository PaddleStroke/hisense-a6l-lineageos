import sys,struct,re
d=open(sys.argv[1],'rb').read()
i=d.find(b'MCFG')
while i>=0:
    fmt,ctype,n,car=struct.unpack_from('<HHIH',d,i+4)
    if n<5000 and fmt<10: break
    i=d.find(b'MCFG',i+1)
print('MCFG at',hex(i),'fmt',fmt,'type',ctype,'items',n,'carrier',car)
# efs paths
for m in re.finditer(rb'(/[\x20-\x7e]{3,120})\x00',d):
    p=m.group(1).decode()
    if not (p.startswith('/nv') or p.startswith('/ims') or p.startswith('/sd') or p.startswith('/data') or p.startswith('/Data') or p.startswith('/efs')): continue
    e=m.end()
    if e+4>len(d): continue
    t,l=struct.unpack_from('<HH',d,e)
    val=d[e+4:e+4+l] if t==2 and l<100000 else b''
    show=val[:24].hex() if val and not all(32<=c<127 or c in (9,10,13) for c in val[:40]) else val[:60]
    print(f'{m.start():#07x} {p} t={t} len={l} {show}')
