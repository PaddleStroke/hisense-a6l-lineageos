#!/usr/bin/env bash
# A6L release signing + OTA package (release-prep, 27 Sep 2026). NOT RUN yet (needs release keys + a release target-files).
# usage (WSL, as a6l):
#   bash tools/release/sign-a6l-release.sh <unsigned-target_files.zip> <boot-dir> <out-dir>
#     <unsigned-target_files.zip> : $OUT/obj/PACKAGING/target_files_intermediates/lineage_gsi_a6l-target_files*.zip
#                                   (A6L_RELEASE=1 build: `m target-files-package otatools`, variant user)
#     <boot-dir>                  : pipeline boot phase output with the REAL A6L boot.img (+ dtbo.img), e.g. /home/a6l/rom-v2/boot-<tag>
#                                   (tools/Prepare-RomV2Boot.py, V67 Image + V75 DTB + first-stage ramdisk with sdhci-msm)
#   env: A6L_KEYDIR (default /home/a6l/.android-certs), ANDROID_PW_FILE (only for password-protected keys)
# Steps: 1 sign every APK/APEX (Lineage wiki 19.1+ method, APEX list taken from the target-files' META/apexkeys.txt, so no
# APEX is left with a test key); 2 put the A6L boot.img into BOOTABLE_IMAGES/ (releasetools prefers it over BOOT/); 3 full
# non-A/B block OTA signed with releasekey (--block, no downgrade); 4 sha256 + updater JSON (make-updater-json.py).
set -euo pipefail
TF=$1; BOOTDIR=$2; OUTD=$3
L=${A6L_TREE:-/home/a6l/android/a6l-lineage24}; K=${A6L_KEYDIR:-/home/a6l/.android-certs}
HOSTBIN=$L/out/host/linux-x86/bin; export PATH=$HOSTBIN:$PATH
for t in sign_target_files_apks ota_from_target_files; do command -v $t > /dev/null || { echo "missing $t: build 'otatools' first (m otatools)"; exit 1; }; done
[ -f "$K/releasekey.pk8" ] || { echo "no release keys in $K (tools/release/gen-release-keys.sh)"; exit 1; }
[ "$(stat -c %a "$K")" = 700 ] || { echo "refusing: $K must be chmod 700"; exit 1; }
[ -f "$BOOTDIR/boot.img" ] || { echo "no $BOOTDIR/boot.img"; exit 1; }
grep -q "androidboot.selinux=permissive" <(strings "$BOOTDIR/boot.img" | head -200) && echo "WARNING: boot.img cmdline still has androidboot.selinux=permissive (ignored by a user build, but rebuild it with A6L_SELINUX=enforcing for clarity)"
mkdir -p "$OUTD"; W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
unzip -p "$TF" META/misc_info.txt | grep -q "^build_type=user$\|ro.build.type=user" || unzip -p "$TF" SYSTEM/build.prop | grep -q "^ro.build.type=user$" || echo "WARNING: target-files is not a 'user' build"

# 1. key mapping
ARGS=()
for apk in com.android.appsearch.apk AdServicesApk FederatedCompute HalfSheetUX HealthConnectBackupRestore HealthConnectController \
           OsuLogin SafetyCenterResources ServiceConnectivityResources ServiceUwbResources ServiceWifiResources \
           TelecomServiceResources TelecomUi WebAppService WifiDialog; do
  n=$apk; case $n in *.apk) ;; *) n=$n.apk;; esac; ARGS+=(--extra_apks "$n=$K/releasekey"); done
missing=0
while read -r name; do
  base=${name%.apex}; base=${base%.capex}
  if [ -f "$K/$base.pk8" ] && [ -f "$K/$base.pem" ]; then
    ARGS+=(--extra_apks "$name=$K/$base" --extra_apex_payload_key "$name=$K/$base.pem")
  else echo "MISSING KEY for APEX $name ($K/$base.{pk8,x509.pem,pem}): A6L_KEYS_ADD_MISSING=1 gen-release-keys.sh --apex-from $TF"; missing=1; fi
