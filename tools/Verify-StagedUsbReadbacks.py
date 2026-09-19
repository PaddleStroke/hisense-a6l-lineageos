"""Independently verify all twelve copied V13 recovery readbacks on desktop."""
import argparse
import hashlib
import json
from pathlib import Path
import WindowsRecoveryReadOnly as baseline

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = 'f302bc49910f0c1595590b5dc7ac062f86681ff9b26233142e93cb8e501c661e'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['install', 'restore'])
    parser.add_argument('--readback-only', action='store_true', help='Verify bytes while the coordinator still awaits manual Android return')
    args = parser.parse_args()
    capture = ROOT / f'captures/capture-diagnostic-{args.mode}-user-v13'
    session = json.loads((capture / 'session.json').read_text())
    report = json.loads((capture / 'edl/report.json').read_text())
    assert session.get('worker_exit') == 0
    assert not session.get('error') and not session.get('cleanup_errors')
    if not args.readback_only:
        assert session.get('finished_utc') and session.get('services_restored')
        assert session.get('edl_disconnected_after_poweroff' if args.mode == 'install' else 'android_return_verified')
    assert report.get('mode') == ('install-diagnostic' if args.mode == 'install' else 'restore-stock')
    assert report.get('readback_verified') and report.get('power_acknowledged') and not report.get('error')
    assert report.get('sahara') == baseline.IDENTITY
    assert report.get('unprivileged_edl_access')
    files = []
    for phase, suffix in [('regions', ''), ('after_regions', '-after-write')]:
        assert set(report[phase]) == set(baseline.REGIONS)
        for name, (offset, size, stock_hash) in baseline.REGIONS.items():
            filename = name + suffix + '.bin'
            data = (capture / 'edl' / filename).read_bytes()
            entry = report[phase][name]
            digest = hashlib.sha256(data).hexdigest()
            assert entry['offset'] == offset and entry['bytes'] == size == len(data)
            assert digest == entry['sha256'], filename
            if name == 'recovery':
                expected = CANDIDATE if (args.mode == 'install') == (phase == 'after_regions') else stock_hash
                assert digest == expected, filename
            elif name == 'misc-bcb' and args.mode == 'restore':
                assert data == (capture / 'edl/misc-bcb.bin').read_bytes()
                assert digest in (stock_hash, '8ac9baa0ce2f52dda6debef8f8ffb4fcd8e85d44bf05882dbcb552dd06b46881'), 'Unexpected boot control message'
            else:
                assert digest == stock_hash, filename
            files.append({'file': filename, 'bytes': size, 'sha256': digest})
    result = {'passed': True, 'mode': args.mode, 'files': files,
              'session_finished_utc': session.get('finished_utc'),
              'services_restored': session.get('services_restored'), 'readback_only': args.readback_only}
    baseline.save(capture / ('desktop-readback-verification.json' if args.readback_only else 'desktop-verification.json'), result)
    print(json.dumps({'passed': True, 'mode': args.mode, 'files_verified': len(files)}, indent=2))

if __name__ == '__main__':
    main()
