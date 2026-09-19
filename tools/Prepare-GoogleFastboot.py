#!/usr/bin/env python3
"""Extract the verified official fastboot executable and its support files only."""
import hashlib
import json
from pathlib import Path
import zipfile

root = Path(__file__).resolve().parent
archive = root / 'platform-tools-latest-linux.zip'
expected = 'd230f13842f60f782a8645f9c813f8f845bf36089ea7289f28c48f17979313f1'
assert hashlib.sha256(archive.read_bytes()).hexdigest() == expected
target = root / 'google-fastboot-37.0.1-linux'
target.mkdir(exist_ok=False)
names = ['fastboot', 'lib64/libc++.so', 'source.properties', 'NOTICE.txt']
hashes = {}
with zipfile.ZipFile(archive) as package:
    for name in names:
        data = package.read('platform-tools/' + name)
        destination = target / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(data)
        hashes[name] = hashlib.sha256(data).hexdigest()
assert hashes['fastboot'] == 'a686e2c7e8dc9cf4cba0cb8a2eef05f7b2bd682c925abd032fe203215d80b618'
manifest = {'source_url': 'https://dl.google.com/android/repository/platform-tools-latest-linux.zip',
            'archive_sha256': expected, 'files_sha256': hashes, 'revision': '37.0.1'}
(target / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
print(json.dumps(manifest, indent=2))
