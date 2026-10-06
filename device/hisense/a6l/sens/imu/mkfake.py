# fake sysfs + records for a6l_imu host dry-run. mode flat|turn|tilt. SMGR raw NED-ish frame: Android X=y, Y=x, Z=-z
import os, sys, struct, math, random
root, mode = sys.argv[1], sys.argv[2]
names = [('qcom-smgr-accel','accel'),('qcom-smgr-gyro','anglvel'),('qcom-smgr-mag','magn')]
os.makedirs(root+'/dev', exist_ok=True)
N=500
for i,(nm,ty) in enumerate(names):
    d=f'{root}/sys/iio:device{i+3}'; os.makedirs(d+'/scan_elements', exist_ok=True); os.makedirs(d+'/buffer', exist_ok=True)
    open(d+'/name','w').write(nm+'\n'); open(d+f'/in_{ty}_scale','w').write('0.000015258\n'); open(d+f'/in_{ty}_sampling_frequency','w').write('100\n')
    for f in ('enable','length'): open(d+'/buffer/'+f,'w').write('0\n')
    for k,a in enumerate('xyz'):
        p=d+f'/scan_elements/in_{ty}_{a}'; open(p+'_en','w').write('0'); open(p+'_index','w').write(str(k)); open(p+'_type','w').write('le:s32/32>>0\n')
    p=d+'/scan_elements/in_timestamp'; open(p+'_en','w').write('0'); open(p+'_index','w').write('3'); open(p+'_type','w').write('le:u32/64>>0\n')
    out=open(f'{root}/dev/iio:device{i+3}','wb')
    for s in range(N):
        t=s/N  # fraction of phase
        if mode=='turn': yaw=2*math.pi*t; wz=2*math.pi/ (N/100.0)
        else: yaw=0.3; wz=0
        tiltx = 0.9*math.sin(2*math.pi*t) if mode=='tilt' else 0
        tilty = 0.9*math.cos(2*math.pi*t) if mode=='tilt' else 0
        if ty=='accel':  # android (ax,ay,az)
            A=(9.81*math.sin(tiltx), 9.81*math.sin(tilty), 9.81*math.cos(tiltx)*math.cos(tilty))
        elif ty=='anglvel':
            A=(0.001,0.002, wz)   # CCW positive
        else:  # field: north horizontal 21uT, down 42uT; phone azimuth=yaw, hard iron (8,-5,3) uT; in gauss
            bx=-21*math.sin(yaw)+8; by=21*math.cos(yaw)-5; bz=-42+3
            A=(bx/100,by/100,bz/100)
        A=[a+random.gauss(0,0.01 if ty!='magn' else 0.002) for a in A]
        raw=(A[1],A[0],-A[2])  # inverse of map +y+x-z
        out.write(struct.pack('<iiiIQ', *[int(round(v*65536)) for v in raw], 0, s)[:12]+b'\0'*4+struct.pack('<Q',s))
