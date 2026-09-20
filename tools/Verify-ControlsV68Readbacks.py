"""Independently verify all twelve copied V68 recovery readbacks on desktop."""
import argparse
import hashlib
import json
from pathlib import Path
import WindowsRecoveryReadOnly as baseline
import RecoveryTransitionV68 as transition

ROOT = Path(__file__).resolve().parents[1]
CANDIDATE = '2448b101eb780b1630cc6fd7181315da50211de2504b08cd6dad9e34d44a7520'

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('mode', choices=['install', 'restore'])
    parser.add_argument('--readback-only', action='store_true', help='Verify bytes while the coordinator still awaits manual Android return')
    args = parser.parse_args()
    capture = ROOT / f'captures/capture-diagnostic-{args.mode}-user-v68'
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
                expected = ({CANDIDATE} if phase == 'after_regions' else set(transition.RECOVERIES)) if args.mode == 'install' else ({stock_hash} if phase == 'after_regions' else {CANDIDATE})
                assert digest in expected, filename
            elif name == 'misc-bcb':
                assert data == (capture / 'edl/misc-bcb.bin').read_bytes()
                assert digest in (stock_hash, '8ac9baa0ce2f52dda6debef8f8ffb4fcd8e85d44bf05882dbcb552dd06b46881'), 'Unexpected boot control message'
            else:
                assert digest == stock_hash, filename
            files.append({'file': filename, 'bytes': size, 'sha256': digest})
    if args.mode == 'install':
        expected_transition = transition.verify_transition(report['regions']['recovery']['sha256'], (capture / 'edl/misc-bcb.bin').read_bytes(), (capture / 'edl/devinfo.bin').read_bytes())
        assert report['transition'] == expected_transition
    result = {'passed': True, 'mode': args.mode, 'files': files,
              'session_finished_utc': session.get('finished_utc'),
              'services_restored': session.get('services_restored'), 'readback_only': args.readback_only}
    baseline.save(capture / ('desktop-readback-verification.json' if args.readback_only else 'desktop-verification.json'), result)
    print(json.dumps({'passed': True, 'mode': args.mode, 'files_verified': len(files)}, indent=2))

if __name__ == '__main__':
    main()
