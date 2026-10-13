"""Independently verify the twelve Linux stock-rollback readbacks on desktop."""
import hashlib
import json
from pathlib import Path
import WindowsRecoveryReadOnly as baseline

ROOT = Path(__file__).resolve().parents[1]
CAPTURE = ROOT / 'captures/capture-stock-restore-after-windows-v1'


def main():
    session = json.loads((CAPTURE / 'session.json').read_text())
    report = json.loads((CAPTURE / 'edl/report.json').read_text())
    assert session.get('finished_utc') and session.get('worker_exit') == 0
    assert session.get('android_return_verified') and session.get('services_restored')
    assert not session.get('error') and not session.get('cleanup_errors')
    assert report.get('mode') == 'restore-stock' and report.get('readback_verified')
    assert report.get('power_acknowledged') and not report.get('error')
    assert report.get('sahara') == baseline.IDENTITY
    files = []
    for phase, suffix in [('regions', ''), ('after_regions', '-after-write')]:
        assert set(report[phase]) == set(baseline.REGIONS)
        for name, (offset, size, stock_hash) in baseline.REGIONS.items():
            filename = name + suffix + '.bin'
            data = (CAPTURE / 'edl' / filename).read_bytes()
            entry = report[phase][name]
            digest = hashlib.sha256(data).hexdigest()
            assert entry['offset'] == offset and entry['bytes'] == size == len(data)
            assert digest == entry['sha256'], filename
            # This attempt began with zero BCB. All fixed regions except the
            # potentially partial pre-restore recovery must match that baseline.
            if name != 'recovery' or phase == 'after_regions':
                assert digest == stock_hash, filename
            files.append({'file': filename, 'bytes': size, 'sha256': digest})
    before = (CAPTURE / 'edl/recovery.bin').read_bytes()
    stock = (CAPTURE / 'edl/recovery-after-write.bin').read_bytes()
    candidate = (ROOT / 'firmware/extracted/recovery-probe-usb-only-20260916/recovery-diagnostic-unsigned.img').read_bytes()
    comparisons = []
    for label, other in [('stock', stock), ('v12', candidate)]:
        blocks = [i for i in range(0, len(before), 512) if before[i:i+512] != other[i:i+512]]
        comparisons.append({'reference': label, 'different_512_byte_blocks': len(blocks),
                            'first_differing_offset': blocks[0] if blocks else None,
                            'last_differing_offset': blocks[-1] if blocks else None})
    result = {'passed': True, 'files': files, 'pre_restore_recovery_comparison': comparisons,
              'android_return_verified': True, 'services_restored': True,
              'restore_finished_utc': report['finished_utc'], 'session_finished_utc': session['finished_utc']}
    baseline.save(CAPTURE / 'desktop-verification.json', result)
    print(json.dumps(result, indent=2))


if __name__ == '__main__':
    main()
