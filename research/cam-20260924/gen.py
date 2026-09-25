# Generate cci_reg_sequence tables from stock sensor-lib dumps.
import sys
def load(fn):
    out=[]
    for l in open(fn):
        a,v,d=l.split(); out.append((int(a,16),int(v,16),int(d)))
    return out
def emit(name, rows, width):
    """split into segments at delays; returns C text + list of (segname, delay_after_us)"""
    segs=[]; cur=[]; txt=''
    for a,v,d in rows:
        cur.append((a,v))
        if d:
            segs.append((cur,d)); cur=[]
    if cur: segs.append((cur,0))
    names=[]
    for i,(s,d) in enumerate(segs):
        n='%s_%d'%(name,i) if len(segs)>1 else name
        txt+='static const struct cci_reg_sequence %s[] = {\n'%n
        for a,v in s:
            txt+='\t{ CCI_REG%d(0x%04x), 0x%0*x },\n'%(width,a,width//4,v)
        txt+='};\n\n'
        names.append((n,d))
    return txt,names

def build(cfg):
    T=open('template.c').read()
    tables=''
    init_txt,init_names=emit('sns_init_regs',load(cfg['init']),cfg['w'])
    tables+=init_txt
    tables+='static const struct sns_seg sns_init[] = {\n'+''.join('\t{ %s, ARRAY_SIZE(%s), %d },\n'%(n,n,d) for n,d in init_names)+'};\n\n'
    modes=''
    for m in cfg['modes']:
        t,names=emit('sns_mode_%s'%m['tag'],load(m['file']),cfg['w'])
        tables+=t
        tables+='static const struct sns_seg sns_mode_%s_segs[] = {\n'%m['tag']+''.join('\t{ %s, ARRAY_SIZE(%s), %d },\n'%(n,n,d) for n,d in names)+'};\n\n'
        modes+=('\t{\n\t\t/* %s */\n\t\t.width = %d, .height = %d, .hts = %d, .vts = %d,\n'
                '\t\t.exposure = %d, .exposure_margin = %d,\n\t\t.link_freq_index = %d, .pixel_rate = %dULL,\n'
                '\t\t.segs = sns_mode_%s_segs, .num_segs = ARRAY_SIZE(sns_mode_%s_segs),\n\t},\n')%(
                m['comment'],m['w'],m['h'],m['hts'],m['vts'],m['vts']-cfg['margin']-8,cfg['margin'],m['lf'],m['pr'],m['tag'],m['tag'])
    rep={'@TABLES@':tables,'@MODES@':modes.rstrip('\n'),'@LINKFREQS@':', '.join('%dLL'%f for f in cfg['lfs']),
         '@FMTS@':', '.join(cfg['fmts'])}
    for k,v in cfg['subs'].items(): rep['@%s@'%k]=str(v)
    for k,v in rep.items(): T=T.replace(k,v)
    assert '@' not in T.replace('@NAME@',''), [l for l in T.splitlines() if '@' in l][:5]
    open(cfg['out'],'w').write(T)

TB='tables/'
build(dict(out='drv/imx576.c', init=TB+'imx576_init.txt', w=8, margin=61,
  lfs=[255000000,1011000000,627000000],
  fmts=['MEDIA_BUS_FMT_SRGGB10_1X10','MEDIA_BUS_FMT_SGRBG10_1X10','MEDIA_BUS_FMT_SGBRG10_1X10','MEDIA_BUS_FMT_SBGGR10_1X10'],
  modes=[dict(tag='2880x2156',file=TB+'imx576_res1.txt',w=2880,h=2156,hts=5544,vts=2215,lf=0,pr=368398800,comment='stock res1: 2x2 binned 30 fps, OP 510 Mbps/lane'),
         dict(tag='5760x4312',file=TB+'imx576_res0.txt',w=5760,h=4312,hts=6144,vts=4401,lf=1,pr=811238400,comment='stock res0: full 30 fps, OP 2022 Mbps/lane (= upstream v4 1011 MHz)'),
         dict(tag='2880x1620',file=TB+'imx576_res3.txt',w=2880,h=1620,hts=5544,vts=1678,lf=2,pr=837254880,comment='stock res3: binned 16:9 90 fps, OP 1254 Mbps/lane')],
  subs=dict(NAME='Sony IMX576',LIB='imx576_hmct',IDREG='0x0016',IDVAL='0x0576',EXPMIN=8,GMIN=0,GMAX=960,GDEF=0,
            NATIVE_W=5760,NATIVE_H=4312,COMPAT='sony,imx576',MODNAME='imx576_a6l',
            POWERSEQ='RESET low 1ms, VANA 1ms, MCLK 24MHz 2ms, RESET high 3ms')))
build(dict(out='drv/s5k3t1.c', init=TB+'s5k3t1_init.txt', w=16, margin=8,
  lfs=[282000000],
  fmts=['MEDIA_BUS_FMT_SGRBG10_1X10','MEDIA_BUS_FMT_SRGGB10_1X10','MEDIA_BUS_FMT_SBGGR10_1X10','MEDIA_BUS_FMT_SGBRG10_1X10'],
  modes=[dict(tag='2304x1728',file=TB+'s5k3t1_res0.txt',w=2304,h=1728,hts=13104,vts=2034,lf=0,pr=799626240,comment='stock res0 (only mode): 2x2 binned 30 fps, OP 564 Mbps/lane')],
  subs=dict(NAME='Samsung S5K3T1',LIB='s5k3t1sp_hmct',IDREG='0x0000',IDVAL='0x3141',EXPMIN=4,GMIN=32,GMAX=512,GDEF=32,
            NATIVE_W=5184,NATIVE_H=3880,COMPAT='samsung,s5k3t1',MODNAME='s5k3t1',
            POWERSEQ='RESET low 1ms, VANA 5ms, RESET high 5ms, MCLK 24MHz 2ms')))
print('generated')
