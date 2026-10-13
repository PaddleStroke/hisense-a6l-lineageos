"""Release accidental simpleDRM master ownership in Android's buffer allocator."""
from pathlib import Path
import hashlib, json, subprocess

root = Path(__file__).resolve().parents[1]
repo = Path('/home/a6l/android/a6l-lineage24/external/minigbm')
source = repo/'cros_gralloc/cros_gralloc_driver.cc'
archive = root/'research/android-graphics-v42-20260917'
before = source.read_text()
old = '\tdrv = drv_create(fd, NULL);\n\tif (!drv)\n\t\tclose(fd);'
new = '''\tdrv = drv_create(fd, NULL);
\t/* simpledrm has no render node. Opening card0 can accidentally take
\t * modesetting ownership, which belongs to the display composer.
\t * Match the ownership release in minigbm_helpers.c, scoped to our
\t * newly supported simpledrm allocation path.
\t */
\tif (drv && !strcmp(drv_get_name(drv), "simpledrm") && drmIsMaster(fd)) {
\t\tif (drmDropMaster(fd)) {
\t\t\tALOGE("Failed to release simpledrm DRM master: %s", strerror(errno));
\t\t\tdrv_destroy(drv);
\t\t\tdrv = nullptr;
\t\t}
\t}
\tif (!drv)
\t\tclose(fd);'''
assert before.count(old) == 1
assert not (archive/'cros_gralloc_driver.cc.before').exists()
(archive/'cros_gralloc_driver.cc.before').write_text(before)
after = before.replace(old,new)
source.write_text(after)
(archive/'cros_gralloc_driver.cc').write_text(after)
patch = subprocess.check_output(['git','diff','--','cros_gralloc/cros_gralloc_driver.cc'],cwd=repo)
(archive/'simpledrm-master.patch').write_bytes(patch)
(archive/'simpledrm-master.json').write_text(json.dumps({
    'before_sha256':hashlib.sha256(before.encode()).hexdigest(),
    'after_sha256':hashlib.sha256(after.encode()).hexdigest(),
    'scope':'Only successful Android simpledrm allocations that accidentally acquired DRM master',
    'reference':'minigbm_helpers.c primary-node drmDropMaster pattern',
},indent=2)+'\n')
print(patch.decode())
