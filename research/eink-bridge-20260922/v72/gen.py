import struct,sys
src=sys.argv[1]
b=open(src,'rb').read()
assert b[:8]==b'A6LEPD1\n'
W,H,N=struct.unpack_from('<3I',b,8)
pos=20; frames=[]
for n in range(N):
    runs,=struct.unpack_from('<I',b,pos); pos+=4
    rl=[struct.unpack_from('<2I',b,pos+8*i) for i in range(runs)]; pos+=8*runs
    frames.append(rl)
def write(name,fn):
    out=bytearray(b'A6LEPD1\n'+struct.pack('<3I',W,H,N))
    for rl in frames:
        nr=fn(rl)
        # merge
        m=[]
        for c,v in nr:
            if m and m[-1][1]==v: m[-1][0]+=c
            else: m.append([c,v])
        out+=struct.pack('<I',len(m))
        for c,v in m: out+=struct.pack('<2I',c,v)
    open(name,'wb').write(out); print(name,len(out))
def solid(code):
    return lambda rl:[(c,(v&~0xff)|code if v&0xff else v) for c,v in rl]
def bars(rl):
    res=[];p=0
    for c,v in rl:
        while c:
            x=p%W; band_end=(x//16+1)*16; take=min(c,band_end-x)
            if v&0xff: code=0x55 if ((x%192)//16)%2==0 else 0xaa; res.append((take,(v&~0xff)|code))
            else: res.append((take,v))
            p+=take; c-=take
    return res
write('black55.a6lepd',solid(0x55)); write('whiteaa.a6lepd',solid(0xaa)); write('bars.a6lepd',bars)
