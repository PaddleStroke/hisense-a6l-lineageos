"""Adapt the single opaque simpleDRM scanout to a BGRA Android client target."""
from pathlib import Path
import hashlib,json,subprocess
root=Path(__file__).resolve().parents[1]
repo=Path('/home/a6l/android/a6l-lineage24/external/drm_hwcomposer')
archive=root/'research/android-present-v43-20260917';archive.mkdir(exist_ok=True)
src=repo/'hwc3/ComposerClient.cpp';before=src.read_text()
assert not (archive/'ComposerClient.cpp.before').exists()
old='''        [parent](auto& bi)
            -> std::shared_ptr<::android::drm_hwcomposer::DrmFbIdHandle> {
          return ImportFb(parent, bi);
        });'''
new='''        [parent, client_target = (&layer == &parent->GetClientLayer())](auto& bi)
            -> std::shared_ptr<::android::drm_hwcomposer::DrmFbIdHandle> {
          // simpleDRM exposes an opaque primary plane. A fully composed BGRA
          // client target has the same color-byte layout as XRGB scanout.
          // Adapt only this private cached view, never application metadata
          // or individual device-composed layers.
          if (client_target && !parent->IsInHeadlessMode() &&
              parent->GetPipe().device->GetName() == "simpledrm" &&
              bi.format == DRM_FORMAT_ARGB8888) {
            bi.format = DRM_FORMAT_XRGB8888;
            bi.blend_mode = BufferBlendMode::kNone;
          }
          return ImportFb(parent, bi);
        });'''
assert before.count(old)==1
after=before.replace(old,new).replace('#include <cinttypes>','#include <drm_fourcc.h>\n#include <cinttypes>')
old2='''  client_layer.SetLayerProperties(properties);
}'''
new2='''  if (!display->IsInHeadlessMode() &&
      display->GetPipe().device->GetName() == "simpledrm") {
    properties.blend_mode = BufferBlendMode::kNone;
  }
  client_layer.SetLayerProperties(properties);
}'''
assert after.count(old2)==1;after=after.replace(old2,new2)
(archive/'ComposerClient.cpp.before').write_text(before);src.write_text(after)
(archive/'ComposerClient.cpp').write_text(after)
patch=subprocess.check_output(['git','diff','--','hwc3/ComposerClient.cpp'],cwd=repo)
(archive/'simpledrm-client-target.patch').write_bytes(patch)
(archive/'source.json').write_text(json.dumps({'base_commit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=repo,text=True).strip(),
    'before_sha256':hashlib.sha256(before.encode()).hexdigest(),'after_sha256':hashlib.sha256(after.encode()).hexdigest(),
    'scope':'Only simpledrm fully composed client-target BGRA view becomes opaque XRGB; all other buffers unchanged'},indent=2)+'\n')
print(patch.decode())
