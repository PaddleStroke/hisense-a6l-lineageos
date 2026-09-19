"""Adapt the established V12 workflow to user USB access and the fixed host helper.

This prepares tools only. It does not run an installer, reboot, or flash.
"""
import hashlib
import json
from pathlib import Path
import py_compile

ROOT = Path(__file__).resolve().parent
NAMES = {
    'Write-LaptopDiagnosticRecovery-v12.py': 'Write-LaptopDiagnosticRecovery-user-v1.py',
    'Inspect-DiagnosticRecovery-v12.py': 'Inspect-DiagnosticRecovery-user-v1.py',
    'Run-LaptopDiagnosticInstall-v12.py': 'Run-LaptopDiagnosticInstall-user-v1.py',
    'Run-LaptopDiagnosticRestore-v12.py': 'Run-LaptopDiagnosticRestore-user-v1.py',
    'Run-LaptopProbeCapture-v12.py': 'Run-LaptopProbeCapture-user-v1.py',
}
PAUSE = '''        host_state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
        if host_state['owned_pause'] is not None:
            raise RuntimeError('An earlier host pause needs inspection/cleanup')
        host_pause_requested = True
        run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'pause'])
'''
RESUME = '''        restored = not host_pause_requested
        if host_pause_requested:
            try:
                run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'resume'])
                host_state = json.loads(run(['/usr/bin/sudo', '-n', '/usr/local/sbin/a6l-host-control', 'status'])['stdout'])
                if host_state['owned_pause'] is not None:
                    raise RuntimeError('Host pause remains after cleanup')
                restored = True
            except Exception as error:
                report.setdefault('cleanup_errors', []).append(str(error))
        report['services_restored'] = restored
'''


def main():
    manifest = {'scope': 'Prepared user-mode V12 trial tools; no physical trial executed', 'sources': {}, 'files': {}}
    for source, target in NAMES.items():
        data = (ROOT / source).read_bytes()
        manifest['sources'][source] = hashlib.sha256(data).hexdigest()
        text = data.decode()
        for a, b in NAMES.items():
            text = text.replace(a, b)
        for a, b in [('capture-diagnostic-install-v12', 'capture-diagnostic-install-user-v1'),
                     ('capture-diagnostic-restore-v12', 'capture-diagnostic-restore-user-v1'),
                     ('capture-probe-serial-v12', 'capture-probe-serial-user-v1')]:
            text = text.replace(a, b)
        if source.startswith('Write-'):
            assert manifest['sources'][source] == 'e71ccc4f15caf5728623d777c618536c2d6c768c95eb5d8ab1df6219376473ad'
            text = text.replace('os.geteuid() != 0', "os.geteuid() != pwd.getpwnam('pierrelouis').pw_uid")
            text = text.replace('This bounded worker requires sudo on the established laptop',
                                'This bounded worker requires the configured user on the established laptop')
            text = text.replace("    account = pwd.getpwnam('pierrelouis')\n", '')
            text = text.replace('    os.chown(args.output, account.pw_uid, account.pw_gid)\n', '')
        if source.startswith('Run-'):
            text = text.replace('    masked = []\n', '    host_pause_requested = False\n')
            a = text.index("        print('Enter the laptop password")
            if 'ProbeCapture' in source:
                b = text.index("        report['phase'] = 'ready_for_physical_boot'", a)
            else:
                b = text.index("        print('Services paused.", a)
            text = text[:a] + PAUSE + text[b:]
            a = text.index('        restored = True\n')
            b = text.index("        report['services_restored'] = restored\n", a) + len("        report['services_restored'] = restored\n")
            text = text[:a] + RESUME + text[b:]
            text = text.replace("['/usr/bin/sudo', '-n', '/usr/bin/timeout'", "['/usr/bin/timeout'")
            assert "['/usr/bin/sudo', '-v']" not in text and 'masked.append' not in text
            assert "['/usr/bin/sudo', '-n', '/usr/bin/python" not in text
            assert "['/usr/bin/sudo', '-n', '/usr/bin/systemctl'" not in text
        output = ROOT / target
        output.write_text(text, newline='\n')
        py_compile.compile(str(output), doraise=True)
        manifest['files'][target] = hashlib.sha256(output.read_bytes()).hexdigest()
    assert hashlib.sha256((ROOT / 'DiagnosticRecoveryProtocolV11.py').read_bytes()).hexdigest() == 'ecb5e29933930b9d536949c91cb2704d82b50353249098fc60a3d04d52113690'
    manifest['protocol_unchanged'] = True
    (ROOT / 'a6l-user-tools-v1-manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(json.dumps(manifest, indent=2))


if __name__ == '__main__':
    main()