done < <(unzip -p "$TF" META/apexkeys.txt | grep -v 'private_key="PRESIGNED"' | sed -n 's/^name="\([^"]*\)".*/\1/p')
[ $missing = 0 ] || exit 1
echo "== sign_target_files_apks (${#ARGS[@]} key args)"
( cd $L && sign_target_files_apks -o -d "$K" "${ARGS[@]}" "$TF" "$W/signed-target_files.zip" ) > "$OUTD/sign.log" 2>&1 || { tail -30 "$OUTD/sign.log"; exit 1; }
# no test key may survive: every non-PRESIGNED apk/apex entry must now point into the release key dir
bad=$(unzip -p "$W/signed-target_files.zip" META/apkcerts.txt | grep -v 'certificate="PRESIGNED"\|certificate="EXTERNAL"' | grep -vc "certificate=\"$K/" || true)
bada=$(unzip -p "$W/signed-target_files.zip" META/apexkeys.txt | grep -v 'private_key="PRESIGNED"' | grep -vc "private_key=\"$K/" || true)
echo "apkcerts not release-signed: $bad ; apexkeys not release-signed: $bada"
[ "$bad" = 0 ] && [ "$bada" = 0 ] || { echo "TEST/UNKNOWN KEYS LEFT (see META/apkcerts.txt / META/apexkeys.txt)"; exit 1; }

# 2. A6L boot image (and dtbo) into the signed target-files
mkdir -p "$W/add/BOOTABLE_IMAGES" "$W/add/IMAGES"
cp "$BOOTDIR/boot.img" "$W/add/BOOTABLE_IMAGES/boot.img"; cp "$BOOTDIR/boot.img" "$W/add/IMAGES/boot.img"
[ -f "$BOOTDIR/dtbo.img" ] && cp "$BOOTDIR/dtbo.img" "$W/add/IMAGES/dtbo.img"
python3 - "$W/signed-target_files.zip" "$W/add" <<'PY'
import sys, zipfile, os, shutil
src, add = sys.argv[1], sys.argv[2]
repl = {os.path.relpath(os.path.join(dp, f), add) for dp, _, fs in os.walk(add) for f in fs}
tmp = src + '.tmp'
with zipfile.ZipFile(src) as zi, zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED, allowZip64=True) as zo:
    for it in zi.infolist():
        if it.filename in repl: continue
        with zi.open(it) as r, zo.open(it, 'w') as w: shutil.copyfileobj(r, w, 1 << 20)
    for n in sorted(repl): zo.write(os.path.join(add, n), n)
os.replace(tmp, src); print('injected:', sorted(repl))
PY
cp "$W/signed-target_files.zip" "$OUTD/"

# 3. OTA (full, block-based, non-A/B, signed with releasekey)
BUILD_ID=$(unzip -p "$OUTD/signed-target_files.zip" SYSTEM/build.prop | sed -n 's/^ro.lineage.version=//p')
OTA="$OUTD/lineage-${BUILD_ID:-24.0-00000000-UNOFFICIAL-a6l}-signed.zip"   # ro.lineage.version = 24.0-<date>-<TYPE>-a6l
( cd $L && ota_from_target_files -k "$K/releasekey" --block "$OUTD/signed-target_files.zip" "$OTA" ) > "$OUTD/ota.log" 2>&1 || { tail -30 "$OUTD/ota.log"; exit 1; }
# 4. checks + checksum + updater entry
unzip -l "$OTA" | grep -E "boot.img|system.new.dat|vendor.new.dat|recovery.img|META-INF/com/android/metadata" || true
unzip -p "$OTA" boot.img | cmp - "$BOOTDIR/boot.img" && echo "OTA boot.img = A6L boot.img" || { echo "OTA boot.img differs from the A6L boot.img"; exit 1; }
( cd "$OUTD" && sha256sum "$(basename "$OTA")" > "$(basename "$OTA").sha256sum" )
python3 "$(dirname "$0")/make-updater-json.py" "$OTA" --url "https://github.com/PaddleStroke/hisense-a6l-lineageos/releases/download/${A6L_RELEASE_TAG:-TAG}/$(basename "$OTA")" > "$OUTD/a6l.json"
echo "A6L_SIGN_DONE ota=$OTA json=$OUTD/a6l.json"
