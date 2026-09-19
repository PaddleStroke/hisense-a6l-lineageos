"""Allow the local RAM diagnostic to use BoringSSL's non-FIPS static variant."""
import difflib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
p = Path('/home/a6l/android/a6l-lineage24/external/boringssl/Android.bp')
s = p.read_text()
anchor = '    name: "libcrypto_static",\n    visibility: [\n'
assert s.count(anchor) == 1
replacement = anchor + '        "//device/hisense/a6l", // RAM-only bring-up SHA-256 checks.\n'
assert '//device/hisense/a6l' not in s
after = s.replace(anchor, replacement)
(ROOT / 'firmware/extracted/storage-read-v37-20260917/boringssl-visibility.patch').write_text(
    ''.join(difflib.unified_diff(s.splitlines(True), after.splitlines(True),
                               fromfile='a/external/boringssl/Android.bp', tofile='b/external/boringssl/Android.bp')))
p.write_text(after)
print('Scoped static BoringSSL visibility enabled for A6L RAM diagnostic.')
