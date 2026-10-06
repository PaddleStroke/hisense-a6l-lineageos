"""Offline review assertions reproduce bugs, not feature acceptance.
Snapshots production source; fake modem only. No adb, RF, SIM or partition access.
"""
from pathlib import Path
import hashlib,json,os,subprocess,tempfile,re,signal,resource,zipfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1];DEV=ROOT/'device/hisense/a6l'
PLATFORM=Path('/home/a6l/android/a6l-lineage24')
def extract(src,sig):
    a=src.index(sig);b=src.index('{',a)+1;depth=1
    while depth:
        depth+=(src[b]=='{')-(src[b]=='}');b+=1
    return src[a:b]
results=[];hashes={}
def checked(cmd,timeout=180):
    p=subprocess.run([str(x) for x in cmd],capture_output=True,text=True,timeout=timeout,
                     env=dict(os.environ,ASAN_OPTIONS='detect_leaks=0'))
    if p.returncode:raise RuntimeError(f'{cmd}: {p.returncode}\n{p.stdout}\n{p.stderr}')
    return p
with tempfile.TemporaryDirectory(prefix='a6l-review5-') as tmp:
    w=Path(tmp)
    paths=[]
    for directory in ('radio/qmi','radio/tests','radio/hal','radio/minradio'):
        paths.extend(p for p in (DEV/directory).rglob('*') if p.suffix in ('.c','.h','.cc','.cpp'))
    contracts=[
      'hardware/interfaces/radio/aidl/android/hardware/radio/RadioAccessFamily.aidl',
      'hardware/interfaces/radio/aidl/android/hardware/radio/config/IRadioConfig.aidl',
      'hardware/interfaces/radio/aidl/android/hardware/radio/sim/IRadioSim.aidl',
      'hardware/interfaces/radio/aidl/android/hardware/radio/network/IRadioNetwork.aidl',
      'hardware/interfaces/radio/aidl/android/hardware/radio/RadioTechnology.aidl']
    with zipfile.ZipFile(HERE/'reviewed-source.zip','w',zipfile.ZIP_DEFLATED) as archive:
        for p in paths:
            data=p.read_bytes();rel=p.relative_to(DEV)
            hashes[str(p)]=hashlib.sha256(data).hexdigest()
            dst=w/rel;dst.parent.mkdir(parents=True,exist_ok=True);dst.write_bytes(data)
            archive.writestr('device/'+str(rel),data)
        for rel in contracts:
            p=PLATFORM/rel;data=p.read_bytes();hashes[str(p)]=hashlib.sha256(data).hexdigest()
            archive.writestr('platform/'+rel,data)
    (HERE/'source-hashes.json').write_text(json.dumps(hashes,indent=2)+'\n')
    sim=(w/'radio/hal/RadioSimModemConfig.cpp').read_text()
    net=(w/'radio/hal/RadioNetworkData.cpp').read_text()
    base=(w/'radio/minradio/sim/RadioSim.cpp').read_text()
    core=(w/'radio/hal/ModemCore.cpp').read_text()
    methods=[extract(sim,'ScopedAStatus A6lRadioConfig::'+n+'(') for n in ('setNumOfLiveModems','getNumOfLiveModems')]
    methods += [extract(base,'ScopedAStatus RadioSim::'+n+'(') for n in ('enableUiccApplications','areUiccApplicationsEnabled')]
    methods += [extract(core,'ModemCore::PinCheck ModemCore::checkPin('),extract(sim,'uim::Session A6lRadioSim::sessionFor(')]
    methods += [extract(sim,'ScopedAStatus A6lRadioSim::'+n+'(') for n in ('supplyIccPinForApp','supplyIccPukForApp','changeIccPinForApp','setFacilityLockForApp','getFacilityLockForApp')]
    methods += [extract(net,'ScopedAStatus A6lRadioNetwork::'+n+'(') for n in ('setAllowedNetworkTypesBitmap','getAllowedNetworkTypesBitmap','setNetworkSelectionModeManual')]
    (w/'methods.inc').write_text('\n\n'.join(methods))
    raf=(PLATFORM/contracts[0]).read_text();body=extract(raf,'enum RadioAccessFamily')
    tech=extract((PLATFORM/contracts[4]).read_text(),'enum RadioTechnology')
    tech=tech.replace('enum RadioTechnology','enum RadioTechnology : int32_t')+';'
    body=body.replace('enum RadioAccessFamily','enum class RadioAccessFamily : int32_t')+';'
    body=body.replace('RadioTechnology.','RadioTechnology::')
    (w/'raf.inc').write_text(tech+'\n'+body)
    apdu=(w/'radio/minradio/sim/apps/FilesystemApp.cpp').read_text()
    (w/'apdu_method.inc').write_text(extract(apdu,'aidl::IccIoResult FilesystemApp::FilesystemChannel::commandReadBinary('))
    inc=['-I'+str(w),'-I'+str(w/'radio/qmi/include'),'-I'+str(w/'radio/tests'),'-I'+str(w/'radio/tests/hoststub')]
    flags=['-std=c++17','-O1','-g','-pthread','-fsanitize=address,undefined','-fno-omit-frame-pointer','-no-pie']
    qmi=[w/'radio/qmi/src'/f for f in ('message.cc','client.cc','services.cc','datacall.cc','multisim.cc','log.cc','sms.cc','ims.cc','rmnet.cc','qrtr_transport.cc')]
    for name in ('contracts','identity','apdu'):
        print('Building '+name,flush=True)
        sources=[HERE/(name+'.cc')]
        if name!='apdu':sources += qmi+[w/'radio/tests/fake_modem.cc']
        if name=='identity':sources += [w/'radio/hal/ModemCore.cpp']
        binary=w/name
        build=checked(['g++',*flags,*inc,*sources,'-o',binary])
        (HERE/(name+'-build.log')).write_text(build.stdout+build.stderr)
        out=checked([binary])
        (HERE/(name+'.log')).write_text(out.stdout+out.stderr)
        results.append(dict(test=name,output=out.stdout.strip()))
        print(out.stdout,flush=True)
        if name=='apdu':
            def no_core():resource.setrlimit(resource.RLIMIT_CORE,(0,0))
            child=subprocess.run([str(binary),'1'],capture_output=True,text=True,timeout=10,preexec_fn=no_core)
            assert child.returncode==-signal.SIGABRT,(child.returncode,child.stderr)
            (HERE/'apdu-offset1.log').write_text(child.stdout+child.stderr+f'process_returncode={child.returncode} (SIGABRT)\n')
            results.append(dict(test='F47 nonzero APDU read offset',returncode=child.returncode,signal='SIGABRT'))
            print('F47 offset=1 -> SIGABRT reproduced',flush=True)
    changed=[p for p,h in hashes.items() if hashlib.sha256(Path(p).read_bytes()).hexdigest()!=h]
    assert not changed,changed
    (HERE/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    (HERE/'changed-during-run.json').write_text(json.dumps(changed)+'\n')
print('ROUND5_REPRODUCTIONS_PASS',flush=True)
