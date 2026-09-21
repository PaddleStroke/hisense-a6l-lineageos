import re,sys
ours={};sec=None
base={'INTF2':0xc96c000,'PHYALL':0xc996400,'CTRL':0xc996000,'MDPTOP':0xc901000}
for l in open('ours-active-full.txt'):
    l=l.strip()
    if l in base:sec=l;continue
    m=re.match(r'([0-9a-f]{8}): ([0-9a-f]{8})',l)
    if m:ours[int(m.group(1),16)]=m.group(2)
sb={'CTRL':0xc996000,'PHY':0xc996400,'MDP':0xc900000}
stock={};cur=None;want=sys.argv[2] if len(sys.argv)>2 else '1'
on=False
for l in open(sys.argv[1]):
    if l.startswith('==='):on=l.split()[2]==want;continue
    if not on:continue
    m=re.match(r'--- (\w+) (\w+)',l)
    if m:cur=sb[m.group(1)];continue
    m=re.match(r'0x(\w+): (.*)',l)
    if m:
        for i,v in enumerate(m.group(2).split()):stock[cur+int(m.group(1),16)+4*i]=v
for a in sorted(stock):
    if a in ours and ours[a]!=stock[a]:print(hex(a),'stock',stock[a],'ours',ours[a])
print('compared',len([a for a in stock if a in ours]))
