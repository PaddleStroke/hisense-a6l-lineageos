"""Preserve raw QEMU evidence alongside decoded UTF-8 logs from V51-V53.

The original report's console_sha256 hashes raw QEMU output. Decoding binary
Android log packets replaces invalid UTF-8 and normalizes newlines, so the
human-readable console.log intentionally has a different hash.
"""
from pathlib import Path
import hashlib,json,shutil
root=Path(__file__).resolve().parents[1]
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
for version,attempts in [(51,range(1,7)),(52,range(1,3)),(53,range(1,2))]:
    for attempt in attempts:
        archive=root/f'firmware/extracted/android-framework-v{version}-20260918-r{attempt}'
        raw=Path(f'/home/a6l/kernel/framework-v{version}-r{attempt}/console.log')
        report=json.loads((archive/'report.json').read_text())
        assert digest(raw)==report['console_sha256'],raw
        target=archive/'console.raw.log'
        if target.exists():assert digest(target)==digest(raw)
        else:shutil.copyfile(raw,target)
        record={'raw_file':target.name,'raw_sha256':digest(target),
                'matches_original_report':True,'decoded_file':'console.log',
                'decoded_sha256':digest(archive/'console.log')}
        (archive/'console-integrity.json').write_text(json.dumps(record,indent=2)+'\n')
        print(f'V{version} r{attempt}: raw evidence matches original report')
