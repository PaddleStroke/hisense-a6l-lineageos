#!/usr/bin/env bash
# Release prebuilts for BoardConfig-release.mk (release-prep, 27 Sep 2026). Called by tools/rom-v2-pipeline.sh prep when
# A6L_RELEASE=1, AFTER stage-rom-v2-prebuilts.sh (which recreates device/hisense/a6l/rom in the tree) and the V75 DT build.
# usage: stage-release-prebuilts.sh <dt-dir with rom-v2.dtb>
# Output (tree only): device/hisense/a6l/rom/release/prebuilt/{Image.gz-dtb, dtbo.img, recovery-modules/*.ko}
set -euo pipefail
DT=$1
R=/mnt/c/Users/Pierre/Desktop/A6L; L=/home/a6l/android/a6l-lineage24
P=$L/device/hisense/a6l/rom/release/prebuilt; rm -rf $P; mkdir -p $P/recovery-modules
V67=$R/firmware/extracted/phone-kernel-v67-candidate-20260919; V74=$R/firmware/extracted/recovery-v74-candidate-20260923
[ "$(sha256sum < $V67/Image | cut -c1-64)" = 0d7d2eb6692b0439a43306ce6f8e26b07334fe21897b8774f99317921fd5acf7 ] || { echo "V67 Image hash mismatch"; exit 1; }
[ "$(fdtget $DT/rom-v2.dtb /chosen hisense,a6l-image)" = rom-v2 ] || { echo "not a rom-v2 DTB: $DT/rom-v2.dtb"; exit 1; }
python3 - $V67/Image $DT/rom-v2.dtb $P/Image.gz-dtb <<'PY'
import gzip, sys
k = open(sys.argv[1], 'rb').read(); d = open(sys.argv[2], 'rb').read()
open(sys.argv[3], 'wb').write(gzip.compress(k, mtime=0) + d)   # = Prepare-RomV2Boot.py payload
PY
cp $V74/recovery-dtbo.img $P/dtbo.img            # the ABL board-id selection table (425 B), embedded as recovery_dtbo
# recovery ramdisk modules: eMMC + splash-framebuffer DRM for minui (+ touch if its deps are built in)
cp $R/firmware/extracted/rom-v1-20260924/stage/ramdisk-modules/sdhci-msm.ko $P/recovery-modules/
cp $V67/a6l_simplefb.ko $P/recovery-modules/
E=$L/device/hisense/a6l/rom/prebuilt/vendor/lib/modules/edt-ft5x06.ko; [ -f $E ] && cp $E $P/recovery-modules/
python3 - $P/recovery-modules <<'PY'
import os, sys
d = sys.argv[1]; have = {f[:-3].replace('-', '_') for f in os.listdir(d)}
for f in sorted(os.listdir(d)):
    b = open(os.path.join(d, f), 'rb').read(); i = b.find(b'\0depends=')
    deps = [x for x in b[i + 9:b.index(b'\0', i + 1)].decode().split(',') if x] if i >= 0 else []
    miss = [x for x in deps if x.replace('-', '_') not in have]
    if miss:
        print(f'drop {f}: needs {miss} (not in the recovery ramdisk)'); os.remove(os.path.join(d, f))
    else:
        print(f'keep {f} deps={deps}')
PY
ls -la $P $P/recovery-modules; echo STAGE_RELEASE_PASS
