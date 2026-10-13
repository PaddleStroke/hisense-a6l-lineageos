from pathlib import Path
root=Path(__file__).resolve().parents[1]/'tools'
for name in ['Run-AndroidPresentV43.py','Run-LaptopPresentCapture-v43.py','Verify-PresentV43Stage.py','Launch-PresentV43.py']:
    assert not (root/name).exists()
runner=(root/'Run-AndroidGraphicsV42.py').read_text().replace('graphics-user-v42','present-user-v43').replace('android-graphics-v42','android-present-v43').replace('a6l-v42','a6l-v43')
runner=runner.replace('def save():','printk=None\n    def save():')
runner=runner.replace("        shell('/system/bin/toybox','insmod'",'''        printk=shell('cat','/proc/sys/kernel/printk')['stdout'].strip()
        assert len(printk.split())==4 and all(v.isdigit() for v in printk.split())
        shell('sh','-c',"'echo 1 > /proc/sys/kernel/printk'")
        shell('/system/bin/toybox','insmod' ''').replace("'insmod' ,","'insmod',")
runner=runner.replace("        after=shell('cat','/proc/mounts')",'''        report['presentation_passed']='A6L_PRESENT_PASS frames=4 buffers=2 commands=checked fences=checked' in client
        after=shell('cat','/proc/mounts')''')
runner=runner.replace("report['passed']=report['graphics_passed']", "report['passed']=report['presentation_passed'] and report['graphics_passed']")
runner=runner.replace("    finally:report['finished_utc']=datetime.now(timezone.utc).isoformat();save()",'''    finally:
        if printk is not None:
            try:
                shell('sh','-c',"'echo "+printk+" > /proc/sys/kernel/printk'")
                report['console_loglevel_restored']=shell('cat','/proc/sys/kernel/printk')['stdout'].split()==printk.split()
                report['passed']=report.get('passed',False) and report['console_loglevel_restored']
            except Exception as e:report['cleanup_error']=repr(e);report['passed']=False
        report['finished_utc']=datetime.now(timezone.utc).isoformat();save()''')
(root/'Run-AndroidPresentV43.py').write_text(runner)
coordinator=(root/'Run-LaptopGraphicsCapture-v42.py').read_text().replace('graphics-user-v42','present-user-v43').replace('V42','V43').replace('A6L-v42','A6L-v43')
# Leave the presentation launch manual so the user can look at the screen first.
(root/'Run-LaptopPresentCapture-v43.py').write_text(coordinator)
verify=(root/'Verify-GraphicsV42Stage.py').read_text().replace('graphics-v42','present-v43')
(root/'Verify-PresentV43Stage.py').write_text(verify)
launch=(root/'Launch-GraphicsV42.py').read_text().replace('graphics-user-v42','present-user-v43').replace('graphics-v42','present-v43').replace('A6L-v42','A6L-v43').replace('Verify-GraphicsV42Stage','Verify-PresentV43Stage').replace('Run-LaptopGraphicsCapture-v42','Run-LaptopPresentCapture-v43').replace('V42','V43')
(root/'Launch-PresentV43.py').write_text(launch)
for name in ['Run-AndroidPresentV43.py','Run-LaptopPresentCapture-v43.py','Verify-PresentV43Stage.py','Launch-PresentV43.py']:
    compile((root/name).read_text(),name,'exec')
print('V43 runners prepared and parsed; none staged or executed')
