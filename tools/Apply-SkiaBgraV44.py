"""Backport the reviewed upstream Android-framework BGRA buffer import fix."""
import base64,json,subprocess,urllib.request
from pathlib import Path
commit='ed4e2bf5398b997c9beb87e832a2f9a5cdc71485'
url=f'https://skia.googlesource.com/skia/+/{commit}^!/?format=TEXT'
archive=Path(__file__).resolve().parents[1]/'research/android-surface-v44-20260917/skia-bgra-upstream'
archive.mkdir(exist_ok=False)
repo=Path('/home/a6l/android/a6l-lineage24/external/skia')
patch=archive/'upstream.patch'
patch.write_bytes(base64.b64decode(urllib.request.urlopen(url,timeout=30).read()))
subprocess.run(['git','apply','--check',str(patch)],cwd=repo,check=True)
subprocess.run(['git','apply',str(patch)],cwd=repo,check=True)
(archive/'provenance.json').write_text(json.dumps({'commit':commit,'source':url,'applied':True},indent=2)+'\n')
print(patch.read_text())
