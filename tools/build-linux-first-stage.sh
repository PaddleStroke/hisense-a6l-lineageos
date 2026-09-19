#!/usr/bin/env bash
# Build an existing Android first-stage init component for boot-path analysis.
# No device access, image signing, or installation on the phone.
set -eo pipefail
source_dir=${A6L_SOURCE_DIR:-"$HOME/android/a6l-lineage24"}
cd "$source_dir"
test -f a6l-source-revisions.xml
python3 - "$HOME/logs/build-probe.log" <<'PY'
from pathlib import Path
import sys
with Path(sys.argv[1]).open('rb') as f:
    f.seek(0, 2)
    f.seek(max(0, f.tell() - 16384))
    if b'#### build completed successfully' not in f.read():
        raise SystemExit('Finish the main system build before starting another build target')
PY
mkdir -p "$HOME/logs"
if [[ -f "$HOME/logs/build-first-stage.log" ]]; then
    cp "$HOME/logs/build-first-stage.log" "$HOME/logs/build-first-stage-$(date +%Y%m%d-%H%M%S).log"
fi
exec > >(tee "$HOME/logs/build-first-stage.log") 2>&1
export A6L_SOONG_GOMEMLIMIT=${A6L_SOONG_GOMEMLIMIT:-36GiB}
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release
lunch lineage_gsi_a6l "$aosp_target_release" userdebug
m -j8 init_first_stage
