"""Archive preparation evidence without starting any phone operation."""
import hashlib,json,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];T=ROOT/'tools';OUT=ROOT/'research/combined-controls-v45-20260917'
checks={}
for name in ['Test-ControlsV45Guards.py','Test-TouchCapture.py','Test-RecoveryTransitionV45.py','Test-DiagnosticRecoveryProtocolV45.py']:
    p=subprocess.run([sys.executable,str(T/name)],capture_output=True,text=True,timeout=30)
    (OUT/(name+'.log')).write_text(p.stdout+p.stderr)
    assert p.returncode==0,name
    checks[name]={'passed':True,'source_sha256':hashlib.sha256((T/name).read_bytes()).hexdigest()}
checks['stage']=json.loads((OUT/'laptop-stage-verification.json').read_text())
assert checks['stage']['passed']
checks['module_qemu']=json.loads((ROOT/'firmware/extracted/controls-v45-prep-20260917/qemu-module-report.json').read_text())['passed']
checks['captured_abl']=json.loads((ROOT/'firmware/extracted/recovery-controls-v45-20260917/captured-abl-validation.json').read_text())['passed']
checks['eink_abi_qemu']=json.loads((ROOT/'firmware/extracted/eink-abi-20260917/qemu-report.json').read_text())['passed']
assert checks['module_qemu'] and checks['captured_abl'] and checks['eink_abi_qemu']
report={'prepared':True,'staged':True,'installed':False,'physical_tests_started':False,'candidate_sha256':'aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14','checks':checks}
(OUT/'readiness.json').write_text(json.dumps(report,indent=2)+'\n')
resume=ROOT/'docs/resume-next-session.md'
note='''V45 COMBINED PREPARATION COMPLETE 2026-09-17, user away until tomorrow. No phone writes/reboots/actuation this turn; stock Android boot_completed=1 and host owned_pause=null verified. Current recovery remains V38 (54b0d7b2502b71d8493e4669f2081e46d9ec4ba7a23c275373d735134977cbbb).
New V45 recovery aae86f0da6f15a0c1f6e425e88806edb165289c459462b232f0b8773f5e4ac14 preserves V38 executable kernel/RAM/cmdline/recovery selection, adds 57 reviewed DT property changes for front touch/buttons/WLED/haptic/FG and chosen marker v45; Android ro.a6l.ramdiag intentionally stays v38. Package/roundtrip/captured ABL PASSED. 3 modules QEMU9/9, guards7, touchparser10, transition4, protocol6 PASS. Full V44 real SF/ANGLE/SwiftShader payload12/12QEMU reused inside V45 via Run-AndroidSurfaceOnV45.py (not old V44 launcher). Phone graphics pipeline still untested; V43 bars were validated. Stage successful: 30pins, 9V38+12V45 tools,5control+146graphics payloads and candidate verified; offline InspectV45 PASSED noUSB. Laptop root /home/pierrelouis/A6L-usb-20260915. All V45 launch/install/capture tools UNUSED. No active sessions/agents.
TOMORROW after user returns: fresh stock/host/pins -> Launch-ControlsV45Install.py ONCE -> copy12readbacks and Verify-ControlsV45Readbacks.py install -> ask Power Android -> Launch-ControlsV45.py ONCE -> confirm fastboot and ask Recovery (NOFILM) -> short secureADB readiness -> Run-ControlsV45.py stages baseline,surface,touch-load,touch-events,keys,brightness,vibration,battery, each separately with user prompts for brief physical actions. Same boot, separate reports, no automatic continuation after failure. 15s touch,15s keys,4s brightness,100ms haptic. Coordinator waits900s for stock return after readiness then resumes host services. Keep stages within window; finish Power stock and cleanup. WLED probes at boot; other3modules stage-loaded. FG is NOT passive initialization, charger remainsdisabled. No modem/audio/BT/wifi activation bundle. See docs/combined-controls-v45-20260917.md and research/combined-controls-v45-20260917/readiness.json.
EINK OFFLINE ADVANCE: new a6l_eink_abi_probe built; stock libtcon_eink.so loads with modern V44 Android library closure; disassembled ReportEinkSWTconLibVersion verified void(uint32_t*,uint32_t*), returns2.2 and guards intact. QEMU4/4PASS, no Init/conversion/panel calls. firmware/extracted/eink-abi-20260917 and docs/eink-abi-preparation-20260917.md. Separate SPI waveform/calibration still NOT backed up; full eInk support unproven.

'''
assert not resume.read_text().startswith('V45 COMBINED PREPARATION COMPLETE')
resume.write_text(note+resume.read_text())
print('PREPARATION_RECORDED; all launchers remain unused')
